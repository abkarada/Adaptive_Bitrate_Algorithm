#pragma once
#include <vector>
#include <string>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <memory>      // <-- eklendi (unique_ptr)
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <thread>
#include <mutex>

#include "udp_port_profiler.h"

// Basit token-bucket (pacing)
struct TokenBucket {
    double cap_bytes = 0;   // burst kapasitesi
    double tokens = 0;      // mevcut jeton
    double fill_Bps = 0;    // doldurma hızı (B/s)
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

// === Tünel ===
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

    // Port profilleri (RTT/loss) güncelle
    void set_profiles(const std::vector<UDPChannelStat>& stats);

    // Pacing toplam hızı (B/s) ayarla
    inline void set_total_rate_bps(int bps) {
        total_rate_bps_ = std::max(100000, bps);      // en az 100 kbps
        const size_t n = tunnels_.size();

        // bucket boyutlarını tünel sayısıyla eşitle
        if (buckets_.size() != n) {
            buckets_.assign(n, TokenBucket{});        // kopyalanabilir -> sorun yok
        }
        if (bucket_mx_.size() != n) {
            // unique_ptr<mutex> hareket ettirilebilir => vector güvenli
            bucket_mx_.resize(n);
            for (size_t i = 0; i < n; ++i) {
                if (!bucket_mx_[i]) bucket_mx_[i] = std::make_unique<std::mutex>();
            }
        }

        // port başına adil paylaşım
        const double per = (double)total_rate_bps_ / std::max<size_t>(1, n);
        for (size_t i = 0; i < n; ++i) {
            buckets_[i].fill_Bps  = per;
            buckets_[i].cap_bytes = per * 0.03;             // ~30ms burst
            buckets_[i].tokens    = buckets_[i].cap_bytes;  // başlangıçta dolu
            buckets_[i].last      = std::chrono::steady_clock::now();
        }
    }

    // Gönderimler
    void send_slices(const std::vector<std::vector<uint8_t>>& chunks);
    void send_slices_parallel(const std::vector<std::vector<uint8_t>>& chunks, int max_threads = 0);

    // İzleme
    void monitor_heartbeat();

    // Slice başına kaç kopya
    void enable_redundancy(int redundancy_count) {
        if (redundancy_count < 1) redundancy_count = 1;
        if (redundancy_count > static_cast<int>(tunnels_.size()))
            redundancy_count = static_cast<int>(tunnels_.size());
        redundancy = redundancy_count;
    }

private:
    std::string remote_ip_;
    std::vector<Tunnel> tunnels_;

    // pacing
    std::vector<TokenBucket> buckets_;
    std::vector<std::unique_ptr<std::mutex>> bucket_mx_; // <-- std::mutex yerine
    int total_rate_bps_ = 5'000'000; // 5 Mbps varsayılan

    size_t rr_index_ = 0;
    int redundancy = 1;
    std::mutex send_mtx_;

    int select_best_port_index();
    int select_weighted_port_index(const std::vector<int>& exclude_indices, bool favor_diversity);
    void send_slice(const std::vector<uint8_t>& slice, int tunnel_index);
    void scatter_slices(const std::vector<std::vector<uint8_t>>& chunks);
};

// === CONSTRUCTOR ===
inline AdaptiveUDPSender::AdaptiveUDPSender(const std::string& ip, const std::vector<uint16_t>& remote_ports)
    : remote_ip_(ip) {

    for (uint16_t port : remote_ports) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Socket creation failed for port: " << port << std::endl;
            continue;
        }

        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = 0;
        if (bind(sock, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0) {
            std::cerr << "Bind failed on port " << port << std::endl;
            close(sock);
            continue;
        }

        int sndbuf = 16 * 1024 * 1024; // 16MB
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        int tos = 0xB8; // DSCP EF
        setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        int prio = 6;
        setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof(prio));

        tunnels_.emplace_back(sock, port);
    }

    // pacing yapıları
    bucket_mx_.resize(tunnels_.size());
    for (size_t i = 0; i < bucket_mx_.size(); ++i)
        bucket_mx_[i] = std::make_unique<std::mutex>();
    buckets_.assign(tunnels_.size(), TokenBucket{});
    set_total_rate_bps(total_rate_bps_);
}

inline void AdaptiveUDPSender::set_profiles(const std::vector<UDPChannelStat>& stats) {
    if (stats.size() != tunnels_.size()) {
        std::cerr << "Profile size mismatch!\n";
        return;
    }
    for (size_t i = 0; i < stats.size(); ++i)
        tunnels_[i].stat = stats[i];
}

// Pacing + retry’li gönderim
inline void AdaptiveUDPSender::send_slice(const std::vector<uint8_t>& slice, int tunnel_index) {
    if (tunnel_index < 0 || (size_t)tunnel_index >= tunnels_.size()) return;

    // pacing: portun bucket’ından izin çıkana kadar bekle
    for (;;) {
        bool ok = false;
        {
            auto& mtxPtr = bucket_mx_[static_cast<size_t>(tunnel_index)];
            std::lock_guard<std::mutex> lk(*mtxPtr);
            ok = buckets_[static_cast<size_t>(tunnel_index)].allow(slice.size());
        }
        if (ok) break;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(tunnels_[tunnel_index].remote_port);
    inet_pton(AF_INET, remote_ip_.c_str(), &dest.sin_addr);

    for (int attempt = 0; attempt < 3; ++attempt) {
        ssize_t sent = sendto(tunnels_[tunnel_index].socket_fd, slice.data(), slice.size(), 0,
                              reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
        if (sent == static_cast<ssize_t>(slice.size())) return;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

inline int AdaptiveUDPSender::select_best_port_index() {
    int best_index = 0;
    double best_score = 1e9;
    for (size_t i = 0; i < tunnels_.size(); ++i) {
        const auto& stat = tunnels_[i].stat;
        double score = stat.avg_rtt_ms + 1000.0 * stat.packet_loss;
        if (score < best_score) { best_score = score; best_index = (int)i; }
    }
    return best_index;
}

inline int AdaptiveUDPSender::select_weighted_port_index(const std::vector<int>& exclude_indices, bool /*favor_diversity*/) {
    if (tunnels_.empty()) return -1;
    constexpr double epsilon = 1e-3;
    constexpr double alpha = 2.0;
    constexpr double beta = 0.01;

    std::vector<double> weights(tunnels_.size(), 0.0);
    double sum_w = 0.0;
    for (size_t i = 0; i < tunnels_.size(); ++i) {
        if (std::find(exclude_indices.begin(), exclude_indices.end(), (int)i) != exclude_indices.end())
            continue;
        const auto& s = tunnels_[i].stat;
        double denom = epsilon + alpha * s.packet_loss + beta * std::max(0.0, s.avg_rtt_ms);
        double w = (denom > 0.0) ? (1.0 / denom) : 0.0;
        weights[i] = w; sum_w += w;
    }
    if (sum_w <= 0.0) {
        for (size_t i = 0; i < tunnels_.size(); ++i)
            if (std::find(exclude_indices.begin(), exclude_indices.end(), (int)i) == exclude_indices.end())
                return (int)i;
        return 0;
    }
    size_t start = rr_index_ % tunnels_.size();
    double pick = ((double)rand() / (double)RAND_MAX) * sum_w, acc = 0.0;
    for (size_t of = 0; of < tunnels_.size(); ++of) {
        size_t i = (start + of) % tunnels_.size();
        if (std::find(exclude_indices.begin(), exclude_indices.end(), (int)i) != exclude_indices.end()) continue;
        acc += weights[i];
        if (pick <= acc) { rr_index_ = (i + 1) % tunnels_.size(); return (int)i; }
    }
    return select_best_port_index();
}

inline void AdaptiveUDPSender::send_slices(const std::vector<std::vector<uint8_t>>& chunks) {
    struct SliceHeaderLocal {
        uint32_t magic, frame_id; uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes;
        uint32_t total_frame_bytes; uint64_t timestamp_us; uint8_t flags; uint32_t checksum;
    };
    if (tunnels_.empty() || chunks.empty()) return;
    size_t port_count = tunnels_.size();

    for (const auto& slice : chunks) {
        uint16_t slice_index = 0;
        uint8_t flags = 0;
        if (slice.size() >= sizeof(SliceHeaderLocal)) {
            SliceHeaderLocal hdr{}; std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
            slice_index = hdr.slice_index; flags = hdr.flags;
        }
        bool is_parity = (flags & 0x01) != 0;
        int clones = is_parity ? 1 : std::min<int>(redundancy, (int)port_count);

        int primary = (int)(slice_index % port_count);
        send_slice(slice, primary);
        for (int c = 1; c < clones; ++c) {
            int next = (primary + c) % (int)port_count;
            send_slice(slice, next);
        }
    }
}

inline void AdaptiveUDPSender::send_slices_parallel(const std::vector<std::vector<uint8_t>>& chunks, int max_threads) {
    if (chunks.empty()) return;
    size_t nthreads = tunnels_.empty() ? 1 : tunnels_.size();
    if (max_threads > 0) nthreads = std::min(nthreads, (size_t)max_threads);
    if (nthreads <= 1) { send_slices(chunks); return; }

    auto worker = [&](size_t start_idx) {
        struct SliceHeaderLocal {
            uint32_t magic, frame_id; uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes;
            uint32_t total_frame_bytes; uint64_t ts; uint8_t flags; uint32_t checksum;
        };
        for (size_t i = start_idx; i < chunks.size(); i += nthreads) {
            const auto& slice = chunks[i];
            uint16_t slice_index = 0; uint8_t flags = 0;
            if (slice.size() >= sizeof(SliceHeaderLocal)) {
                SliceHeaderLocal hdr{}; std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
                slice_index = hdr.slice_index; flags = hdr.flags;
            }
            bool is_parity = (flags & 0x01) != 0;
            int primary = (int)(slice_index % tunnels_.size());
            int clones  = is_parity ? 1 : std::min<int>(redundancy, (int)tunnels_.size());

            send_slice(slice, primary);
            for (int c = 1; c < clones; ++c) {
                int next = (primary + c) % (int)tunnels_.size();
                send_slice(slice, next);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (size_t t = 0; t < nthreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();
}

inline void AdaptiveUDPSender::scatter_slices(const std::vector<std::vector<uint8_t>>& chunks) {
    if (tunnels_.empty()) return;
    struct SliceHeaderLocal { uint32_t magic, frame_id; uint16_t slice_index, total_slices, k_data, r_parity, payload_bytes; uint32_t total_frame_bytes; uint64_t ts; uint8_t flags; uint32_t checksum; };
    for (const auto& slice : chunks) {
        int port_index = select_best_port_index();
        if (slice.size() >= sizeof(SliceHeaderLocal)) {
            SliceHeaderLocal hdr{}; std::memcpy(&hdr, slice.data(), sizeof(SliceHeaderLocal));
            if (!tunnels_.empty()) port_index = (int)(hdr.slice_index % tunnels_.size());
        }
        send_slice(slice, port_index);
    }
}

inline void AdaptiveUDPSender::monitor_heartbeat() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); return; }
    for (auto& tunnel : tunnels_) {
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = tunnel.socket_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tunnel.socket_fd, &ev) < 0) perror("epoll_ctl");
    }
    epoll_event events[10];
    while (true) {
        int nfds = epoll_wait(epoll_fd, events, 10, 1000);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd; char buf[64];
            recv(fd, buf, sizeof(buf), 0);
            for (auto& t : tunnels_) if (t.socket_fd == fd) { t.last_alive = std::chrono::steady_clock::now(); break; }
        }
        for (auto& t : tunnels_) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - t.last_alive).count() > 3) {
                std::cerr << "Tunnel on port " << t.remote_port << " seems dead.\n";
            }
        }
    }
    close(epoll_fd);
}
