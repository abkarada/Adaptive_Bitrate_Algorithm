extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}
#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstddef>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>

using namespace cv;
using namespace std;

#define Device_ID 0

#define WIDTH 1280
#define HEIGHT 720
#define FPS 30

static size_t mtu = 1200; // Şimdilik sabit bunu da bitrate e göre hesaplıcaz

std::vector<std::vector<uint8_t>> slice(const uint8_t* data, size_t size, size_t mtu) {
    std::vector<std::vector<uint8_t>> chunks;
    size_t offset = 0;
    while (offset < size) {
        size_t len = std::min(mtu, size - offset);
        chunks.emplace_back(data + offset, data + offset + len);
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

            auto chunks = slice(pkt->data, pkt->size, mtu);
            cout << "Frame #" << frame_count
                 << " | Raw: " << raw_size << " bytes"
                 << " -> Encoded: " << encoded_size << " bytes"
                 <<"->Chunk Count: "<<chunks.size()<<endl;

            //-->

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