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
#include <thread>

struct UDPChannelStat {
    uint16_t port;
    double avg_rtt_ms = 10.0;
    double packet_loss = 0.0;
    size_t sent = 0;
    size_t received = 0;

    void update(bool success, double rtt_ms) {
        sent++;
        if (success) {
            received++;
            avg_rtt_ms = 0.8 * avg_rtt_ms + 0.2 * rtt_ms;
        }
        // Loss her zaman güncellenir
        if (sent > 0)
            packet_loss = 1.0 - (double(received) / sent);
    }
};

struct UdpProbe {
    uint32_t magic = 0xDEADBEEF;
    uint16_t port;
    uint64_t timestamp_us;

    void fill(uint16_t p) {
        port = p;
        auto now = std::chrono::high_resolution_clock::now();
        timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    }

    bool validate() const {
        return magic == 0xDEADBEEF;
    }
};

class UDPPortProfiler {
public:
    UDPPortProfiler(const std::string& ip, const std::vector<uint16_t>& ports);
    void send_probes();
    void receive_replies(int timeout_ms = 1000);
    const std::vector<UDPChannelStat>& get_stats() const;

private:
    std::string target_ip_;
    std::vector<UDPChannelStat> stats_;
    std::vector<int> sockets_;
};


UDPPortProfiler::UDPPortProfiler(const std::string& ip, const std::vector<uint16_t>& ports)
    : target_ip_(ip)
{
    for (auto port : ports) {
        UDPChannelStat stat;
        stat.port = port;
        stats_.emplace_back(stat);

        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            perror("socket creation failed");
            sockets_.emplace_back(-1);
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockets_.emplace_back(sock);
    }
}

void UDPPortProfiler::send_probes() {
    for (size_t i = 0; i < stats_.size(); ++i) {
        int sock = sockets_[i];
        if (sock < 0) continue; // geçersiz socket

        UdpProbe probe;
        probe.fill(stats_[i].port);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(stats_[i].port);  // hedefte dinlenen port
        inet_pton(AF_INET, target_ip_.c_str(), &addr.sin_addr);

        ssize_t sent = sendto(sock, &probe, sizeof(probe), 0, (sockaddr*)&addr, sizeof(addr));
        if (sent < 0) {
            perror(("sendto failed for port " + std::to_string(stats_[i].port)).c_str());
        }
    }
}
void UDPPortProfiler::receive_replies(int timeout_ms) {
    using Clock = std::chrono::high_resolution_clock;

    for (size_t i = 0; i < sockets_.size(); ++i) {
        int sock = sockets_[i];
        if (sock < 0) continue;

        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        char buffer[1024];

        ssize_t len = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&from, &from_len);
        auto now = Clock::now();

        if (len < (ssize_t)sizeof(UdpProbe)) {
            stats_[i].update(false, 0);
            continue;
        }

        UdpProbe reply;
        memcpy(&reply, buffer, sizeof(reply));

        if (!reply.validate()) {
            stats_[i].update(false, 0);
            continue;
        }

        uint64_t sent_us = reply.timestamp_us;
        uint64_t recv_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        double rtt_ms = (recv_us - sent_us) / 1000.0;

        for (auto& stat : stats_) {
            if (stat.port == reply.port) {
                stat.update(true, rtt_ms);
                break;
            }
        }
    }
}

const std::vector<UDPChannelStat>& UDPPortProfiler::get_stats() const {
    return stats_;
}

