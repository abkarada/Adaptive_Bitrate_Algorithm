#include <iostream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <string>
#include <set>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <isa-l/erasure_code.h>

#include "udp_port_profiler.h" // for UdpProbe struct (echo)

using namespace std;

// Simplified receiver - no shared stats needed
// std::vector<UDPChannelStat> g_last_stats; // Removed for performance

// Mirror of sender's header
#pragma pack(push, 1)
struct SliceHeader {
    uint32_t magic;            // 0xABCD1234
    uint32_t frame_id;         // increasing
    uint16_t slice_index;      // [0..total_slices-1]
    uint16_t total_slices;     // k + r
    uint16_t k_data;           // k
    uint16_t r_parity;         // r
    uint16_t payload_bytes;    // payload size used by encoder
    uint32_t total_frame_bytes;// original encoded size
    uint64_t timestamp_us;     // sender ts
    uint8_t  flags;            // bit0 parity
    uint32_t checksum;         // FNV-1a over payload
};
#pragma pack(pop)

static constexpr uint32_t SLICE_MAGIC = 0xABCD1234u;

struct FrameBuffer {
    uint32_t frame_id = 0;
    uint16_t k = 0, r = 0, m = 0;
    uint16_t payload_size = 0; // MTU - header
    uint32_t total_frame_bytes = 0;
    std::vector<std::vector<uint8_t>> data_blocks;   // k
    std::vector<std::vector<uint8_t>> parity_blocks; // r
    std::vector<uint8_t> data_present;   // k flags
    std::vector<uint8_t> parity_present; // r flags
    std::chrono::steady_clock::time_point first_seen;
    std::set<std::pair<uint16_t, uint16_t>> received_packets; // track (slice_index, port) to filter duplicates
};

static inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

// Minimal decode-matrix generator adapted from ISA-L examples (RS matrix)
static int gen_decode_matrix_rs(uint8_t* encode_matrix, uint8_t* decode_matrix,
                                uint8_t* invert_matrix, uint8_t* temp_matrix,
                                uint8_t* decode_index, uint8_t* frag_err_list,
                                int nerrs, int k, int m) {
    // Build matrix b from rows of encode_matrix for surviving fragments
    std::vector<uint8_t> frag_in_err(static_cast<size_t>(m), 0);
    int nsrcerrs = 0;
    for (int i = 0; i < nerrs; ++i) {
        if (frag_err_list[i] < k) nsrcerrs++;
        frag_in_err[frag_err_list[i]] = 1;
    }
    int r = 0;
    for (int i = 0; i < k; i++, r++) {
        while (frag_in_err[r]) r++;
        for (int j = 0; j < k; ++j) temp_matrix[k * i + j] = encode_matrix[k * r + j];
        decode_index[i] = static_cast<uint8_t>(r);
    }
    if (gf_invert_matrix(temp_matrix, invert_matrix, k) < 0) return -1;
    // Source erasures rows from invert_matrix
    for (int i = 0; i < nerrs; ++i) {
        if (frag_err_list[i] < k) {
            for (int j = 0; j < k; ++j)
                decode_matrix[k * i + j] = invert_matrix[k * frag_err_list[i] + j];
        }
    }
    // Parity erasures rows: invert * encode_matrix row
    for (int p = 0; p < nerrs; ++p) {
        if (frag_err_list[p] >= k) {
            for (int i = 0; i < k; ++i) {
                uint8_t s = 0;
                for (int j = 0; j < k; ++j)
                    s ^= gf_mul(invert_matrix[j * k + i], encode_matrix[k * frag_err_list[p] + j]);
                decode_matrix[k * p + i] = s;
            }
        }
    }
    return 0;
}

// Attempt to assemble a complete encoded frame into out_bytes
static bool try_reconstruct_frame(FrameBuffer& fb, std::vector<uint8_t>& out_bytes) {
    const int k = fb.k;
    const int r = fb.r;
    const int m = fb.m;
    const int len = fb.payload_size;

    int have_data = 0;
    for (int i = 0; i < k; ++i) have_data += fb.data_present[i] ? 1 : 0;
    if (have_data == k) {
        // Fast path: direct concat of data blocks
        out_bytes.resize(fb.total_frame_bytes);
        size_t off = 0;
        for (int i = 0; i < k; ++i) {
            size_t to_copy = std::min<size_t>(len, fb.total_frame_bytes - off);
            if (to_copy == 0) break;
            std::memcpy(out_bytes.data() + off, fb.data_blocks[i].data(), to_copy);
            off += to_copy;
        }
        return true;
    }

    // Need to recover missing data blocks using parity if possible
    int present_total = have_data;
    for (int i = 0; i < r; ++i) present_total += fb.parity_present[i] ? 1 : 0;
    int missing_data = k - have_data;
    if (present_total < k || missing_data > r) return false; // not enough to recover

    // Build encode matrix A (m x k)
    std::vector<uint8_t> a(static_cast<size_t>(m) * static_cast<size_t>(k));
    gf_gen_rs_matrix(a.data(), m, k);

    // Build error list for missing data fragments only (indices among 0..k-1)
    std::vector<uint8_t> frag_err_list;
    frag_err_list.reserve(k);
    for (int i = 0; i < k; ++i) if (!fb.data_present[i]) frag_err_list.push_back(static_cast<uint8_t>(i));
    int nerrs = static_cast<int>(frag_err_list.size());
    if (nerrs == 0) return false; // shouldn't reach here, fast path handled earlier

    // Choose k surviving fragment indices from actually present blocks
    std::vector<uint8_t> survive_indices;
    survive_indices.reserve(k);
    for (int i = 0; i < k && (int)survive_indices.size() < k; ++i) if (fb.data_present[i]) survive_indices.push_back(static_cast<uint8_t>(i));
    for (int i = 0; i < r && (int)survive_indices.size() < k; ++i) if (fb.parity_present[i]) survive_indices.push_back(static_cast<uint8_t>(k + i));
    if ((int)survive_indices.size() < k) return false;

    // Construct temp_matrix from survive_indices rows (B)
    std::vector<uint8_t> temp_matrix(static_cast<size_t>(k) * static_cast<size_t>(k));
    for (int row = 0; row < k; ++row) {
        uint8_t idx = survive_indices[row];
        for (int col = 0; col < k; ++col) temp_matrix[k * row + col] = a[k * idx + col];
    }
    // Invert B
    std::vector<uint8_t> invert_matrix(static_cast<size_t>(k) * static_cast<size_t>(k));
    if (gf_invert_matrix(temp_matrix.data(), invert_matrix.data(), k) < 0) return false;

    // Build decode matrix rows for missing data
    std::vector<uint8_t> decode_matrix(static_cast<size_t>(k) * static_cast<size_t>(nerrs));
    for (int e = 0; e < nerrs; ++e) {
        int err_idx = frag_err_list[e];
        for (int j = 0; j < k; ++j) decode_matrix[k * e + j] = invert_matrix[k * err_idx + j];
    }

    // Prepare recover_srcs pointers from actually present fragments in survive_indices
    std::vector<uint8_t*> recover_srcs(static_cast<size_t>(k), nullptr);
    for (int row = 0; row < k; ++row) {
        uint8_t idx = survive_indices[row];
        if (idx < k) recover_srcs[row] = fb.data_blocks[idx].data();
        else recover_srcs[row] = fb.parity_blocks[idx - k].data();
    }
    // Output buffers for recovered data blocks
    std::vector<std::vector<uint8_t>> recover_out(static_cast<size_t>(nerrs), std::vector<uint8_t>(len));
    std::vector<uint8_t*> recover_outp(static_cast<size_t>(nerrs));
    for (int i = 0; i < nerrs; ++i) recover_outp[static_cast<size_t>(i)] = recover_out[static_cast<size_t>(i)].data();

    // Initialize g_tbls and recover
    std::vector<uint8_t> g_tbls(static_cast<size_t>(k) * static_cast<size_t>(nerrs) * 32);
    ec_init_tables(k, nerrs, decode_matrix.data(), g_tbls.data());
    ec_encode_data(len, k, nerrs, g_tbls.data(), recover_srcs.data(), recover_outp.data());

    // Place recovered data blocks back to their indices
    for (int i = 0; i < nerrs; ++i) {
        int idx = frag_err_list[static_cast<size_t>(i)];
        fb.data_blocks[static_cast<size_t>(idx)] = std::move(recover_out[static_cast<size_t>(i)]);
        fb.data_present[static_cast<size_t>(idx)] = 1;
    }

    // Now we should have all k data blocks
    int have = 0; for (int i = 0; i < k; ++i) have += fb.data_present[i] ? 1 : 0;
    if (have != k) return false;

    out_bytes.resize(fb.total_frame_bytes);
    size_t off = 0;
    for (int i = 0; i < k; ++i) {
        size_t to_copy = std::min<size_t>(len, fb.total_frame_bytes - off);
        if (to_copy == 0) break;
        std::memcpy(out_bytes.data() + off, fb.data_blocks[i].data(), to_copy);
        off += to_copy;
    }
    return true;
}

static std::vector<uint16_t> parse_ports_csv(const std::string& csv) {
    std::vector<uint16_t> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        int v = std::stoi(item);
        if (v > 0 && v < 65536) out.push_back(static_cast<uint16_t>(v));
    }
    return out;
}

static void print_usage_receiver(const char* prog) {
    std::cout << "Usage: " << prog << " [--ports <p1,p2,...> | --port <p>] [--mtu <bytes>]" << std::endl;
    std::cout << "  examples:\n"
              << "    " << prog << " --port 4000 --mtu 1200\n"
              << "    " << prog << " --ports 4000,4001,4002 --mtu 1200\n";
}

int main(int argc, char** argv) {
    // 5 ports for 5x bandwidth multiplication!
    std::vector<uint16_t> listen_ports = {4000, 4001, 4002, 4003, 4004};
    size_t mtu = 1000; // Fixed size buffer - no fragmentation

    // CLI
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ports" && i + 1 < argc) {
            listen_ports = parse_ports_csv(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            int p = std::stoi(argv[++i]);
            if (p > 0 && p < 65536) listen_ports = { static_cast<uint16_t>(p) };
        } else if (arg == "--mtu" && i + 1 < argc) {
            int v = std::stoi(argv[++i]);
            if (v > 200 && v <= 2000) mtu = static_cast<size_t>(v);
        } else if (arg == "-h" || arg == "--help") {
            print_usage_receiver(argv[0]);
            return 0;
        }
    }
    if (listen_ports.empty()) {
        std::cerr << "No listen ports specified.\n";
        print_usage_receiver(argv[0]);
        return 1;
    }

    const size_t header_size = sizeof(SliceHeader);
    const size_t payload_size_default = mtu > header_size ? (mtu - header_size) : 0;

    // Sockets
    std::vector<int> sockets;
    sockets.reserve(listen_ports.size());
    for (uint16_t port : listen_ports) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket"); continue; }
        
        // Huge receive buffer to prevent packet loss
        int rcvbuf = 16 * 1024 * 1024; // 16MB receive buffer
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(fd); continue; }
        sockets.push_back(fd);
        std::cout << "Listening UDP port " << port << std::endl;
    }
    if (sockets.empty()) { std::cerr << "No sockets bound" << std::endl; return 1; }

    // epoll
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }
    for (int fd : sockets) {
        epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) perror("epoll_ctl");
    }

    // FFmpeg decoder
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { std::cerr << "H264 decoder not found" << std::endl; return 2; }
    AVCodecContext* dctx = avcodec_alloc_context3(codec);
    // keep default decoder opts; no libavutil/opt on receiver to minimize deps
    if (avcodec_open2(dctx, codec, nullptr) < 0) { std::cerr << "decoder open failed" << std::endl; return 2; }
    AVFrame* frm = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;

    std::unordered_map<uint32_t, FrameBuffer> frames;
    // Smart 30ms buffering for packet reordering from multiple tunnels
    std::chrono::milliseconds frame_timeout{30}; // 30ms buffer for reordering

    cv::namedWindow("NovaEngine Receiver", cv::WINDOW_AUTOSIZE);

    size_t rxbuf_capacity = std::max<size_t>(mtu, 4096); // Bigger receive buffer
    std::vector<uint8_t> rxbuf(rxbuf_capacity);
    epoll_event events[32];

    while (true) {
        // No dynamic timeout calculations - just process packets as fast as possible
        int nfds = epoll_wait(epfd, events, 32, 5); // Reduced from 50ms to 5ms for responsiveness
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            sockaddr_in from{}; socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(fd, rxbuf.data(), rxbuf.size(), 0, (sockaddr*)&from, &fromlen);
            if (n <= 0) continue;

            // Probe echo support
            if (n == (ssize_t)sizeof(UdpProbe)) {
                UdpProbe pr{}; std::memcpy(&pr, rxbuf.data(), sizeof(pr));
                if (pr.validate()) {
                    // echo back
                    sendto(fd, &pr, sizeof(pr), 0, (sockaddr*)&from, fromlen);
                    continue;
                }
            }

            if (n < (ssize_t)sizeof(SliceHeader)) continue;
            SliceHeader sh{}; std::memcpy(&sh, rxbuf.data(), sizeof(SliceHeader));
            if (sh.magic != SLICE_MAGIC) continue;
            const size_t recv_payload = static_cast<size_t>(n) - header_size;

            // Initialize frame buffer if first time
            auto& fb = frames[sh.frame_id];
            if (fb.k == 0) {
                fb.frame_id = sh.frame_id;
                fb.k = sh.k_data; fb.r = sh.r_parity; fb.m = sh.total_slices;
                // Trust sender-advertised payload_bytes to avoid MTU mismatch
                size_t alloc_payload = sh.payload_bytes ? sh.payload_bytes : payload_size_default;
                fb.payload_size = static_cast<uint16_t>(alloc_payload);
                fb.total_frame_bytes = sh.total_frame_bytes;
                fb.data_blocks.assign(fb.k, std::vector<uint8_t>(fb.payload_size));
                fb.parity_blocks.assign(fb.r, std::vector<uint8_t>(fb.payload_size));
                fb.data_present.assign(fb.k, 0);
                fb.parity_present.assign(fb.r, 0);
                fb.first_seen = std::chrono::steady_clock::now();
            }

            // Check for duplicate packets from multiple tunnels
            uint16_t recv_port = ntohs(from.sin_port);
            auto packet_id = std::make_pair(sh.slice_index, recv_port);
            
            // Skip if we already have this slice (from any tunnel)
            if ((sh.slice_index < fb.k && fb.data_present[sh.slice_index]) ||
                (sh.slice_index >= fb.k && sh.slice_index - fb.k < fb.r && fb.parity_present[sh.slice_index - fb.k])) {
                continue; // Already have this slice, skip duplicate
            }
            
            const uint8_t* payload = rxbuf.data() + sizeof(SliceHeader);
            if (sh.slice_index < fb.k) {
                size_t copy_len = std::min<size_t>(fb.payload_size, recv_payload);
                std::memcpy(fb.data_blocks[sh.slice_index].data(), payload, copy_len);
                // checksum validation
                uint32_t h = 2166136261u; for (size_t i=0;i<fb.payload_size;++i){ h ^= fb.data_blocks[sh.slice_index][i]; h *= 16777619u; }
                if (h == sh.checksum) {
                    fb.data_present[sh.slice_index] = 1;
                    fb.received_packets.insert(packet_id);
                }
            } else {
                uint16_t pi = static_cast<uint16_t>(sh.slice_index - fb.k);
                if (pi < fb.r) {
                    size_t copy_len = std::min<size_t>(fb.payload_size, recv_payload);
                    std::memcpy(fb.parity_blocks[pi].data(), payload, copy_len);
                    uint32_t h = 2166136261u; for (size_t i=0;i<fb.payload_size;++i){ h ^= fb.parity_blocks[pi][i]; h *= 16777619u; }
                    if (h == sh.checksum) {
                        fb.parity_present[pi] = 1;
                        fb.received_packets.insert(packet_id);
                    }
                }
            }

             // Try reconstruct
            std::vector<uint8_t> complete;
            if (try_reconstruct_frame(fb, complete)) {
                // Feed entire access unit directly (fps düşük, payload Annex B)
                av_packet_unref(pkt);
                if (av_new_packet(pkt, static_cast<int>(complete.size())) == 0) {
                    std::memcpy(pkt->data, complete.data(), complete.size());
                } else {
                    continue;
                }
                if (avcodec_send_packet(dctx, pkt) == 0) {
                    while (avcodec_receive_frame(dctx, frm) == 0) {
                        // Drain all frames for this packet
                        if (!sws) sws = sws_getContext(frm->width, frm->height, (AVPixelFormat)frm->format,
                                                       frm->width, frm->height, AV_PIX_FMT_BGR24,
                                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                        if (!sws) break;
                        cv::Mat img(frm->height, frm->width, CV_8UC3);
                        uint8_t* dst[] = { img.data, nullptr, nullptr, nullptr };
                        int dstStride[] = { static_cast<int>(img.step[0]), 0, 0, 0 };
                        sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dst, dstStride);
                        
                        // No FPS limiting - display immediately for minimum latency
                        cv::imshow("NovaEngine Receiver", img);
                        cv::waitKey(1); // Minimal wait for OpenCV event processing
                    }
                }
                frames.erase(sh.frame_id);
            }
        }

        // Cleanup old frames
        auto now = std::chrono::steady_clock::now();
        for (auto it = frames.begin(); it != frames.end(); ) {
            if (now - it->second.first_seen > frame_timeout) it = frames.erase(it); else ++it;
        }
    }

    // Cleanup
    if (sws) sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    // no parser in use
    avcodec_free_context(&dctx);
    for (int fd : sockets) close(fd);
    close(epfd);
    return 0;
}


