extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/highgui.hpp>
#include <iostream>
#include <libavutil/opt.h>

using namespace cv;
using namespace std;

#define Device_ID 0

#define WIDTH 640
#define HEIGHT 480
#define FPS 30

int main(){
    Mat frame;
    VideoCapture cap;
    int bitrate = 400000;//->Siktiğimin şeyini Adaptive Yapıcam sonra
    cap.open(Device_ID, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        cout << "Error opening video device" << endl;
        return -1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, WIDTH);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, HEIGHT);
    cap.set(cv::CAP_PROP_FPS, FPS);

    avcodec_register_all();

    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
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
    ctx->thread_count = 16;
    ctx->thread_type = FF_THREAD_FRAME;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune",   "film", 0);



    while(cap.read(frame)){
        imshow("frame", frame);
        if(waitKey(5) >= 0 ){
            break;
        }
    }

    return 0;

}