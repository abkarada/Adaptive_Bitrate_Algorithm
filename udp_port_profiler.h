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

// === PORT PROFİLİ ===
struct UDPChannelStat {
    uint16_t port = 0;
    double   avg_rtt_ms  = 10.0;
    double   packet_loss = 0.0;
    size_t   sent = 0;
    size_t   received = 0;

    void update(bool success, double rtt_ms) {
        // her döngüde tam olarak 1 kez çağrılır (true/false)
        sent++;
        if (success) {
            received++;
            avg_rtt_ms = 0.8 * avg_rtt_ms + 0.2 * rtt_ms;
        }
        packet_loss = (sent > 0) ? (1.0 - double(received) / double(sent)) : 0.0;
    }
};

// === PROBE VERİ YAPISI ===
#pragma pack(push, 1)
struct UdpProbe {
    uint32_t magic = 0xDEADBEEF;
    uint16_t port  = 0;
    uint64_t timestamp_us = 0;

    void fill(uint16_t p) {
        std::memset(this, 0, sizeof(*this));
        magic = 0xDEADBEEF;
        port  = p;
        auto now = std::chrono::high_resolution_clock::now();
        timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
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
    const std::vector<UDPChannelStat>& get_stats() const { return stats_; }

private:
    std::string target_ip_;
    std::vector<UDPChannelStat> stats_;
    std::vector<int> sockets_;
};

inline UDPPortProfiler::UDPPortProfiler(const std::string& ip, const std::vector<uint16_t>& ports)
: target_ip_(ip)
{
    for (auto port : ports) {
        UDPChannelStat s{}; s.port = port; stats_.push_back(s);
        int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) { perror("socket"); sockets_.push_back(-1); continue; }

        // recv timeout (güvenlik)
        timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sockets_.push_back(sock);
    }
}

inline UDPPortProfiler::~UDPPortProfiler() {
    for (int s : sockets_) if (s >= 0) close(s);
}

inline void UDPPortProfiler::send_probes() {
    for (size_t i = 0; i < stats_.size(); ++i) {
        int sock = sockets_[i]; if (sock < 0) continue;
        UdpProbe pr; pr.fill(stats_[i].port);

        sockaddr_in addr{}; addr.sin_family = AF_INET;
        addr.sin_port = htons(stats_[i].port);
        inet_pton(AF_INET, target_ip_.c_str(), &addr.sin_addr);

        ssize_t n = sendto(sock, &pr, sizeof(pr), 0, (sockaddr*)&addr, sizeof(addr));
        if (n != (ssize_t)sizeof(pr)) perror("sendto(probe)");
        // sent sayacını receive aşamasında update(true/false) ile arttırıyoruz
    }
}

inline void UDPPortProfiler::receive_replies_epoll(int timeout_ms) {
    using Clock = std::chrono::high_resolution_clock;

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return; }

    for (int s : sockets_) {
        if (s < 0) continue;
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = s;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, s, &ev) < 0) perror("epoll_ctl");
    }

    const int MAXE = 64;
    epoll_event evs[MAXE];
    int nfds = epoll_wait(epfd, evs, MAXE, timeout_ms);

    std::vector<bool> got(stats_.size(), false);

    for (int i = 0; i < nfds; ++i) {
        int s = evs[i].data.fd;
        sockaddr_in from{}; socklen_t fl = sizeof(from);
        char buf[64]{};
        ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
        if (n != (ssize_t)sizeof(UdpProbe)) continue;

        UdpProbe r{}; std::memcpy(&r, buf, sizeof(r));
        if (!r.validate()) continue;

        double rtt_ms = 0.0;
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        rtt_ms = (now_us - (double)r.timestamp_us) / 1000.0;

        for (size_t k = 0; k < stats_.size(); ++k)
            if (stats_[k].port == r.port) { stats_[k].update(true, rtt_ms); got[k] = true; break; }
    }

    for (size_t i = 0; i < sockets_.size(); ++i) if (!got[i]) stats_[i].update(false, 0.0);

    close(epfd);
}
