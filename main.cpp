#include <iostream>
#include <vector>
#include <cstdint>
#include <cstddef>

#include <algorithm>
#include <thread>
#include <atomic>
#include <chrono>

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
}


#define Device_ID 0

#define WIDTH 640
#define HEIGHT 480
#define FPS 30


const int k = 20;
const int r = 5;

static size_t mtu = 1200;

using namespace cv;
using namespace std;

// === Transmission slice header and FEC builder ===
#pragma pack(push, 1)
struct SliceHeader {
    uint32_t magic = 0xABCD1234;
    uint32_t frame_id = 0;
    uint16_t slice_index = 0;
    uint16_t total_slices = 0; // k + r
    uint16_t k_data = 0;
    uint16_t r_parity = 0;
    uint64_t timestamp_us = 0;
    uint8_t  flags = 0; // bit0: parity(1)/data(0), bit1: keyframe
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

BuiltSlices build_slices_with_fec(const uint8_t* data, size_t size, size_t mtu_bytes, uint32_t frame_id) {
    BuiltSlices out{};
    const size_t header_size = sizeof(SliceHeader);
    if (mtu_bytes <= header_size) return out;

    const size_t payload_size = mtu_bytes - header_size;
    int k_data = static_cast<int>((size + payload_size - 1) / payload_size);
    if (k_data <= 0) k_data = 1;
    int r_parity = std::clamp(k_data / 4, 1, 10);
    out.k = k_data; out.r = r_parity;

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
        hdr.timestamp_us = now_us();
        hdr.flags = 0;
        std::memcpy(out.data_slices[i].data(), &hdr, sizeof(SliceHeader));

        size_t remain = (src_off < size) ? (size - src_off) : 0;
        size_t copy_len = std::min(payload_size, remain);
        if (copy_len > 0) {
            std::memcpy(out.data_slices[i].data() + header_size, data + src_off, copy_len);
            src_off += copy_len;
        }
    }

    if (k_data >= 2) {
        std::vector<uint8_t*> data_ptrs(static_cast<size_t>(k_data));
        for (int i = 0; i < k_data; ++i) data_ptrs[i] = out.data_slices[i].data() + header_size;

        std::vector<std::vector<uint8_t>> parity_payloads(r_parity, std::vector<uint8_t>(payload_size));
        std::vector<uint8_t*> parity_ptrs(static_cast<size_t>(r_parity));
        for (int i = 0; i < r_parity; ++i) parity_ptrs[i] = parity_payloads[i].data();

        std::vector<uint8_t> matrix(static_cast<size_t>(k_data + r_parity) * static_cast<size_t>(k_data));
        std::vector<uint8_t> g_tbls(32 * 1024);
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
            hdr.timestamp_us = now_us();
            hdr.flags = 0x01; // parity
            std::memcpy(out.parity_slices[i].data(), &hdr, sizeof(SliceHeader));
            std::memcpy(out.parity_slices[i].data() + header_size, parity_payloads[i].data(), payload_size);
        }
    }

    return out;
}



int main(){
    Mat frame;
    VideoCapture cap;
    int bitrate = 400000;//->Siktiğimin şeyini Adaptive Yapıcam sonra
    int64_t counter = 0;

    std::string receiver_ip = "192.168.1.100"; // Gerçek hedefin IP’si
    std::vector<uint16_t> receiver_ports = {4000, 4001, 4002, 4003, 4004};

    AdaptiveUDPSender udp_sender(receiver_ip, receiver_ports);

    // default redundancy for data slices
    udp_sender.enable_redundancy(2);

    // live profiler thread to feed sender with channel stats
    UDPPortProfiler profiler(receiver_ip, receiver_ports);
    std::atomic<bool> run_profiler{true};
    std::thread profiler_thread([&](){
        while (run_profiler.load()) {
            profiler.send_probes();
            profiler.receive_replies_epoll(150);
            udp_sender.set_profiles(profiler.get_stats());
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
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
    ctx->bit_rate = bitrate;
    ctx->width = WIDTH;
    ctx->height = HEIGHT;
    ctx->time_base = AVRational{1, FPS};
    ctx->framerate = AVRational{FPS, 1};
    ctx->gop_size = 10;
    ctx->max_b_frames = 0;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->rc_buffer_size = bitrate;
    ctx->rc_max_rate    = bitrate;
    ctx->rc_min_rate    = bitrate;
    ctx->thread_count = 16;
    ctx->thread_type = FF_THREAD_FRAME;

    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune",   "zerolatency", 0);
    av_opt_set_int(ctx->priv_data, "rc_lookahead", 0, 0);

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

    //OpenCV -->BGR input --Translation via SwsContext-->YUV420P -->AVFrame
    SwsContext* sws = sws_getContext(
        WIDTH, HEIGHT, AV_PIX_FMT_BGR24,
        WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );
    int frame_count = 0;
    int fps_counter = 0;
    auto t0 = chrono::high_resolution_clock::now();

    while(cap.read(frame)){
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

            BuiltSlices built = build_slices_with_fec(pkt->data, static_cast<size_t>(pkt->size), mtu, static_cast<uint32_t>(frame_count));

            cout << "Frame #" << frame_count
                 << " | Raw: " << raw_size << " bytes"
                 << " -> Encoded: " << encoded_size << " bytes"
                 << " | k=" << built.k
                 << " r=" << built.r
                 << " | total=" << (built.k + built.r)
                 << endl;

            if (!built.data_slices.empty()) {
                udp_sender.send_slices(built.data_slices);
            }
            if (!built.parity_slices.empty()) {
                udp_sender.enable_redundancy(1);
                udp_sender.send_slices(built.parity_slices);
                udp_sender.enable_redundancy(2);
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
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    cap.release();
    destroyAllWindows();

    // stop profiler thread
    run_profiler.store(false);
    if (profiler_thread.joinable()) profiler_thread.join();

    return 0;

}