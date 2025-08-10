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
#include <sys/uio.h>
#include <thread>
#include <mutex>

#include "udp_port_profiler.h"

// ---------------- Token Bucket ----------------
struct TokenBucket {
    double cap_bytes = 0;                 // örn. 2 * fill_Bps
    double tokens    = 0;
    double fill_Bps  = 0;                 // bayt/saniye
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    bool allow(size_t bytes) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        tokens = std::min(cap_bytes, tokens + dt * fill_Bps);
        if (tokens >= (double)bytes) { tokens -= (double)bytes; return true; }
        return false;
    }
};

// ---------------- Tunnel ----------------
struct Tunnel {
    int socket_fd;
    uint16_t remote_port;
    UDPChannelStat stat;
    std::chrono::steady_clock::time_point last_alive;

    Tunnel(int fd, uint16_t port)
        : socket_fd(fd), remote_port(port), last_alive(std::chrono::steady_clock::now()) {}
};

// ---------------- AdaptiveUDPSender ----------------
class AdaptiveUDPSender {
public:
    AdaptiveUDPSender(const std::string& ip, const std::vector<uint16_t>& remote_ports);

    void set_profiles(const std::vector<UDPChannelStat>& stats);

    // chunks: her eleman tek bir UDP paketi (header + payload)
    void send_slices(const std::vector<std::vector<uint8_t>>& chunks);
    void send_slices_parallel(const std::vector<std::vector<uint8_t>>& chunks, int max_threads = 0);

    void monitor_heartbeat();
    void enable_redundancy(int redundancy_count); // slice başına kaç clone gönderilecek?

    // toplam hedef bitrate (bps) -> portlara dağıtılır
    void set_total_rate_bps(int bps);

    // pacing yapıları (port sayısı kadar)
    std::vector<TokenBucket> buckets_;
    std::vector<std::mutex>  bucket_mx_;
    int total_rate_bps_ = 5'000'000; // default 5 Mbps

private:
    std::string remote_ip_;
    std::vector<Tunnel> tunnels_;
    size_t rr_index_ = 0;
    int redundancy = 1; // 1 = clone yok, 2 = bir clone vb.
    std::mutex send_mtx_;

    int  select_best_port_index();
    int  select_weighted_port_index(const std::vector<int>& exclude_indices, bool favor_diversity);
    int  select_clone_port(int primary); // primary'den farklı en iyi port

    void send_slice(const std::vector<uint8_t>& slice, int tunnel_index);

    // (şu an kullanılmıyor ama dursun) rr dağıtım
    void scatter_slices(const std::vector<std::vector<uint8_t>>& chunks);
};

// --------------- Impl ----------------

AdaptiveUDPSender::AdaptiveUDPSender(const std::string& ip, const std::vector<uint16_t>& remote_ports)
    : remote_ip_(ip)
{
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

        // Büyük send buffer + öncelik
        int sndbuf = 16 * 1024 * 1024; // 16MB buffer
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        int tos = 0xB8; // DSCP EF (Expedited Forwarding)
        setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        int prio = 6; // kernel socket priority (0-6 typical)
        setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));

        tunnels_.emplace_back(sock, port);
    }

    // pacing bucket'ları init
    buckets_.resize(tunnels_.size());
    bucket_mx_.resize(tunnels_.size());
    double per_port_Bps = (double)total_rate_bps_ / 8.0 / std::max<size_t>(1, tunnels_.size());
    for (auto& b : buckets_) {
        b.fill_Bps  = per_port_Bps;
        b.cap_bytes = per_port_Bps * 2.0;   // 2 saniyelik kapasite
        b.tokens    = b.cap_bytes;
        b.last      = std::chrono::steady_clock::now();
    }
}

void AdaptiveUDPSender::set_total_rate_bps(int bps) {
    total_rate_bps_ = std::max(100000, bps); // min 100 kbps
    if (tunnels_.empty()) return;
    double per_port_Bps = (double)total_rate_bps_ / 8.0 / std::max<size_t>(1, tunnels_.size());
    for (size_t i = 0; i < buckets_.size(); ++i) {
        auto& b = buckets_[i];
        std::lock_guard<std::mutex> lk(bucket_mx_[i]);
        b.fill_Bps  = per_port_Bps;
        b.cap_bytes = per_port_Bps * 2.0;
        b.tokens    = std::min(b.tokens, b.cap_bytes);
        b.last      = std::chrono::steady_clock::now();
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
    if (tunnel_index < 0 || (size_t)tunnel_index >= tunnels_.size()) return;

    // ---- Per-port pacing ----
    size_t need = slice.size();
    bool allowed = false;
    for (int spins = 0; spins < 200; ++spins) { // ~40 ms üst limit (200 * 200us)
        {
            std::lock_guard<std::mutex> lk(bucket_mx_[tunnel_index]);
            if (buckets_[tunnel_index].allow(need)) { allowed = true; }
        }
        if (allowed) break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // allowed olmasa da dene: en kötü kernel düşürür
    (void)allowed;

    // ---- sendto (kısa retry) ----
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(tunnels_[tunnel_index].remote_port);
    inet_pton(AF_INET, remote_ip_.c_str(), &dest.sin_addr);

    for (int attempt = 0; attempt < 3; ++attempt) {
        ssize_t sent = sendto(tunnels_[tunnel_index].socket_fd,
                              slice.data(), slice.size(), 0,
                              reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent == (ssize_t)slice.size()) return;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

int AdaptiveUDPSender::select_best_port_index() {
    if (tunnels_.empty()) return -1;
    int best_index = 0;
    double best_score = 1e9;
    for (size_t i = 0; i < tunnels_.size(); ++i) {
        const auto& s = tunnels_[i].stat;
        double score = s.avg_rtt_ms + 1000.0 * s.packet_loss; // loss ağır bastın
        if (score < best_score) { best_score = score; best_index = (int)i; }
    }
    return best_index;
}

int AdaptiveUDPSender::select_weighted_port_index(const std::vector<int>& exclude_indices, bool /*favor_diversity*/) {
    if (tunnels_.empty()) return -1;

    constexpr double epsilon = 1e-3;
    constexpr double alpha = 2.0;   // loss ağırlığı
    constexpr double beta  = 0.01;  // RTT ağırlığı

    std::vector<double> weights(tunnels_.size(), 0.0);
    double sum_w = 0.0;

    for (size_t i = 0; i < tunnels_.size(); ++i) {
        if (std::find(exclude_indices.begin(), exclude_indices.end(), (int)i) != exclude_indices.end())
            continue;
        const auto& s = tunnels_[i].stat;
        double denom = epsilon + alpha * s.packet_loss + beta * std::max(0.0, s.avg_rtt_ms);
        double w = (denom > 0.0) ? (1.0 / denom) : 0.0;
        weights[i] = w;
        sum_w += w;
    }

    if (sum_w <= 0.0) {
        // fallback: sıradaki uygun portu seç
        for (size_t i = 0; i < tunnels_.size(); ++i) {
            if (std::find(exclude_indices.begin(), exclude_indices.end(), (int)i) == exclude_indices.end())
                return (int)i;
        }
        return 0;
    }

    size_t start = rr_index_ % std::max<size_t>(1, tunnels_.size());
    double pick = ((double)rand() / (double)RAND_MAX) * sum_w;
    double acc  = 0.0;
    for (size_t of = 0; of < tunnels_.size(); ++of) {
        size_t i = (start + of) % tunnels_.size();
        if (std::find(exclude_indices.begin(), exclude_indices.end(), (int)i) != exclude_indices.end())
            continue;
        acc += weights[i];
        if (pick <= acc) {
            rr_index_ = (i + 1) % tunnels_.size();
            return (int)i;
        }
    }
    return select_best_port_index();
}

int AdaptiveUDPSender::select_clone_port(int primary) {
    // primary dışında en iyi portu seç
    if (tunnels_.empty()) return -1;
    std::vector<int> exclude = { primary };
    int idx = select_weighted_port_index(exclude, /*favor_diversity=*/true);
    if (idx < 0) {
        // basit fallback
        idx = (primary + 1) % std::max<size_t>(1, tunnels_.size());
        if (idx == primary && tunnels_.size() > 1)
            idx = (primary + 2) % tunnels_.size();
    }
    return idx;
}

void AdaptiveUDPSender::send_slices(const std::vector<std::vector<uint8_t>>& chunks) {
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
        uint8_t  flags;     // bit0: parity
        uint32_t checksum;
    };

    if (tunnels_.empty() || chunks.empty()) return;

    for (const auto& slice : chunks) {
        // parity mi?
        uint8_t flags = 0;
        if (slice.size() >= sizeof(SliceHeaderLocal)) {
            SliceHeaderLocal hdr{};
            std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
            flags = hdr.flags;
        }
        bool is_parity = (flags & 0x01);

        // primary & clone seçimi
        int clones = is_parity ? 1 : std::min<int>(redundancy, (int)tunnels_.size());

        int primary = select_weighted_port_index(/*exclude*/{}, /*favor_diversity*/true);
        if (primary < 0) primary = select_best_port_index();
        if (primary < 0) return;

        send_slice(slice, primary);

        // Data için ek clone'lar
        if (!is_parity) {
            std::vector<int> chosen{ primary };
            for (int c = 1; c < clones; ++c) {
                int next = select_weighted_port_index(chosen, /*favor_diversity*/true);
                if (next < 0 || std::find(chosen.begin(), chosen.end(), next) != chosen.end()) break;
                chosen.push_back(next);
                send_slice(slice, next);
            }
        }
    }
}

void AdaptiveUDPSender::send_slices_parallel(const std::vector<std::vector<uint8_t>>& chunks, int max_threads) {
    if (chunks.empty()) return;
    size_t nthreads = tunnels_.empty() ? 1 : tunnels_.size();
    if (max_threads > 0) nthreads = std::min<size_t>(nthreads, (size_t)max_threads);
    if (nthreads <= 1) { send_slices(chunks); return; }

    auto worker = [&](size_t start_idx) {
        struct SliceHeaderLocal {
            uint32_t magic, frame_id;
            uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes;
            uint32_t total_frame_bytes;
            uint64_t timestamp_us;
            uint8_t  flags; // bit0 parity
            uint32_t checksum;
        };

        for (size_t i = start_idx; i < chunks.size(); i += nthreads) {
            const auto& slice = chunks[i];

            uint8_t flags = 0;
            if (slice.size() >= sizeof(SliceHeaderLocal)) {
                SliceHeaderLocal hdr{};
                std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
                flags = hdr.flags;
            }
            bool is_parity = (flags & 0x01);

            int clones_local;
            {
                std::lock_guard<std::mutex> lk(send_mtx_);
                clones_local = is_parity ? 1 : std::min<int>(redundancy, (int)tunnels_.size());
            }

            int primary = select_weighted_port_index(/*exclude*/{}, /*favor_diversity*/true);
            if (primary < 0) primary = select_best_port_index();
            if (primary < 0) continue;

            send_slice(slice, primary);

            if (!is_parity) {
                std::vector<int> chosen{ primary };
                for (int c = 1; c < clones_local; ++c) {
                    int next = select_weighted_port_index(chosen, /*favor_diversity*/true);
                    if (next < 0 || std::find(chosen.begin(), chosen.end(), next) != chosen.end()) break;
                    chosen.push_back(next);
                    send_slice(slice, next);
                }
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (size_t t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
}

// Basit scatter (şu an kullanılmıyor)
void AdaptiveUDPSender::scatter_slices(const std::vector<std::vector<uint8_t>>& chunks) {
    if (tunnels_.empty()) return;
    struct SliceHeaderLocal {
        uint32_t magic, frame_id;
        uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes;
        uint32_t total_frame_bytes; uint64_t ts; uint8_t flags; uint32_t checksum;
    };
    for (const auto& slice : chunks) {
        int port_index = select_best_port_index();
        if (port_index < 0) port_index = 0;
        send_slice(slice, port_index);
    }
}

void AdaptiveUDPSender::enable_redundancy(int redundancy_count) {
    if (redundancy_count < 1) redundancy_count = 1;
    if (redundancy_count > (int)tunnels_.size()) redundancy_count = (int)tunnels_.size();
    redundancy = redundancy_count;
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
