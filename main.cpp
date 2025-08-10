#include <iostream>
#include <vector>
#include <cstdint>
#include <cstddef>

#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <sstream>
#include <string>
#include <cmath>
#include <unordered_map>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <pthread.h>   // <-- eklendi (CPU affinity için)
#include <sched.h>     // <-- eklendi

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

#define Device_ID 0
#define WIDTH 640
#define HEIGHT 480
#define FPS 30

// MTU sabit: fragmentation önleme
static size_t mtu = 1000;

using namespace cv;
using namespace std;

// Son profil istatistiklerini FEC r hesaplamasında kullanmak için global buffer
std::vector<UDPChannelStat> g_last_stats;

// (şu an kontrol kanalı kullanılmıyor ama header dursun)
static constexpr uint16_t CTRL_NACK_PORT = 49000;
#pragma pack(push, 1)
struct NackPacket {
    uint32_t magic = 0x4E41434Bu; // 'NACK'
    uint32_t frame_id = 0;
    uint16_t missing_count = 0;
    uint16_t indices[128]{}; // up to 128 missing data slice indices
};
#pragma pack(pop)

// === Transmission slice header and FEC builder ===
#pragma pack(push, 1)
struct SliceHeader {
    uint32_t magic = 0xABCD1234;
    uint32_t frame_id = 0;
    uint16_t slice_index = 0;
    uint16_t total_slices = 0; // k + r
    uint16_t k_data = 0;
    uint16_t r_parity = 0;
    uint16_t payload_bytes = 0; // payload size used for RS
    uint32_t total_frame_bytes = 0; // encoded size of this block
    uint64_t timestamp_us = 0;
    uint8_t  flags = 0; // bit0: parity(1)/data(0), bit1: keyframe
    uint32_t checksum = 0; // FNV-1a over payload_bytes
};
#pragma pack(pop)
static_assert(sizeof(SliceHeader) >= 1, "SliceHeader sanity");

struct BuiltSlices {
    std::vector<std::vector<uint8_t>> data_slices;
    std::vector<std::vector<uint8_t>> parity_slices;
    int k = 0;
    int r = 0;
};

static inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}

BuiltSlices build_slices_with_fec(const uint8_t* data, size_t size, size_t mtu_bytes, uint32_t frame_id, bool is_keyframe) {
    BuiltSlices out{};
    const size_t header_size = sizeof(SliceHeader);
    if (mtu_bytes <= header_size) return out;

    const size_t payload_size = mtu_bytes - header_size;
    int k_data = static_cast<int>((size + payload_size - 1) / payload_size);
    if (k_data <= 0) k_data = 1;

    // Dinamik r belirleme: son profil metriklerine göre
    int r_parity = 1;
    double avg_loss = 0.0;
    if (!g_last_stats.empty()) {
        for (const auto& s : g_last_stats) avg_loss += s.packet_loss;
        avg_loss /= g_last_stats.size();
    }
    // Base 20% + loss'a göre artış, hard cap 50%
    double base_redundancy = 0.20;
    double loss_factor = avg_loss > 0.01 ? avg_loss : 0.01;
    double fec_ratio = std::min(0.5, base_redundancy + loss_factor * 1.5);
    r_parity = std::clamp(static_cast<int>(std::ceil(k_data * fec_ratio)),
                          2, std::max(4, k_data / 2));
    if (is_keyframe) {
        r_parity = std::min(r_parity + 2, k_data * 2 / 3);
    }

    out.k = k_data; out.r = r_parity;

    auto fnv1a = [](const uint8_t* p, size_t n){
        uint32_t h = 2166136261u;
        for (size_t i=0;i<n;++i){ h ^= p[i]; h *= 16777619u; }
        return h;
    };

    out.data_slices.resize(k_data);
    size_t src_off = 0;
    for (int i = 0; i < k_data; ++i) {
        out.data_slices[i].assign(mtu_bytes, 0);
        SliceHeader hdr{};
        hdr.frame_id = frame_id;
        hdr.slice_index = static_cast<uint16_t>(i);
        hdr.total_slices = static_cast<uint16_t>(k_data + r_parity);
        hdr.k_data = static_cast<uint16_t>(k_data);
        hdr.r_parity = static_cast<uint16_t>(r_parity);
        hdr.payload_bytes = static_cast<uint16_t>(payload_size);
        hdr.total_frame_bytes = static_cast<uint32_t>(size);
        hdr.timestamp_us = now_us();
        hdr.flags = is_keyframe ? 0x02 : 0x00; // bit1: keyframe
        std::memcpy(out.data_slices[i].data(), &hdr, sizeof(SliceHeader));

        size_t remain = (src_off < size) ? (size - src_off) : 0;
        size_t copy_len = std::min(payload_size, remain);
        if (copy_len > 0) {
            std::memcpy(out.data_slices[i].data() + header_size, data + src_off, copy_len);
            hdr.checksum = fnv1a(out.data_slices[i].data() + header_size, payload_size);
            std::memcpy(out.data_slices[i].data(), &hdr, sizeof(SliceHeader));
            src_off += copy_len;
        } else {
            hdr.checksum = fnv1a(out.data_slices[i].data() + header_size, payload_size);
            std::memcpy(out.data_slices[i].data(), &hdr, sizeof(SliceHeader));
        }
    }

    if (k_data >= 2) {
        std::vector<uint8_t*> data_ptrs(static_cast<size_t>(k_data));
        for (int i = 0; i < k_data; ++i) data_ptrs[i] = out.data_slices[i].data() + header_size;

        std::vector<std::vector<uint8_t>> parity_payloads(r_parity, std::vector<uint8_t>(payload_size));
        std::vector<uint8_t*> parity_ptrs(static_cast<size_t>(r_parity));
        for (int i = 0; i < r_parity; ++i) parity_ptrs[i] = parity_payloads[i].data();

        std::vector<uint8_t> matrix(static_cast<size_t>(k_data + r_parity) * static_cast<size_t>(k_data));
        std::vector<uint8_t> g_tbls(static_cast<size_t>(k_data) * static_cast<size_t>(r_parity) * 32);
        gf_gen_rs_matrix(matrix.data(), k_data + r_parity, k_data);
        ec_init_tables(k_data, r_parity, matrix.data() + k_data * k_data, g_tbls.data());
        ec_encode_data(static_cast<int>(payload_size), k_data, r_parity, g_tbls.data(), data_ptrs.data(), parity_ptrs.data());

        out.parity_slices.resize(r_parity);
        for (int i = 0; i < r_parity; ++i) {
            out.parity_slices[i].assign(mtu_bytes, 0);
            SliceHeader hdr{};
            hdr.frame_id = frame_id;
            hdr.slice_index = static_cast<uint16_t>(k_data + i);
            hdr.total_slices = static_cast<uint16_t>(k_data + r_parity);
            hdr.k_data = static_cast<uint16_t>(k_data);
            hdr.r_parity = static_cast<uint16_t>(r_parity);
            hdr.payload_bytes = static_cast<uint16_t>(payload_size);
            hdr.total_frame_bytes = static_cast<uint32_t>(size);
            hdr.timestamp_us = now_us();
            hdr.flags = static_cast<uint8_t>(0x01 | (is_keyframe ? 0x02 : 0x00)); // parity + keyframe
            std::memcpy(out.parity_slices[i].data(), &hdr, sizeof(SliceHeader));
            std::memcpy(out.parity_slices[i].data() + header_size, parity_payloads[i].data(), payload_size);
            hdr.checksum = fnv1a(out.parity_slices[i].data() + header_size, payload_size);
            std::memcpy(out.parity_slices[i].data(), &hdr, sizeof(SliceHeader));
        }
    }

    return out;
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

static void print_usage_sender(const char* prog) {
    std::cout << "Usage: " << prog << " --ip <receiver_ip> --ports <p1,p2,...> [--mtu <bytes>]" << std::endl;
}

int main(int argc, char** argv){
    Mat frame;
    VideoCapture cap;
    int bitrate = 5'000'000; // 5 Mbps
    std::atomic<int> target_bitrate{bitrate};

    std::string receiver_ip = "192.168.1.100"; // default target
    std::vector<uint16_t> receiver_ports = {4000, 4001, 4002, 4003, 4004};

    // CLI
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc) {
            receiver_ip = argv[++i];
        } else if (arg == "--ports" && i + 1 < argc) {
            receiver_ports = parse_ports_csv(argv[++i]);
        } else if (arg == "--mtu" && i + 1 < argc) {
            int v = std::stoi(argv[++i]);
            if (v > 200 && v <= 2000) mtu = (size_t)v;
        } else if (arg == "-h" || arg == "--help") {
            print_usage_sender(argv[0]); return 0;
        }
    }
    if (receiver_ports.empty()) {
        std::cerr << "No receiver ports specified.\n";
        print_usage_sender(argv[0]);
        return 1;
    }

    AdaptiveUDPSender udp_sender(receiver_ip, receiver_ports);
    // toplam bitrate’i pacing’e bildir
    udp_sender.set_total_rate_bps(target_bitrate.load());

    // Redundancy başlangıcı (profilere göre aşağıda dinamik güncelleniyor)
    udp_sender.enable_redundancy(4);

    // thread-safe queue (şu an aktif kullanılmıyor ama kalabilir)
    struct Packet { std::vector<uint8_t> data; };
    class PacketQ {
        std::queue<Packet> q; std::mutex m; std::condition_variable cv;
    public:
        void push(Packet &&p){ std::lock_guard<std::mutex> lk(m); q.push(std::move(p)); cv.notify_one(); }
        bool pop(Packet &out){ std::unique_lock<std::mutex> lk(m); cv.wait(lk,[&]{return !q.empty();}); out = std::move(q.front()); q.pop(); return true; }
    } packet_q;

    // sender thread (şu an kullanılmıyor; yine de bırakıyoruz)
    std::atomic<bool> run_sender{true};
    std::thread sender_thread([&](){
        cpu_set_t cp; CPU_ZERO(&cp); CPU_SET(0, &cp);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cp);
        Packet pktv;
        while(true){
            packet_q.pop(pktv);
            if(!run_sender.load() && pktv.data.empty()) break;
            if(pktv.data.empty()) continue;
            std::vector<std::vector<uint8_t>> one{std::move(pktv.data)};
            udp_sender.send_slices(one);
        }
    });

    // profiler
    UDPPortProfiler profiler(receiver_ip, receiver_ports);
    std::atomic<bool> run_profiler{true};
    std::thread profiler_thread([&](){
        cpu_set_t cp; CPU_ZERO(&cp); CPU_SET(1, &cp);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cp);
        while (run_profiler.load()) {
            profiler.send_probes();
            profiler.receive_replies_epoll(150);
            auto stats = profiler.get_stats();
            udp_sender.set_profiles(stats);
            g_last_stats = stats;

            // redundancy’yi loss’a göre ayarla
            double loss_sum = 0.0;
            for (const auto& s : stats) loss_sum += s.packet_loss;
            double avg_loss = stats.empty() ? 0.0 : loss_sum / stats.size();

            int new_redundancy = 1;
            if (avg_loss > 0.10) new_redundancy = 4;
            else if (avg_loss > 0.05) new_redundancy = 3;
            else new_redundancy = 2;

            udp_sender.enable_redundancy(new_redundancy);

            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        }
    });

    // Kamera
    cap.open(Device_ID, cv::CAP_V4L2);
    if (!cap.isOpened()) { cout << "Error opening video device" << endl; return -1; }
    cap.set(CAP_PROP_FOURCC, VideoWriter::fourcc('M','J','P','G'));
    cap.set(CAP_PROP_FRAME_WIDTH, WIDTH);
    cap.set(CAP_PROP_FRAME_HEIGHT, HEIGHT);
    cap.set(CAP_PROP_FPS, FPS);

    // Encoder
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec){ cout << "Codec not found" << endl; return -1; }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    int current_bitrate = bitrate;
    ctx->bit_rate = current_bitrate;
    ctx->width = WIDTH; ctx->height = HEIGHT;
    ctx->time_base = AVRational{1, FPS};
    ctx->framerate = AVRational{FPS, 1};
    ctx->gop_size = 7; ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->rc_buffer_size = current_bitrate * 2;
    ctx->rc_max_rate    = current_bitrate * 2;
    ctx->rc_min_rate    = std::max(current_bitrate / 2, 400000);
    unsigned hw_threads = std::max(2u, std::thread::hardware_concurrency());
    ctx->thread_count = std::min<unsigned>(16, hw_threads);
    ctx->thread_type = FF_THREAD_FRAME;

    av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(ctx->priv_data, "tune",   "zerolatency", 0);
    av_opt_set_int(ctx->priv_data, "rc_lookahead", 0, 0);
    av_opt_set(ctx->priv_data, "repeat-headers", "1", 0);
    av_opt_set(ctx->priv_data, "profile", "baseline", 0);
    av_opt_set_int(ctx->priv_data, "keyint", 7, 0);
    av_opt_set_int(ctx->priv_data, "min-keyint", 7, 0);
    av_opt_set(ctx->priv_data, "scenecut", "0", 0);
    av_opt_set(ctx->priv_data, "nal-hrd", "cbr", 0);

    if (avcodec_open2(ctx, codec, nullptr) < 0){ cerr<<"Error opening codec"<<endl; return -2; }

    AVFrame* yuv = av_frame_alloc();
    yuv->format = ctx->pix_fmt; yuv->width  = ctx->width; yuv->height = ctx->height;
    av_image_alloc(yuv->data, yuv->linesize, WIDTH, HEIGHT, ctx->pix_fmt, 1);

    AVPacket* pkt = av_packet_alloc();
    AVPacket* pkt_filtered = av_packet_alloc();

    SwsContext* sws = sws_getContext(
        WIDTH, HEIGHT, AV_PIX_FMT_BGR24,
        WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    int frame_count = 0;
    uint32_t tx_unit_id = 0; // her encoder çıktısı için benzersiz ID
    int fps_counter = 0;
    auto t0 = chrono::high_resolution_clock::now();

    // Annex B garantisi
    const AVBitStreamFilter* bsf_filter = av_bsf_get_by_name("h264_mp4toannexb");
    AVBSFContext* bsf_ctx = nullptr;
    if (bsf_filter) {
        av_bsf_alloc(bsf_filter, &bsf_ctx);
        avcodec_parameters_from_context(bsf_ctx->par_in, ctx);
        bsf_ctx->time_base_in = ctx->time_base;
        if (av_bsf_init(bsf_ctx) < 0) {
            std::cerr << "Failed to init h264_mp4toannexb bitstream filter" << std::endl;
            av_bsf_free(&bsf_ctx); bsf_ctx = nullptr;
        }
    }

    while(cap.read(frame)){
        fps_counter++;
        frame_count++;
        auto t1 = chrono::high_resolution_clock::now();
        if (chrono::duration_cast<chrono::seconds>(t1 - t0).count() >= 1) {
            cout << "Measured FPS: " << fps_counter << endl;
            fps_counter = 0; t0 = t1;
        }

        // BGR -> YUV420P
        const uint8_t* src_slice[] = {frame.data};
        int src_stride[] = {static_cast<int>(frame.step)};
        sws_scale(sws, src_slice, src_stride, 0, HEIGHT, yuv->data, yuv->linesize);
        yuv->pts = frame_count;

        avcodec_send_frame(ctx, yuv);

        while (avcodec_receive_packet(ctx, pkt) == 0) {
            bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (is_key) {
                // keyframe'lerde clone sayısını biraz yükselt
                udp_sender.enable_redundancy(std::min<int>(4, std::max<int>(3, (int)receiver_ports.size()/2)));
            }

            const uint8_t* send_data = pkt->data;
            size_t send_size = (size_t)pkt->size;

            if (bsf_ctx) {
                if (av_bsf_send_packet(bsf_ctx, pkt) == 0) {
                    while (av_bsf_receive_packet(bsf_ctx, pkt_filtered) == 0) {
                        send_data = pkt_filtered->data;
                        send_size = (size_t)pkt_filtered->size;
                        is_key = (pkt_filtered->flags & AV_PKT_FLAG_KEY) != 0;

                        BuiltSlices built = build_slices_with_fec(send_data, send_size, mtu, tx_unit_id++, is_key);
                        if (!built.data_slices.empty())   udp_sender.send_slices_parallel(built.data_slices);
                        if (!built.parity_slices.empty()) udp_sender.send_slices_parallel(built.parity_slices);

                        av_packet_unref(pkt_filtered);
                    }
                }
            } else {
                BuiltSlices built = build_slices_with_fec(send_data, send_size, mtu, tx_unit_id++, is_key);
                if (!built.data_slices.empty())   udp_sender.send_slices_parallel(built.data_slices);
                if (!built.parity_slices.empty()) udp_sender.send_slices_parallel(built.parity_slices);
            }

            av_packet_unref(pkt);
        }

        imshow("Video", frame);
        if(waitKey(5) >= 0 ){ break; }
    }

    av_frame_free(&yuv);
    av_packet_free(&pkt);
    av_packet_free(&pkt_filtered);
    if (bsf_ctx) av_bsf_free(&bsf_ctx);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    cap.release();
    destroyAllWindows();

    // stop profiler thread
    run_profiler.store(false);
    run_sender.store(false);
    // sender thread’i unblock et
    struct Packet { std::vector<uint8_t> data; };
    // zaten üstte aynı struct tanımlı; burada sadece unblock için boş push etmiştik
    // packet_q.push(Packet{}); // eğer queue’yu hiç kullanmıyorsan yoruma alabilirsin

    if (sender_thread.joinable()) sender_thread.join();
    if (profiler_thread.joinable()) profiler_thread.join();

    return 0;
}
