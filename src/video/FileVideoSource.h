#pragma once
#include "video/IVideoSource.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

class FileVideoSource : public IVideoSource {
public:
    FileVideoSource() = default;
    ~FileVideoSource() override;

    bool open(const std::string& source_path) override;
    bool read(cv::Mat& frame) override;
    void close() override;
    int getWidth() const { return codec_ctx_ ? codec_ctx_->width : 0; }
    int getHeight() const { return codec_ctx_ ? codec_ctx_->height : 0; }

private:
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    
    int video_stream_index_ = -1;
    AVFrame* frame_ = nullptr;      // 存放解码后的 YUV 帧
    AVPacket* packet_ = nullptr;    // 存放压缩数据包
    cv::Mat bgr_mat_;               // 存放转换后的 BGR 帧
};