extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <isa-l/erasure_code.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstddef>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>


#define Device_ID 0

#define WIDTH 1280
#define HEIGHT 720
#define FPS 30


const int k = 20;
const int r = 5;

static size_t mtu = 1200;

using namespace cv;
using namespace std;

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



int main(){
    Mat frame;
    VideoCapture cap;
    int bitrate = 400000;//->Siktiğimin şeyini Adaptive Yapıcam sonra
    int64_t counter = 0;

    cap.open(Device_ID, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        cout << "Error opening video device" << endl;
        return -1;
    }
    // Önce dört‐character code’u MJPG olarak ayarlayın
    cap.set(CAP_PROP_FOURCC, VideoWriter::fourcc('M','J','P','G'));
    cap.set(CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(CAP_PROP_FRAME_HEIGHT, 720);
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
    ctx->max_b_frames = 1;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->rc_buffer_size = bitrate;
    ctx->rc_max_rate    = bitrate;
    ctx->rc_min_rate    = bitrate;
    ctx->thread_count = 16;
    ctx->thread_type = FF_THREAD_FRAME;

    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune",   "film", 0);

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

    next_frame:
    while(cap.read(frame)){
        fps_counter++;
        frame_count++;

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

            auto chunks = slice_and_pad(pkt->data, pkt->size, mtu);
            cout << "Frame #" << frame_count
                 << " | Raw: " << raw_size << " bytes"
                 << " -> Encoded: " << encoded_size << " bytes"
                 << "Chunk count: " << chunks.size()
                 << " | Redundant: " << r
                 << " | Total sendable: " << chunks.size() + r << endl;

            //-->Chunks ı UDP Channelları ile gönder

            int effective_k = chunks.size();
            if (effective_k < 2) {
                cerr << "Too small to apply FEC: skipping." << endl;
                goto send_anyway;
            }

            // r: parity count = %25 of k, minimum 1, maximum 10
            int effective_r = std::clamp(effective_k / 4, 1, 10);


            size_t chunk_size = chunks[0].size();
            for (int i = 0; i < effective_k; ++i) {
                if (chunks[i].size() != chunk_size) {
                    cerr << "Inconsistent chunk size." << endl;
                    av_packet_unref(pkt);
                    goto next_frame;
                }
            }

            // Parity için yer ayır
            std::vector<std::vector<uint8_t>> parity_chunks(effective_r, std::vector<uint8_t>(chunk_size));
            std::vector<uint8_t*> data_ptrs(effective_k), parity_ptrs(effective_r);
            for (int i = 0; i < effective_k; ++i) data_ptrs[i] = chunks[i].data();
            for (int i = 0; i < effective_r; ++i) parity_ptrs[i] = parity_chunks[i].data();

            // Her frame için RS matris yeniden oluştur
            std::vector<uint8_t> matrix((effective_k + effective_r) * effective_k);
            std::vector<uint8_t> g_tbls(32 * 1024);

            gf_gen_rs_matrix(matrix.data(), effective_k + effective_r, effective_k);
            ec_init_tables(effective_k, effective_r, matrix.data() + effective_k * effective_k, g_tbls.data());
            ec_encode_data(chunk_size, effective_k, effective_r, g_tbls.data(), data_ptrs.data(), parity_ptrs.data());

            send_anyway:
            //Burada gönderme işlemi yapıcam
            //yani UDP_SEND(&chunk) gibi yapıcam sonuçta chunk a +r bit eklenmişse de eklenmemişse de sorun olmayavak hepsi gönderilecek

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

    return 0;

}