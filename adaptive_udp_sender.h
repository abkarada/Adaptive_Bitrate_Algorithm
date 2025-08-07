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

void AdaptiveUDPSender::send_slices(const std::vector<std::vector<uint8_t>>& chunks) {
    for (const auto& slice : chunks) {
        std::vector<int> used_ports;

        for (int i = 0; i < redundancy; ++i) {
            int port_index = select_best_port_index();

            // Aynı portu iki kez seçmemeye çalış (varsayım: redundancy < tunnel sayısı)
            while (std::find(used_ports.begin(), used_ports.end(), port_index) != used_ports.end()) {
                port_index = (port_index + 1) % tunnels_.size();  // round-robin fallback
            }

            send_slice(slice, port_index);
            used_ports.push_back(port_index);
        }
    }
}

void AdaptiveUDPSender::enable_redundancy(int redundancy_count) {
    if (redundancy_count < 1) redundancy_count = 1;
    if (redundancy_count > tunnels_.size()) redundancy_count = tunnels_.size();
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
