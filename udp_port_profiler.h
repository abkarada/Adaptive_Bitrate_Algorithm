#pragma once
#include <vector>
#include <string>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <thread>

// === PORT PROFİLİ ===
struct UDPChannelStat {
    uint16_t port;
    double avg_rtt_ms = 10.0;
    double packet_loss = 0.0;
    size_t sent = 0;
    size_t received = 0;

    void update(bool success, double rtt_ms) {
        // 'sent' bu turda probe yollandığını temsil eder
        sent++;
        if (success) {
            received++;
            avg_rtt_ms = 0.8 * avg_rtt_ms + 0.2 * rtt_ms;
        }
        if (sent > 0) {
            packet_loss = 1.0 - (double(received) / (double)sent);
        }
    }
};

// === PROBE VERİ YAPISI ===
#pragma pack(push, 1)
struct UdpProbe {
    uint32_t magic = 0xDEADBEEF;
    uint16_t port = 0;
    uint64_t timestamp_us = 0;

    void fill(uint16_t p) {
        std::memset(this, 0, sizeof(*this));
        magic = 0xDEADBEEF;
        port = p;
        auto now = std::chrono::high_resolution_clock::now();
        timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           now.time_since_epoch()).count();
    }
    bool validate() const { return magic == 0xDEADBEEF; }
};
#pragma pack(pop)
static_assert(sizeof(UdpProbe) == 14, "UdpProbe boyutu beklenmedik!");

// === PROFİLLEYİCİ SINIF ===
class UDPPortProfiler {
public:
    UDPPortProfiler(const std::string& ip, const std::vector<uint16_t>& ports);
    ~UDPPortProfiler();

    void send_probes();
    void receive_replies_epoll(int timeout_ms = 1000);
    const std::vector<UDPChannelStat>& get_stats() const;

private:
    std::string target_ip_;
    std::vector<UDPChannelStat> stats_;
    std::vector<int> sockets_;
};

// === CONSTRUCTOR ===
UDPPortProfiler::UDPPortProfiler(const std::string& ip, const std::vector<uint16_t>& ports)
    : target_ip_(ip)
{
    for (auto port : ports) {
        UDPChannelStat stat{};
        stat.port = port;
        stats_.emplace_back(stat);

        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            sockets_.emplace_back(-1);
            continue;
        }

        // recv timeout (epoll kullansak da güvenli)
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockets_.emplace_back(sock);
    }
}

// === DESTRUCTOR ===
UDPPortProfiler::~UDPPortProfiler() {
    for (int sock : sockets_) {
        if (sock >= 0) close(sock);
    }
}

// === PROBE GÖNDER ===
void UDPPortProfiler::send_probes() {
    for (size_t i = 0; i < stats_.size(); ++i) {
        int sock = sockets_[i];
        if (sock < 0) continue;

        UdpProbe probe; probe.fill(stats_[i].port);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(stats_[i].port);
        inet_pton(AF_INET, target_ip_.c_str(), &addr.sin_addr);

        ssize_t sent_bytes = sendto(sock, &probe, sizeof(probe), 0,
                                    (sockaddr*)&addr, sizeof(addr));
        if (sent_bytes != (ssize_t)sizeof(probe)) {
            perror(("sendto failed for port " + std::to_string(stats_[i].port)).c_str());
        }
        // Not: sent sayacı receive aşamasında update(false/true) ile artacak.
    }
}

// === EPOLL DESTEKLİ CEVAP DİNLEME ===
void UDPPortProfiler::receive_replies_epoll(int timeout_ms) {
    using Clock = std::chrono::high_resolution_clock;

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return;
    }

    for (int sock : sockets_) {
        if (sock < 0) continue;
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev) < 0) {
            perror("epoll_ctl");
        }
    }

    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    int nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);

    // Bu turda yanıt alan portları işaretlemek için
    std::vector<bool> got_reply(stats_.size(), false);

    for (int i = 0; i < nfds; ++i) {
        int sock = events[i].data.fd;

        sockaddr_in from{}; socklen_t from_len = sizeof(from);
        char buffer[1024]{};

        ssize_t len = recvfrom(sock, buffer, sizeof(buffer), 0,
                               (sockaddr*)&from, &from_len);
        if (len != (ssize_t)sizeof(UdpProbe)) continue;

        UdpProbe reply{};
        std::memcpy(&reply, buffer, sizeof(reply));
        if (!reply.validate()) continue;

        // RTT'yi probe içindeki timestamp ile ölç
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          Clock::now().time_since_epoch()).count();
        double rtt_ms = (now_us - (double)reply.timestamp_us) / 1000.0;

        // ilgili istatistiği güncelle
        for (size_t idx = 0; idx < stats_.size(); ++idx) {
            if (stats_[idx].port == reply.port) {
                stats_[idx].update(true, rtt_ms);
                got_reply[idx] = true;
                break;
            }
        }
    }

    // Yanıt alamayanlar için (loss say)
    for (size_t i = 0; i < sockets_.size(); ++i) {
        if (!got_reply[i]) {
            stats_[i].update(false, 0.0);
        }
    }

    close(epfd);
}

// === STAT ERİŞİMİ ===
const std::vector<UDPChannelStat>& UDPPortProfiler::get_stats() const {
    return stats_;
}
