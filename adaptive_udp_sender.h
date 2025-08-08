#pragma once
#include <vector>
#include <string>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <thread>
#include <mutex>
#include "udp_port_profiler.h"


struct Tunnel {
    int socket_fd;
    uint16_t remote_port;
    UDPChannelStat stat;
    std::chrono::steady_clock::time_point last_alive;

    Tunnel(int fd, uint16_t port)
        : socket_fd(fd), remote_port(port), last_alive(std::chrono::steady_clock::now()) {}
};
class AdaptiveUDPSender {
public:
    AdaptiveUDPSender(const std::string& ip, const std::vector<uint16_t>& remote_ports);
    void set_profiles(const std::vector<UDPChannelStat>& stats);
    void send_slices(const std::vector<std::vector<uint8_t>>& chunks);
    void send_slices_parallel(const std::vector<std::vector<uint8_t>>& chunks, int max_threads = 0);
    void monitor_heartbeat();
    void enable_redundancy(int redundancy_count); // slice başına kaç clone gönderilecek?

private:
    std::string remote_ip_;
    std::vector<Tunnel> tunnels_;
    size_t rr_index_ = 0;
    int redundancy = 1; // her slice kaç porttan gidecek (1 = clone yok, 2 = clone)
    std::mutex send_mtx_;

    int select_best_port_index();
    void send_slice(const std::vector<uint8_t>& slice, int tunnel_index);
    int select_weighted_port_index(const std::vector<int>& exclude_indices, bool favor_diversity);
    // Scatter: farklı slice gruplarını farklı tünellere paylaştır
    void scatter_slices(const std::vector<std::vector<uint8_t>>& chunks);
};

AdaptiveUDPSender::AdaptiveUDPSender(const std::string& ip, const std::vector<uint16_t>& remote_ports)
    : remote_ip_(ip) {

    for (uint16_t port : remote_ports) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Socket creation failed for port: " << port << std::endl;
            continue;
        }

        // OS port seçsin, bind et ama sabit port verme
        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = 0;

        if (bind(sock, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            std::cerr << "Bind failed on port " << port << std::endl;
            close(sock);
            continue;
        }

        // Çok büyük gönderim tamponu - packet loss'u önlemek için
        int sndbuf = 16 * 1024 * 1024; // 16MB buffer
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        int tos = 0x10; // IPTOS_LOWDELAY
        setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        tunnels_.emplace_back(sock, port);
    }
}

void AdaptiveUDPSender::set_profiles(const std::vector<UDPChannelStat>& stats) {
    if (stats.size() != tunnels_.size()) {
        std::cerr << "Profile size mismatch!" << std::endl;
        return;
    }

    for (size_t i = 0; i < stats.size(); ++i) {
        tunnels_[i].stat = stats[i];
    }
}

void AdaptiveUDPSender::send_slice(const std::vector<uint8_t>& slice, int tunnel_index) {
    if (tunnel_index >= tunnels_.size()) return;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(tunnels_[tunnel_index].remote_port);
    inet_pton(AF_INET, remote_ip_.c_str(), &dest.sin_addr);

    // Retry kısa beklemelerle: ENOBUFS/EAGAIN durumunda düşürmeyelim
    for (int attempt = 0; attempt < 3; ++attempt) {
        ssize_t sent = sendto(tunnels_[tunnel_index].socket_fd, slice.data(), slice.size(), 0,
                              reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent == static_cast<ssize_t>(slice.size())) return;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

int AdaptiveUDPSender::select_best_port_index() {
    int best_index = 0;
    double best_score = 1e9;

    for (size_t i = 0; i < tunnels_.size(); ++i) {
        const auto& stat = tunnels_[i].stat;
        // RTT + loss_weight penalization
double score = stat.avg_rtt_ms + 1000.0 * stat.packet_loss;
        if (score < best_score) {
            best_score = score;
            best_index = static_cast<int>(i);
        }
    }

    return best_index;
}

int AdaptiveUDPSender::select_weighted_port_index(const std::vector<int>& exclude_indices, bool favor_diversity) {
    if (tunnels_.empty()) return -1;

    // Compute weights inversely proportional to (loss, rtt)
    // weight = 1 / (epsilon + alpha*loss + beta*rtt_ms)
    constexpr double epsilon = 1e-3;
    constexpr double alpha = 2.0;   // loss emphasis
    constexpr double beta = 0.01;   // RTT emphasis

    std::vector<double> weights(tunnels_.size(), 0.0);
    double sum_w = 0.0;
    for (size_t i = 0; i < tunnels_.size(); ++i) {
        if (std::find(exclude_indices.begin(), exclude_indices.end(), static_cast<int>(i)) != exclude_indices.end()) continue;
        const auto& s = tunnels_[i].stat;
        double denom = epsilon + alpha * s.packet_loss + beta * std::max(0.0, s.avg_rtt_ms);
        double w = (denom > 0.0) ? (1.0 / denom) : 0.0;
        weights[i] = w;
        sum_w += w;
    }

    if (sum_w <= 0.0) {
        // fallback: round-robin avoiding excludes
        for (size_t i = 0; i < tunnels_.size(); ++i) {
            if (std::find(exclude_indices.begin(), exclude_indices.end(), static_cast<int>(i)) == exclude_indices.end()) {
                return static_cast<int>(i);
            }
        }
        return 0;
    }

    // Favor diversity: slightly boost least-used index using simple rr pointer
    // We will start sampling from rr_index_ to promote spread
    size_t start = rr_index_ % tunnels_.size();
    double pick = ((double)rand() / (double)RAND_MAX) * sum_w;
    double acc = 0.0;
    for (size_t of = 0; of < tunnels_.size(); ++of) {
        size_t i = (start + of) % tunnels_.size();
        if (std::find(exclude_indices.begin(), exclude_indices.end(), static_cast<int>(i)) != exclude_indices.end()) continue;
        acc += weights[i];
        if (pick <= acc) {
            rr_index_ = (i + 1) % tunnels_.size();
            return static_cast<int>(i);
        }
    }
    return select_best_port_index();
}

void AdaptiveUDPSender::send_slices(const std::vector<std::vector<uint8_t>>& chunks) {
    // Weighted best-port strategy:
    // - Data slices: prefer the best tunnel(s)
    // - Parity slices: diversify onto different tunnels to maximize independence
    // - Redundancy > 1: send clones to next-best distinct tunnels
    struct SliceHeaderLocal {
        uint32_t magic;
        uint32_t frame_id;
        uint16_t slice_index;
        uint16_t total_slices;
        uint16_t k_data;
        uint16_t r_parity;
        uint16_t payload_bytes;
        uint32_t total_frame_bytes;
        uint64_t timestamp_us;
        uint8_t  flags;
        uint32_t checksum;
    };

    if (tunnels_.empty() || chunks.empty()) return;

    for (const auto& slice : chunks) {
        bool is_parity = false;
        if (slice.size() >= sizeof(SliceHeaderLocal)) {
            SliceHeaderLocal hdr{};
            std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
            is_parity = (hdr.flags & 0x01) != 0;
        }

        std::vector<int> used_ports;
        int clones = std::min<int>(redundancy, static_cast<int>(tunnels_.size()));

        for (int c = 0; c < clones; ++c) {
            int port_index = -1;
            if (!is_parity) {
                // Data: prefer best tunnel first, then next-best excluding already used
                if (c == 0) {
                    port_index = select_best_port_index();
                    // avoid duplicates
                    if (std::find(used_ports.begin(), used_ports.end(), port_index) != used_ports.end()) {
                        port_index = select_weighted_port_index(used_ports, /*favor_diversity*/ true);
                    }
                } else {
                    port_index = select_weighted_port_index(used_ports, /*favor_diversity*/ false);
                }
            } else {
                // Parity: diversify away from the very best path
                int best = select_best_port_index();
                port_index = select_weighted_port_index(used_ports, /*favor_diversity*/ true);
                if (port_index == best) {
                    // shift to a different tunnel to keep independence
                    port_index = (port_index + 1) % static_cast<int>(tunnels_.size());
                    // avoid duplicates
                    while (std::find(used_ports.begin(), used_ports.end(), port_index) != used_ports.end()) {
                        port_index = (port_index + 1) % static_cast<int>(tunnels_.size());
                    }
                }
            }

            if (port_index < 0) port_index = select_best_port_index();
            send_slice(slice, port_index);
            used_ports.push_back(port_index);
        }
    }
}
void AdaptiveUDPSender::send_slices_parallel(const std::vector<std::vector<uint8_t>>& chunks, int max_threads) {
    if (chunks.empty()) return;
    size_t nthreads = tunnels_.empty() ? 1 : std::min<size_t>(tunnels_.size(), 4);
    if (max_threads > 0) nthreads = std::min<size_t>(nthreads, static_cast<size_t>(max_threads));
    if (nthreads <= 1) { send_slices(chunks); return; }

    auto worker = [&](size_t start_idx) {
        struct SliceHeaderLocal {
            uint32_t magic, frame_id; uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes; uint32_t total_frame_bytes; uint64_t ts; uint8_t flags; uint32_t checksum;
        };
        for (size_t i = start_idx; i < chunks.size(); i += nthreads) {
            const auto& slice = chunks[i];
            bool is_parity = false;
            if (slice.size() >= sizeof(SliceHeaderLocal)) {
                SliceHeaderLocal hdr{}; std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
                is_parity = (hdr.flags & 0x01) != 0;
            }
            std::vector<int> used_ports;
            int clones = 1;
            {
                std::lock_guard<std::mutex> lk(send_mtx_);
                clones = std::min<int>(redundancy, static_cast<int>(tunnels_.size()));
            }
            for (int c = 0; c < clones; ++c) {
                int port_index;
                {
                    std::lock_guard<std::mutex> lk(send_mtx_);
                    port_index = select_weighted_port_index(used_ports, /*favor_diversity*/ true);
                    if (port_index < 0) port_index = select_best_port_index();
                    if (is_parity && !tunnels_.empty()) {
                        int best = select_best_port_index();
                        if (port_index == best) port_index = (port_index + 1) % static_cast<int>(tunnels_.size());
                    }
                }
                send_slice(slice, port_index);
                used_ports.push_back(port_index);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (size_t t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
}

// Basit scatter: slice_index mod tunnel_count ile dağıt, ağırlıklandırmayla ayarla
void AdaptiveUDPSender::scatter_slices(const std::vector<std::vector<uint8_t>>& chunks) {
    if (tunnels_.empty()) return;
    struct SliceHeaderLocal { uint32_t magic, frame_id; uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes; uint32_t total_frame_bytes; uint64_t ts; uint8_t flags; uint32_t checksum; };
    for (const auto& slice : chunks) {
        int port_index = select_best_port_index();
        if (slice.size() >= sizeof(SliceHeaderLocal)) {
            SliceHeaderLocal hdr{}; std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
            // slice index’e göre round-robin kaydırma
            if (!tunnels_.empty()) port_index = static_cast<int>(hdr.slice_index % tunnels_.size());
        }
        send_slice(slice, port_index);
    }
}

void AdaptiveUDPSender::enable_redundancy(int redundancy_count) {
    if (redundancy_count < 1) redundancy_count = 1;
    if (redundancy_count > static_cast<int>(tunnels_.size())) redundancy_count = static_cast<int>(tunnels_.size());
    // Tek tünelde clone göndermek aynı yola yük bindirir; kapat
    if (tunnels_.size() <= 1) redundancy = 1; else redundancy = redundancy_count;
}

void AdaptiveUDPSender::monitor_heartbeat() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return;
    }

    for (auto& tunnel : tunnels_) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = tunnel.socket_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tunnel.socket_fd, &ev) < 0) {
            perror("epoll_ctl");
            continue;
        }
    }

    epoll_event events[10];
    while (true) {
        int nfds = epoll_wait(epoll_fd, events, 10, 1000); // 1 saniye timeout
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            char buf[64];
            recv(fd, buf, sizeof(buf), 0); // heartbeat mesajı geldi
            for (auto& tunnel : tunnels_) {
                if (tunnel.socket_fd == fd) {
                    tunnel.last_alive = std::chrono::steady_clock::now();
                    break;
                }
            }
        }

        // Her 3 saniyede bir tünel kontrolü
        for (auto& tunnel : tunnels_) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - tunnel.last_alive).count() > 3) {
                std::cerr << "Tunnel on port " << tunnel.remote_port << " seems dead." << std::endl;
            }
        }
    }

    close(epoll_fd);
}
