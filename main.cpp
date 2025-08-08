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


// k ve r dinamik hesaplanacak, burada sabitler kullanılmıyor

static size_t mtu = 1000; // Fixed size buffer - no fragmentation issues

using namespace cv;
using namespace std;

// Son profil istatistiklerini FEC r hesaplamasında kullanmak için global buffer
std::vector<UDPChannelStat> g_last_stats;

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
    uint8_t  flags = 0; // bit0: parity(1)/data(0), bit1: keyframe, bit2: critical_packet
    uint32_t checksum = 0; // FNV-1a over payload_bytes
};
#pragma pack(pop)
static_assert(sizeof(SliceHeader) >= 1, "SliceHeader sanity");

std::vector<std::vector<uint8_t>> slice_and_pad(const uint8_t* data, size_t size, size_t mtu) {
    std::vector<std::vector<uint8_t>> chunks;
    size_t offset = 0;

    while (offset < size) {
        size_t len = std::min(mtu, size - offset);
        std::vector<uint8_t> chunk(mtu, 0); // always full MTU size
        std::memcpy(chunk.data(), data + offset, len);
        chunks.emplace_back(std::move(chunk));
        offset += len;
    }

    return chunks;
}

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
    extern std::vector<UDPChannelStat> g_last_stats; // aşağıda güncellenecek
    double avg_loss = 0.0;
    if (!g_last_stats.empty()) {
        for (const auto& s : g_last_stats) avg_loss += s.packet_loss;
        avg_loss /= g_last_stats.size();
    }
    // Balanced FEC for real network - not too aggressive
    // Base 20% redundancy, scale with actual loss
    double base_redundancy = 0.20; // 20% base redundancy
    double loss_factor = avg_loss > 0.01 ? avg_loss : 0.01; // min 1% assumed loss
    double fec_ratio = std::min(0.5, base_redundancy + loss_factor * 1.5);
    r_parity = std::clamp(static_cast<int>(std::ceil(k_data * fec_ratio)), 
                         2, // min 2 parity blocks
                         std::max(4, k_data / 2)); // max 50% redundancy
    
    // Moderate extra for keyframes
    if (is_keyframe) {
        r_parity = std::min(r_parity + 2, k_data * 2 / 3); // max 66% for keyframes
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
        // compute checksum over payload area we will fill
        // first write header, then payload, then update checksum
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
    int bitrate = 2500000; // Start high bitrate - no more conservative bullshit
    std::atomic<int> target_bitrate{bitrate};
    int64_t counter = 0;

    std::string receiver_ip = "192.168.1.100"; // default target
    std::vector<uint16_t> receiver_ports = {4000, 4001, 4002}; // Multiple tunnels for redundancy

    // Parse CLI args
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ip" && i + 1 < argc) {
            receiver_ip = argv[++i];
        } else if (arg == "--ports" && i + 1 < argc) {
            receiver_ports = parse_ports_csv(argv[++i]);
        } else if (arg == "--mtu" && i + 1 < argc) {
            int v = std::stoi(argv[++i]);
            if (v > 200 && v <= 2000) mtu = static_cast<size_t>(v);
        } else if (arg == "-h" || arg == "--help") {
            print_usage_sender(argv[0]);
            return 0;
        }
    }
    if (receiver_ports.empty()) {
        std::cerr << "No receiver ports specified.\n";
        print_usage_sender(argv[0]);
        return 1;
    }

    AdaptiveUDPSender udp_sender(receiver_ip, receiver_ports);
    
    // Global paylaşımlı son profil istatistikleri
    

    // Clone packets to multiple tunnels for redundancy
    udp_sender.enable_redundancy(2); // Send each packet through 2 different tunnels

    // thread-safe queue for slice buffers
    struct Packet {
        std::vector<uint8_t> data;
    };
    class PacketQ {
        std::queue<Packet> q; std::mutex m; std::condition_variable cv;
    public:
        void push(Packet &&p){ std::lock_guard<std::mutex> lk(m); q.push(std::move(p)); cv.notify_one(); }
        bool pop(Packet &out){ std::unique_lock<std::mutex> lk(m); cv.wait(lk,[&]{return !q.empty();}); out = std::move(q.front()); q.pop(); return true; }
    } packet_q;

    // live profiler thread to feed sender with channel stats
    std::atomic<bool> run_sender{true};
    std::thread sender_thread([&](){
        Packet pktv;
        while(true){
            packet_q.pop(pktv);
            if(!run_sender.load() && pktv.data.empty()) break;
            if(pktv.data.empty()) continue;
            std::vector<std::vector<uint8_t>> one{std::move(pktv.data)};
            udp_sender.send_slices(one);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    UDPPortProfiler profiler(receiver_ip, receiver_ports);
    std::atomic<bool> run_profiler{true};
    std::thread profiler_thread([&](){
        while (run_profiler.load()) {
            profiler.send_probes();
            profiler.receive_replies_epoll(150);
            auto stats = profiler.get_stats();
            udp_sender.set_profiles(stats);
            // Global istatistikleri güncelle
            g_last_stats = stats;

            // Less aggressive bitrate adaptation - stabilized for smooth playback
            double loss_sum = 0.0;
            double rtt_sum = 0.0;
            for (const auto& s : stats) { loss_sum += s.packet_loss; rtt_sum += s.avg_rtt_ms; }
            double avg_loss = stats.empty() ? 0.0 : loss_sum / stats.size();
            double avg_rtt = stats.empty() ? 0.0 : rtt_sum / stats.size();

            int cur = target_bitrate.load();
            int new_bitrate = cur;

            // Reed-Solomon handles packet loss - only reduce bitrate in extreme cases
            if (avg_loss > 0.80) new_bitrate = std::max(cur * 85 / 100, 2000000);      // Only extreme loss
            else if (avg_loss < 0.01) new_bitrate = std::min(cur * 102 / 100, 3500000); // Very slow increase

            // No redundancy changes - FEC handles everything
            // Single port = no cloning needed

            // Only change bitrate if huge difference (>500kbps) - let FEC handle the rest
            if (abs(new_bitrate - cur) > 500000) {
                target_bitrate.store(new_bitrate);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(3000)); // Update every 3 seconds - very sparse
        }
    });


    cap.open(Device_ID, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        cout << "Error opening video device" << endl;
        return -1;
    }
    // Önce dört‐character code’u MJPG olarak ayarlayın
    cap.set(CAP_PROP_FOURCC, VideoWriter::fourcc('M','J','P','G'));
    cap.set(CAP_PROP_FRAME_WIDTH, WIDTH);
    cap.set(CAP_PROP_FRAME_HEIGHT, HEIGHT);
    cap.set(CAP_PROP_FPS, FPS);

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec)
    {
        cout << "Codec not found" << endl;
        return -1;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    int current_bitrate = bitrate;
    ctx->bit_rate = current_bitrate;
    ctx->width = WIDTH;
    ctx->height = HEIGHT;
    ctx->time_base = AVRational{1, FPS};
    ctx->framerate = AVRational{FPS, 1};
    ctx->gop_size = 7; // çok kısa GOP (yaklaşık 7 frame'de bir IDR)
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP; // bağımsız GOP’lar
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->rc_buffer_size = current_bitrate * 2;
    ctx->rc_max_rate    = current_bitrate * 2;
    ctx->rc_min_rate    = std::max(current_bitrate / 2, 400000);
    ctx->thread_count = 4; // CPU baskısını azaltarak sabit performans
    ctx->thread_type = FF_THREAD_FRAME;

    av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(ctx->priv_data, "tune",   "zerolatency", 0);
    av_opt_set_int(ctx->priv_data, "rc_lookahead", 0, 0);
    av_opt_set(ctx->priv_data, "repeat-headers", "1", 0);
    av_opt_set(ctx->priv_data, "profile", "baseline", 0);
    // Stabil ve kısa GOP
    av_opt_set_int(ctx->priv_data, "keyint", 7, 0);
    av_opt_set_int(ctx->priv_data, "min-keyint", 7, 0);
    av_opt_set(ctx->priv_data, "scenecut", "0", 0);
    av_opt_set(ctx->priv_data, "nal-hrd", "cbr", 0);

    int encoder_API = avcodec_open2(ctx, codec, nullptr);
    if (encoder_API < 0)
    {
        cerr<<"Error opening codec"<<endl;
        return -2;
    }

    AVFrame* yuv = av_frame_alloc();
    yuv->format = ctx->pix_fmt;
    yuv->width  = ctx->width;
    yuv->height = ctx->height;
    av_image_alloc(yuv->data, yuv->linesize, WIDTH, HEIGHT, ctx->pix_fmt, 1);

    AVPacket* pkt = av_packet_alloc();
    AVPacket* pkt_filtered = av_packet_alloc();

    //OpenCV -->BGR input --Translation via SwsContext-->YUV420P -->AVFrame
    SwsContext* sws = sws_getContext(
        WIDTH, HEIGHT, AV_PIX_FMT_BGR24,
        WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );
    int frame_count = 0;
    uint32_t tx_unit_id = 0; // her encoder çıktısı için benzersiz ID
    int fps_counter = 0;
    auto t0 = chrono::high_resolution_clock::now();

    // Initialize H.264 bitstream filter to ensure Annex B format
    const AVBitStreamFilter* bsf_filter = av_bsf_get_by_name("h264_mp4toannexb");
    AVBSFContext* bsf_ctx = nullptr;
    if (bsf_filter) {
        av_bsf_alloc(bsf_filter, &bsf_ctx);
        avcodec_parameters_from_context(bsf_ctx->par_in, ctx);
        bsf_ctx->time_base_in = ctx->time_base;
        if (av_bsf_init(bsf_ctx) < 0) {
            std::cerr << "Failed to init h264_mp4toannexb bitstream filter" << std::endl;
            av_bsf_free(&bsf_ctx);
            bsf_ctx = nullptr;
        }
    }

    while(cap.read(frame)){
        // dynamic bitrate adjustment (conservative: keep high floor)
        if (target_bitrate.load() != current_bitrate) {
            current_bitrate = target_bitrate.load();
            ctx->bit_rate = current_bitrate;
            ctx->rc_buffer_size = current_bitrate * 2;
            ctx->rc_max_rate = current_bitrate * 2;
            ctx->rc_min_rate = std::max(current_bitrate / 2, 400000);
            av_opt_set_int(ctx->priv_data, "b", current_bitrate, 0);
            av_opt_set_int(ctx->priv_data, "vbv-maxrate", current_bitrate, 0);
            av_opt_set_int(ctx->priv_data, "vbv-bufsize", current_bitrate, 0);
        }
        fps_counter++;
        frame_count++;
        std::vector<std::vector<uint8_t>> chunks;
        auto t1 = chrono::high_resolution_clock::now();
        if (chrono::duration_cast<chrono::seconds>(t1 - t0).count() >= 1) {
            cout << "Measured FPS: " << fps_counter << endl;
            fps_counter = 0;
            t0 = t1;
        }
        //cv::Mat (BGR) ----[sws_scale]---> AVFrame (YUV420P)
        const uint8_t* src_slice[] = {frame.data};
        int src_stride[] = {static_cast<int>(frame.step)};
        sws_scale(sws, src_slice, src_stride, 0, HEIGHT, yuv->data, yuv->linesize);

        //Encoder a gönder
        yuv->pts = frame_count;

        int raw_size = frame.total() * frame.elemSize(); // BGR frame boyutu (ham)
        avcodec_send_frame(ctx, yuv);

        while (avcodec_receive_packet(ctx, pkt) == 0) {
            int encoded_size = pkt->size;

            bool is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (is_key) {
                // Anahtar karelerde FEC oranını arttırmak için global loss’a ek margin ver
                // g_last_stats zaten r içinde dikkate alınıyor; burada opsiyonel olarak redundancy artırabiliriz
                udp_sender.enable_redundancy( std::min<int>(3, std::max<int>(2, (int)receiver_ports.size()/2)) );
            }
            const uint8_t* send_data = pkt->data;
            size_t send_size = static_cast<size_t>(pkt->size);
            AVPacket* use_pkt = pkt;

            if (bsf_ctx) {
                // Run through bitstream filter to convert to Annex B
                if (av_bsf_send_packet(bsf_ctx, pkt) == 0) {
                    while (av_bsf_receive_packet(bsf_ctx, pkt_filtered) == 0) {
                        send_data = pkt_filtered->data;
                        send_size = static_cast<size_t>(pkt_filtered->size);
                        is_key = (pkt_filtered->flags & AV_PKT_FLAG_KEY) != 0;
                        BuiltSlices built = build_slices_with_fec(send_data,
                                                                 send_size,
                                                                 mtu,
                                                                 tx_unit_id++,
                                                                 is_key);
                        cout << "Frame #" << frame_count
                             << " | Raw: " << raw_size << " bytes"
                             << " -> Encoded: " << encoded_size << " bytes"
                             << " | k=" << built.k
                             << " r=" << built.r
                             << " | total=" << (built.k + built.r)
                             << endl;
                        // Scatter data slices deterministically; parity slices weighted
                        if (!built.data_slices.empty()) {
                            udp_sender.send_slices_parallel(built.data_slices);
                        }
                        if (!built.parity_slices.empty()) {
                            for(auto &sl: built.parity_slices) packet_q.push(Packet{std::move(sl)});
                        }
                        av_packet_unref(pkt_filtered);
                    }
                }
            } else {
                BuiltSlices built = build_slices_with_fec(send_data,
                                                         send_size,
                                                         mtu,
                                                         tx_unit_id++,
                                                         is_key);
                cout << "Frame #" << frame_count
                     << " | Raw: " << raw_size << " bytes"
                     << " -> Encoded: " << encoded_size << " bytes"
                     << " | k=" << built.k
                     << " r=" << built.r
                     << " | total=" << (built.k + built.r)
                     << endl;
                if (!built.data_slices.empty()) udp_sender.send_slices_parallel(built.data_slices);
                if (!built.parity_slices.empty()) {
                    for(auto &sl: built.parity_slices) packet_q.push(Packet{std::move(sl)});
                }
            }

            av_packet_unref(pkt);
        }

        imshow("Video", frame);
        if(waitKey(5) >= 0 ){
            break;
        }
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
    packet_q.push(Packet{}); // unblock
    if (sender_thread.joinable()) sender_thread.join();
    if (profiler_thread.joinable()) profiler_thread.join();

    return 0;

}