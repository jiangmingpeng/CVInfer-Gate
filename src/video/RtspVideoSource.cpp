#include "video/RtspVideoSource.h"
#include <iostream>

RtspVideoSource::~RtspVideoSource() { close(); }

bool RtspVideoSource::open(const std::string& source_path) {
    // 关键：设置 RTSP 传输参数，防止卡死
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0); // 强制 TCP，避免 UDP 丢包
    av_dict_set(&opts, "stimeout", "5000000", 0);   // 5秒超时（微秒）
    av_dict_set(&opts, "max_delay", "500000", 0);   // 最大延迟 500ms

    if (avformat_open_input(&fmt_ctx_, source_path.c_str(), nullptr, &opts) != 0) {
        std::cerr << "[RtspVideoSource] 无法打开 RTSP 流: " << source_path << std::endl;
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        std::cerr << "[RtspVideoSource] 无法获取流信息" << std::endl;
        return false;
    }

    video_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) return false;

    const AVCodec* codec = avcodec_find_decoder(fmt_ctx_->streams[video_stream_index_]->codecpar->codec_id);
    codec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx_, fmt_ctx_->streams[video_stream_index_]->codecpar);
    
    // 开启多线程解码
    codec_ctx_->thread_count = 4; 
    
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) return false;

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    std::cout << "[RtspVideoSource] RTSP 流打开成功: " << source_path 
              << " (" << codec_ctx_->width << "x" << codec_ctx_->height << ")" << std::endl;
    return true;
}

bool RtspVideoSource::read(cv::Mat& frame) {
    if (!fmt_ctx_ || !codec_ctx_) return false;

    while (av_read_frame(fmt_ctx_, packet_) >= 0) {
        if (packet_->stream_index == video_stream_index_) {
            if (avcodec_send_packet(codec_ctx_, packet_) < 0) {
                av_packet_unref(packet_);
                continue;
            }
            while (avcodec_receive_frame(codec_ctx_, frame_) == 0) {
                sws_ctx_ = sws_getCachedContext(sws_ctx_,
                    frame_->width, frame_->height, (AVPixelFormat)frame_->format,
                    frame_->width, frame_->height, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);

                if (bgr_mat_.empty() || bgr_mat_.cols != frame_->width || bgr_mat_.rows != frame_->height) {
                    bgr_mat_ = cv::Mat(frame_->height, frame_->width, CV_8UC3);
                }

                uint8_t* dest_data[4] = { bgr_mat_.data, nullptr, nullptr, nullptr };
                int dest_linesize[4] = { static_cast<int>(bgr_mat_.step[0]), 0, 0, 0 };
                sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height, dest_data, dest_linesize);

                frame = bgr_mat_.clone();
                av_packet_unref(packet_);
                return true;
            }
        }
        av_packet_unref(packet_);
    }
    return false; // 流断开
}

void RtspVideoSource::close() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); }
    if (packet_) { av_packet_free(&packet_); }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_); }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); }
}