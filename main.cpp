#include <iostream>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <isa-l/erasure_code.h>

#include "adaptive_udp_sender.h"
#include "udp_port_profiler.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/bsf.h>
}

using namespace std;
using namespace cv;

#define DEVICE_ID 0
#define WIDTH 640
#define HEIGHT 480
#define FPS 30

static std::atomic<size_t> g_mtu_bytes{1000};
std::vector<UDPChannelStat> g_last_stats;

#pragma pack(push, 1)
struct SliceHeader {
    uint32_t magic = 0xABCD1234;
    uint32_t frame_id = 0;
    uint16_t slice_index = 0;
    uint16_t total_slices = 0;
    uint16_t k_data = 0;
    uint16_t r_parity = 0;
    uint16_t payload_bytes = 0;
    uint32_t total_frame_bytes = 0;
    uint64_t timestamp_us = 0;
    uint8_t  flags = 0; // bit0 parity, bit1 key
    uint32_t checksum = 0;
};
#pragma pack(pop)

struct BuiltSlices {
    std::vector<std::vector<uint8_t>> data_slices;
    std::vector<std::vector<uint8_t>> parity_slices;
    int k = 0;
    int r = 0;
};

static inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

static BuiltSlices build_slices_with_fec(const uint8_t* data, size_t size, size_t mtu_bytes, uint32_t frame_id, bool is_keyframe) {
    BuiltSlices out{};
    const size_t H = sizeof(SliceHeader);
    if (mtu_bytes <= H) return out;

    const size_t payload = mtu_bytes - H;
    int k = (int)((size + payload - 1) / payload);
    if (k <= 0) k = 1;

    // daha makul FEC: taban %12, loss ile art, cap %35
    double avg_loss = 0.0;
    if (!g_last_stats.empty()) {
        for (auto& s : g_last_stats) avg_loss += s.packet_loss;
        avg_loss /= g_last_stats.size();
    }
    double base = 0.12, cap = 0.35;
    double fec_ratio = std::min(cap, base + std::max(0.01, avg_loss) * 1.2);
    int r = std::clamp((int)std::ceil(k * fec_ratio), 1, std::max(3, k/3));
    if (is_keyframe) r = std::min(r + 1, std::max(4, k/2)); // keyframe'e hafif bonus

    out.k = k; out.r = r;

    auto fnv1a = [](const uint8_t* p, size_t n){
        uint32_t h = 2166136261u;
        for (size_t i=0;i<n;++i){ h ^= p[i]; h *= 16777619u; }
        return h;
    };

    out.data_slices.resize(k);
    size_t off = 0;
    for (int i=0;i<k;++i) {
        out.data_slices[i].assign(mtu_bytes, 0);
        SliceHeader h{};
        h.frame_id = frame_id; h.slice_index=(uint16_t)i;
        h.total_slices=(uint16_t)(k+r); h.k_data=(uint16_t)k; h.r_parity=(uint16_t)r;
        h.payload_bytes=(uint16_t)payload; h.total_frame_bytes=(uint32_t)size;
        h.timestamp_us = now_us(); h.flags = is_keyframe?0x02:0x00;
        std::memcpy(out.data_slices[i].data(), &h, sizeof(SliceHeader));

        size_t rem = (off < size) ? (size - off) : 0;
        size_t cp  = std::min(payload, rem);
        if (cp) std::memcpy(out.data_slices[i].data()+H, data+off, cp);
        h.checksum = fnv1a(out.data_slices[i].data()+H, payload);
        std::memcpy(out.data_slices[i].data(), &h, sizeof(SliceHeader));
        off += cp;
    }

    if (k >= 2) {
        std::vector<uint8_t*> dptr((size_t)k);
        for (int i=0;i<k;++i) dptr[i] = out.data_slices[i].data() + H;

        std::vector<std::vector<uint8_t>> pbuf((size_t)r, std::vector<uint8_t>(payload));
        std::vector<uint8_t*> pptr((size_t)r);
        for (int i=0;i<r;++i) pptr[i] = pbuf[i].data();

        std::vector<uint8_t> M((size_t)(k+r)*(size_t)k);
        std::vector<uint8_t> tbl((size_t)k*(size_t)r*32);
        gf_gen_rs_matrix(M.data(), k+r, k);
        ec_init_tables(k, r, M.data()+k*k, tbl.data());
        ec_encode_data((int)payload, k, r, tbl.data(), dptr.data(), pptr.data());

        out.parity_slices.resize(r);
        for (int i=0;i<r;++i) {
            out.parity_slices[i].assign(mtu_bytes, 0);
            SliceHeader h{};
            h.frame_id=frame_id; h.slice_index=(uint16_t)(k+i);
            h.total_slices=(uint16_t)(k+r); h.k_data=(uint16_t)k; h.r_parity=(uint16_t)r;
            h.payload_bytes=(uint16_t)payload; h.total_frame_bytes=(uint32_t)size;
            h.timestamp_us = now_us(); h.flags = (uint8_t)(0x01 | (is_keyframe?0x02:0x00));
            std::memcpy(out.parity_slices[i].data(), &h, sizeof(SliceHeader));
            std::memcpy(out.parity_slices[i].data()+H, pbuf[i].data(), payload);
            h.checksum = fnv1a(out.parity_slices[i].data()+H, payload);
            std::memcpy(out.parity_slices[i].data(), &h, sizeof(SliceHeader));
        }
    }
    return out;
}

static std::vector<uint16_t> parse_ports_csv(const std::string& csv) {
    std::vector<uint16_t> out; std::stringstream ss(csv); std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue; int v=std::stoi(item);
        if (v>0 && v<65536) out.push_back((uint16_t)v);
    }
    return out;
}

static void usage(const char* p) {
    std::cout << "Usage: " << p << " --ip <receiver_ip> --ports <p1,p2,...> [--mtu <bytes>] [--bitrate <bps>]\n";
}

int main(int argc, char** argv) {
    std::string receiver_ip = "127.0.0.1";
    std::vector<uint16_t> receiver_ports = {4000,4001,4002,4003,4004};
    int bitrate = 5'000'000;

    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        if (a=="--ip" && i+1<argc) receiver_ip = argv[++i];
        else if (a=="--ports" && i+1<argc) receiver_ports = parse_ports_csv(argv[++i]);
        else if (a=="--mtu" && i+1<argc) { int v=std::stoi(argv[++i]); if (v>200 && v<=2000) g_mtu_bytes.store((size_t)v); }
        else if (a=="--bitrate" && i+1<argc) { bitrate = std::max(200000, std::stoi(argv[++i])); }
        else if (a=="-h"||a=="--help") { usage(argv[0]); return 0; }
    }
    if (receiver_ports.empty()) { std::cerr<<"No receiver ports\n"; return 1; }

    std::atomic<int> target_bitrate{bitrate};

    AdaptiveUDPSender udp_sender(receiver_ip, receiver_ports);
    udp_sender.set_total_rate_bps(target_bitrate.load());
    udp_sender.enable_redundancy(2); // başlangıç için makul

    // === PROFILER THREAD ===
    UDPPortProfiler profiler(receiver_ip, receiver_ports);
    std::atomic<bool> run_prof{true};
    std::thread prof_thread([&]{
        int stable_ok=0, stable_bad=0;
        while (run_prof.load()) {
            profiler.send_probes();
            profiler.receive_replies_epoll(150);
            auto stats = profiler.get_stats();
            udp_sender.set_profiles(stats);
            g_last_stats = stats;

            double loss_sum=0.0, rtt_sum=0.0;
            for (auto& s: stats){ loss_sum+=s.packet_loss; rtt_sum+=s.avg_rtt_ms; }
            double avg_loss = stats.empty()?0.0:loss_sum/stats.size();
            (void)rtt_sum;

            // clone sayısını loss'a göre
            int new_red = 2;
            if (avg_loss > 0.10) new_red = 3;
            if (avg_loss > 0.20) new_red = 4;
            udp_sender.enable_redundancy(std::min(new_red, (int)receiver_ports.size()));

            // Dinamik MTU: kötüleşmede hızlı küçült, düzelmede yavaş büyüt
            size_t cur = g_mtu_bytes.load(), tgt = cur;
            if (avg_loss >= 0.12) { stable_bad++; stable_ok=0; }
            else if (avg_loss <= 0.02) { stable_ok++; stable_bad=0; }
            else { stable_ok=stable_bad=0; }

            if (stable_bad >= 1) {
                if      (cur > 800) tgt = 800;
                else if (cur > 600) tgt = 600;
                else if (cur > 480) tgt = 480;
                stable_bad = 0;
            }
            if (stable_ok >= 3) {
                if      (cur < 600) tgt = 600;
                else if (cur < 800) tgt = 800;
                else if (cur < 1000) tgt = 1000;
                stable_ok = 0;
            }
            if (tgt != cur) {
                g_mtu_bytes.store(tgt);
                std::cout << "[ABR] avg_loss=" << avg_loss << " -> MTU " << cur << " => " << tgt << "\n";
            }

            // pacing güncelle
            udp_sender.set_total_rate_bps(target_bitrate.load());
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });

    // === Kamera / Encoder ===
    Mat frame; VideoCapture cap;
    cap.open(DEVICE_ID, cv::CAP_V4L2);
    if (!cap.isOpened()) { std::cerr<<"camera open failed\n"; return -1; }
    cap.set(CAP_PROP_FOURCC, VideoWriter::fourcc('M','J','P','G'));
    cap.set(CAP_PROP_FRAME_WIDTH, WIDTH);
    cap.set(CAP_PROP_FRAME_HEIGHT, HEIGHT);
    cap.set(CAP_PROP_FPS, FPS);

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) { std::cerr<<"Codec not found\n"; return -1; }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->bit_rate = bitrate; ctx->width=WIDTH; ctx->height=HEIGHT;
    ctx->time_base = AVRational{1, FPS}; ctx->framerate = AVRational{FPS,1};
    ctx->gop_size = 7; ctx->max_b_frames = 0; ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->rc_buffer_size = bitrate*2; ctx->rc_max_rate = bitrate*2; ctx->rc_min_rate = std::max(bitrate/2, 400000);
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    ctx->thread_count = std::min<unsigned>(16, hw); ctx->thread_type = FF_THREAD_FRAME;

    av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(ctx->priv_data, "tune",   "zerolatency", 0);
    av_opt_set_int(ctx->priv_data, "rc_lookahead", 0, 0);
    av_opt_set(ctx->priv_data, "repeat-headers", "1", 0);
    av_opt_set(ctx->priv_data, "profile", "baseline", 0);
    av_opt_set_int(ctx->priv_data, "keyint", 7, 0);
    av_opt_set_int(ctx->priv_data, "min-keyint", 7, 0);
    av_opt_set(ctx->priv_data, "scenecut", "0", 0);
    av_opt_set(ctx->priv_data, "nal-hrd", "cbr", 0);

    if (avcodec_open2(ctx, codec, nullptr) < 0) { std::cerr<<"encoder open failed\n"; return -2; }

    AVFrame* yuv = av_frame_alloc();
    yuv->format = ctx->pix_fmt; yuv->width=ctx->width; yuv->height=ctx->height;
    av_image_alloc(yuv->data, yuv->linesize, WIDTH, HEIGHT, ctx->pix_fmt, 1);

    AVPacket* pkt = av_packet_alloc();
    AVPacket* pkt_f = av_packet_alloc();

    SwsContext* sws = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_BGR24, WIDTH, HEIGHT, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

    const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
    AVBSFContext* bsf_ctx = nullptr;
    if (bsf) {
        av_bsf_alloc(bsf, &bsf_ctx);
        avcodec_parameters_from_context(bsf_ctx->par_in, ctx);
        bsf_ctx->time_base_in = ctx->time_base;
        if (av_bsf_init(bsf_ctx) < 0) { av_bsf_free(&bsf_ctx); bsf_ctx=nullptr; }
    }

    int frame_cnt=0,fps_cnt=0; auto t0=chrono::high_resolution_clock::now();
    while (cap.read(frame)) {
        fps_cnt++; frame_cnt++;
        auto t1 = chrono::high_resolution_clock::now();
        if (chrono::duration_cast<chrono::seconds>(t1 - t0).count() >= 1) {
            std::cout << "Measured FPS: " << fps_cnt << "\n"; fps_cnt=0; t0=t1;
        }

        const uint8_t* src[] = { frame.data };
        int stride[] = { (int)frame.step };
        sws_scale(sws, src, stride, 0, HEIGHT, yuv->data, yuv->linesize);
        yuv->pts = frame_cnt;

        avcodec_send_frame(ctx, yuv);
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (is_key) udp_sender.enable_redundancy(std::min<int>(4, std::max<int>(2,(int)receiver_ports.size()/2)));

            const uint8_t* send_data = pkt->data;
            size_t send_size = (size_t)pkt->size;

            if (bsf_ctx) {
                if (av_bsf_send_packet(bsf_ctx, pkt) == 0) {
                    while (av_bsf_receive_packet(bsf_ctx, pkt_f) == 0) {
                        send_data = pkt_f->data; send_size = (size_t)pkt_f->size;
                        is_key = (pkt_f->flags & AV_PKT_FLAG_KEY) != 0;

                        auto built = build_slices_with_fec(send_data, send_size, g_mtu_bytes.load(), (uint32_t)frame_cnt, is_key);
                        if (!built.data_slices.empty())   udp_sender.send_slices_parallel(built.data_slices);
                        if (!built.parity_slices.empty()) udp_sender.send_slices_parallel(built.parity_slices);
                        av_packet_unref(pkt_f);
                    }
                }
            } else {
                auto built = build_slices_with_fec(send_data, send_size, g_mtu_bytes.load(), (uint32_t)frame_cnt, is_key);
                if (!built.data_slices.empty())   udp_sender.send_slices_parallel(built.data_slices);
                if (!built.parity_slices.empty()) udp_sender.send_slices_parallel(built.parity_slices);
            }
            av_packet_unref(pkt);
        }

        imshow("Video", frame);
        if (waitKey(1) >= 0) break;
    }

    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    sws_freeContext(sws);
    av_frame_free(&yuv);
    av_packet_free(&pkt);
    av_packet_free(&pkt_f);
    avcodec_free_context(&ctx);
    cap.release(); destroyAllWindows();

    run_prof.store(false);
    if (prof_thread.joinable()) prof_thread.join();
    return 0;
}
