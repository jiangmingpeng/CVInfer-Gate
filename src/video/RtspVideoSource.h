//支持真实摄像头

#pragma once
#include "video/IVideoSource.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

class RtspVideoSource : public IVideoSource {
public:
    RtspVideoSource() = default;
    ~RtspVideoSource() override;

    bool open(const std::string& source_path) override;
    bool read(cv::Mat& frame) override;
    void close() override;

    int getWidth() const { return codec_ctx_ ? codec_ctx_->width : 0; }
    int getHeight() const { return codec_ctx_ ? codec_ctx_->height : 0; }
    // [T29] 源真实帧率(来自流元数据 avg_frame_rate); 拿不到返回 0
    double getFps() const override;

private:
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int video_stream_index_ = -1;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    cv::Mat bgr_mat_;
};