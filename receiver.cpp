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
#include <deque>
#include <condition_variable>
#include <array>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

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
#include "udp_port_profiler.h"

using namespace std;

// --- slice header (sender ile aynı) ---
#pragma pack(push, 1)
struct SliceHeader {
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
#pragma pack(pop)

static constexpr uint32_t SLICE_MAGIC = 0xABCD1234u;

struct FrameBuffer {
    uint32_t frame_id = 0;
    uint16_t k = 0, r = 0, m = 0;
    uint16_t payload_size = 0;
    uint32_t total_frame_bytes = 0;
    std::vector<std::vector<uint8_t>> data_blocks;
    std::vector<std::vector<uint8_t>> parity_blocks;
    std::vector<uint8_t> data_present;
    std::vector<uint8_t> parity_present;
    std::chrono::steady_clock::time_point first_seen;
};

static bool try_reconstruct_frame(FrameBuffer& fb, std::vector<uint8_t>& out_bytes) {
    const int k = fb.k, r = fb.r, m = fb.m, len = fb.payload_size;

    int have_data = 0; for (int i=0;i<k;++i) have_data += fb.data_present[i]?1:0;
    if (have_data == k) {
        out_bytes.resize(fb.total_frame_bytes);
        size_t off = 0;
        for (int i=0;i<k;++i) {
            size_t to_copy = std::min<size_t>(len, fb.total_frame_bytes - off);
            if (!to_copy) break;
            std::memcpy(out_bytes.data()+off, fb.data_blocks[i].data(), to_copy);
            off += to_copy;
        }
        return true;
    }
    int present_total = have_data; for (int i=0;i<r;++i) present_total += fb.parity_present[i]?1:0;
    int missing_data = k - have_data;
    if (present_total < k || missing_data > r) return false;

    std::vector<uint8_t> A((size_t)m*(size_t)k); gf_gen_rs_matrix(A.data(), m, k);
    std::vector<uint8_t> frag_err; frag_err.reserve(k);
    for (int i=0;i<k;++i) if (!fb.data_present[i]) frag_err.push_back((uint8_t)i);
    int nerrs = (int)frag_err.size(); if (nerrs==0) return false;

    std::vector<uint8_t> survive; survive.reserve(k);
    for (int i=0;i<k && (int)survive.size()<k; ++i) if (fb.data_present[i]) survive.push_back((uint8_t)i);
    for (int i=0;i<r && (int)survive.size()<k; ++i) if (fb.parity_present[i]) survive.push_back((uint8_t)(k+i));
    if ((int)survive.size() < k) return false;

    std::vector<uint8_t> B((size_t)k*(size_t)k);
    for (int row=0; row<k; ++row) {
        uint8_t idx = survive[row];
        for (int col=0; col<k; ++col) B[(size_t)k*row+col] = A[(size_t)k*idx+col];
    }
    std::vector<uint8_t> Binv((size_t)k*(size_t)k);
    if (gf_invert_matrix(B.data(), Binv.data(), k) < 0) return false;

    std::vector<uint8_t> D((size_t)k*(size_t)nerrs);
    for (int e=0; e<nerrs; ++e) {
        int err_idx = frag_err[e];
        for (int j=0;j<k;++j) D[(size_t)k*e + j] = Binv[(size_t)k*err_idx + j];
    }

    std::vector<uint8_t*> srcs((size_t)k,nullptr);
    for (int row=0; row<k; ++row) {
        uint8_t idx = survive[row];
        srcs[row] = (idx < k) ? fb.data_blocks[idx].data() : fb.parity_blocks[idx-k].data();
    }

    std::vector<std::vector<uint8_t>> out((size_t)nerrs, std::vector<uint8_t>(len));
    std::vector<uint8_t*> outp((size_t)nerrs);
    for (int i=0;i<nerrs;++i) outp[i] = out[i].data();

    std::vector<uint8_t> g_tbls((size_t)k*(size_t)nerrs*32);
    ec_init_tables(k, nerrs, D.data(), g_tbls.data());
    ec_encode_data(len, k, nerrs, g_tbls.data(), srcs.data(), outp.data());

    for (int i=0;i<nerrs;++i) {
        int idx = frag_err[i];
        fb.data_blocks[(size_t)idx] = std::move(out[(size_t)i]);
        fb.data_present[(size_t)idx] = 1;
    }
    int have = 0; for (int i=0;i<k;++i) have += fb.data_present[i]?1:0;
    if (have != k) return false;

    out_bytes.resize(fb.total_frame_bytes);
    size_t off = 0;
    for (int i=0;i<k;++i) {
        size_t to_copy = std::min<size_t>(len, fb.total_frame_bytes - off);
        if (!to_copy) break;
        std::memcpy(out_bytes.data()+off, fb.data_blocks[i].data(), to_copy);
        off += to_copy;
    }
    return true;
}

static std::vector<uint16_t> parse_ports_csv(const std::string& csv) {
    std::vector<uint16_t> out; std::stringstream ss(csv); std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        int v = std::stoi(item);
        if (v>0 && v<65536) out.push_back((uint16_t)v);
    }
    return out;
}

static void print_usage_receiver(const char* prog) {
    std::cout << "Usage: " << prog << " [--ports <p1,p2,...> | --port <p>] [--mtu <bytes>]\n";
}

int main(int argc, char** argv) {
    std::vector<uint16_t> listen_ports = {4000,4001,4002,4003,4004};
    size_t mtu = 1000;

    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        if (a=="--ports" && i+1<argc) listen_ports = parse_ports_csv(argv[++i]);
        else if (a=="--port" && i+1<argc) { int p=std::stoi(argv[++i]); if (p>0&&p<65536) listen_ports={ (uint16_t)p }; }
        else if (a=="--mtu" && i+1<argc) { int v=std::stoi(argv[++i]); if (v>200 && v<=2000) mtu=(size_t)v; }
        else if (a=="-h"||a=="--help") { print_usage_receiver(argv[0]); return 0; }
    }

    const size_t header_size = sizeof(SliceHeader);
    const size_t payload_size_default = mtu>header_size ? (mtu-header_size) : 0;

    std::vector<int> sockets; sockets.reserve(listen_ports.size());
    for (uint16_t port : listen_ports) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { perror("socket"); continue; }

        // non-blocking
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rcvbuf = 16 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(port);
        if (bind(fd, (sockaddr*)&addr, sizeof(addr))<0) { perror("bind"); close(fd); continue; }
        sockets.push_back(fd);
        std::cout << "Listening UDP port " << port << "\n";
    }
    if (sockets.empty()) { std::cerr << "No sockets bound\n"; return 1; }

    int epfd = epoll_create1(0);
    if (epfd<0) { perror("epoll_create1"); return 1; }
    for (int fd : sockets) {
        epoll_event ev{}; ev.events=EPOLLIN; ev.data.fd=fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) perror("epoll_ctl");
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { std::cerr<<"H264 decoder not found\n"; return 2; }
    AVCodecContext* dctx = avcodec_alloc_context3(codec);
    if (avcodec_open2(dctx, codec, nullptr) < 0) { std::cerr<<"decoder open failed\n"; return 2; }
    AVFrame* frm = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    struct ImgTask { int w; int h; int fmt; std::array<uint8_t*,4> data; std::array<int,4> linesize; };
    std::mutex q_mtx; std::condition_variable q_cv; std::deque<ImgTask> img_q;
    std::atomic<bool> run_workers{true};
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    unsigned worker_threads = std::min<unsigned>(16, std::max<unsigned>(6, hw));
    std::vector<std::thread> workers; workers.reserve(worker_threads);
    for (unsigned t=0;t<worker_threads;++t) {
        workers.emplace_back([&,t]{
            while (run_workers.load()) {
                ImgTask task{};
                {
                    std::unique_lock<std::mutex> lk(q_mtx);
                    q_cv.wait(lk,[&]{ return !img_q.empty() || !run_workers.load(); });
                    if (!run_workers.load()) break;
                    task = std::move(img_q.front()); img_q.pop_front();
                }
                SwsContext* lsws = sws_getContext(task.w, task.h, (AVPixelFormat)task.fmt,
                                                  task.w, task.h, AV_PIX_FMT_BGR24,
                                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!lsws) continue;
                cv::Mat img(task.h, task.w, CV_8UC3);
                uint8_t* dst[] = { img.data, nullptr, nullptr, nullptr };
                int dstStride[] = { (int)img.step[0], 0, 0, 0 };
                sws_scale(lsws, task.data.data(), task.linesize.data(), 0, task.h, dst, dstStride);
                sws_freeContext(lsws);
                cv::imshow("NovaEngine Receiver", img);
                cv::waitKey(1);
            }
        });
    }

    std::unordered_map<uint32_t, FrameBuffer> frames;
    int num_ports = (int)listen_ports.size();
    int buffer_ms = (num_ports <= 1) ? 60 : (num_ports <= 3 ? 80 : 90);
    std::chrono::milliseconds frame_timeout{buffer_ms};

    cv::namedWindow("NovaEngine Receiver", cv::WINDOW_AUTOSIZE);

    std::vector<uint8_t> rxbuf(std::max<size_t>(mtu, 4096));
    epoll_event events[64];

    const size_t MAX_LIVE_FRAMES = 6;

    while (true) {
        int nfds = epoll_wait(epfd, events, 64, std::max(5, buffer_ms / 3));
        for (int i=0;i<nfds;++i) {
            int fd = events[i].data.fd;
            for (;;) {
                sockaddr_in from{}; socklen_t fromlen = sizeof(from);
                ssize_t n = recvfrom(fd, rxbuf.data(), rxbuf.size(), 0, (sockaddr*)&from, &fromlen);
                if (n < 0) {
                    if (errno==EAGAIN || errno==EWOULDBLOCK) break; // bu fd boşaldı
                    else break;
                }
                if (n == (ssize_t)sizeof(UdpProbe)) {
                    UdpProbe pr{}; std::memcpy(&pr, rxbuf.data(), sizeof(pr));
                    if (pr.validate()) { sendto(fd, &pr, sizeof(pr), 0, (sockaddr*)&from, fromlen); continue; }
                }
                if (n < (ssize_t)sizeof(SliceHeader)) continue;

                SliceHeader sh{}; std::memcpy(&sh, rxbuf.data(), sizeof(SliceHeader));
                if (sh.magic != SLICE_MAGIC) continue;
                const size_t recv_payload = (size_t)n - sizeof(SliceHeader);

                auto& fb = frames[sh.frame_id];
                if (fb.k == 0) {
                    fb.frame_id = sh.frame_id; fb.k = sh.k_data; fb.r = sh.r_parity; fb.m = sh.total_slices;
                    size_t alloc_payload = sh.payload_bytes ? sh.payload_bytes : payload_size_default;
                    fb.payload_size = (uint16_t)alloc_payload;
                    fb.total_frame_bytes = sh.total_frame_bytes;
                    fb.data_blocks.assign(fb.k, std::vector<uint8_t>(fb.payload_size));
                    fb.parity_blocks.assign(fb.r, std::vector<uint8_t>(fb.payload_size));
                    fb.data_present.assign(fb.k, 0);
                    fb.parity_present.assign(fb.r, 0);
                    fb.first_seen = std::chrono::steady_clock::now();

                    if (frames.size() > MAX_LIVE_FRAMES) {
                        uint32_t oldest = UINT32_MAX;
                        for (auto &kv : frames) oldest = std::min(oldest, kv.first);
                        if (oldest != sh.frame_id) frames.erase(oldest);
                    }
                }

                const uint8_t* payload = rxbuf.data() + sizeof(SliceHeader);
                if (sh.slice_index < fb.k) {
                    size_t copy_len = std::min<size_t>(fb.payload_size, recv_payload);
                    std::memcpy(fb.data_blocks[sh.slice_index].data(), payload, copy_len);
                    uint32_t h = 2166136261u;
                    for (size_t i2=0;i2<fb.payload_size;++i2){ h ^= fb.data_blocks[sh.slice_index][i2]; h *= 16777619u; }
                    if (h == sh.checksum) fb.data_present[sh.slice_index] = 1;
                } else {
                    uint16_t pi = (uint16_t)(sh.slice_index - fb.k);
                    if (pi < fb.r) {
                        size_t copy_len = std::min<size_t>(fb.payload_size, recv_payload);
                        std::memcpy(fb.parity_blocks[pi].data(), payload, copy_len);
                        uint32_t h = 2166136261u;
                        for (size_t i2=0;i2<fb.payload_size;++i2){ h ^= fb.parity_blocks[pi][i2]; h *= 16777619u; }
                        if (h == sh.checksum) fb.parity_present[pi] = 1;
                    }
                }

                std::vector<uint8_t> complete;
                if (try_reconstruct_frame(fb, complete)) {
                    av_packet_unref(pkt);
                    if (av_new_packet(pkt, (int)complete.size()) == 0) {
                        std::memcpy(pkt->data, complete.data(), complete.size());
                    } else continue;

                    if (avcodec_send_packet(dctx, pkt) == 0) {
                        while (avcodec_receive_frame(dctx, frm) == 0) {
                            ImgTask task{}; task.w=frm->width; task.h=frm->height; task.fmt=frm->format;
                            for (int p=0;p<4;++p){ task.data[p]=frm->data[p]; task.linesize[p]=frm->linesize[p]; }
                            {
                                std::lock_guard<std::mutex> lk(q_mtx);
                                img_q.emplace_back(std::move(task));
                            }
                            q_cv.notify_one();
                        }
                    }
                    frames.erase(sh.frame_id);
                }
            } // drain for this fd
        } // epoll events

        // timeout temizliği
        auto now = std::chrono::steady_clock::now();
        for (auto it = frames.begin(); it != frames.end(); ) {
            if (now - it->second.first_seen > frame_timeout) it = frames.erase(it);
            else ++it;
        }
    }

    run_workers = false; q_cv.notify_all();
    for (auto& th : workers) if (th.joinable()) th.join();
    av_frame_free(&frm); av_packet_free(&pkt); avcodec_free_context(&dctx);
    for (int fd : sockets) close(fd); close(epfd);
    return 0;
}
