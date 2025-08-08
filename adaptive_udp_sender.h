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
    void monitor_heartbeat();
    void enable_redundancy(int redundancy_count); // slice başına kaç clone gönderilecek?

private:
    std::string remote_ip_;
    std::vector<Tunnel> tunnels_;
    size_t rr_index_ = 0;
    int redundancy = 1; // her slice kaç porttan gidecek (1 = clone yok, 2 = clone)

    int select_best_port_index();
    void send_slice(const std::vector<uint8_t>& slice, int tunnel_index);
    int select_weighted_port_index(const std::vector<int>& exclude_indices, bool favor_diversity);
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

    sendto(tunnels_[tunnel_index].socket_fd, slice.data(), slice.size(), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
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
    // We will parse SliceHeader flags to diversify data vs parity placement
    struct SliceHeaderLocal {
        uint32_t magic;
        uint32_t frame_id;
        uint16_t slice_index;
        uint16_t total_slices;
        uint16_t k_data;
        uint16_t r_parity;
        uint32_t total_frame_bytes;
        uint64_t timestamp_us;
        uint8_t  flags;
    };

    for (const auto& slice : chunks) {
        bool is_parity = false;
        if (slice.size() >= sizeof(SliceHeaderLocal)) {
            SliceHeaderLocal hdr{};
            std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
            is_parity = (hdr.flags & 0x01) != 0;
        }

        std::vector<int> used_ports;
        int clones = std::min<int>(redundancy, static_cast<int>(tunnels_.size()));
        for (int i = 0; i < clones; ++i) {
            int port_index = select_weighted_port_index(used_ports, /*favor_diversity*/ true);
            if (port_index < 0) port_index = select_best_port_index();

            // Ensure data/parity are diversified across different paths
            // If parity, try to avoid the absolute best port to keep independence
            if (is_parity && !tunnels_.empty()) {
                // If chosen is same as best, shift by 1
                int best = select_best_port_index();
                if (port_index == best) {
                    port_index = (port_index + 1) % static_cast<int>(tunnels_.size());
                    // avoid duplicates
                    while (std::find(used_ports.begin(), used_ports.end(), port_index) != used_ports.end()) {
                        port_index = (port_index + 1) % static_cast<int>(tunnels_.size());
                    }
                }
            }

            send_slice(slice, port_index);
            used_ports.push_back(port_index);
        }
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
