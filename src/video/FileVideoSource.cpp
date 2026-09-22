#include "video/FileVideoSource.h"
#include <iostream>

FileVideoSource::~FileVideoSource() {
    close();
}

bool FileVideoSource::open(const std::string& source_path) {
    // 1. 打开输入流
    if (avformat_open_input(&fmt_ctx_, source_path.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "[FileVideoSource] 无法打开视频文件: " << source_path << std::endl;
        return false;
    }

    // 2. 获取流信息
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        std::cerr << "[FileVideoSource] 无法获取流信息" << std::endl;
        return false;
    }

    // 3. 查找视频流索引
    video_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        std::cerr << "[FileVideoSource] 未找到视频流" << std::endl;
        return false;
    }

    // 4. 查找并打开解码器
    AVCodecParameters* codec_par = fmt_ctx_->streams[video_stream_index_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (!codec) {
        std::cerr << "[FileVideoSource] 找不到合适的解码器" << std::endl;
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx_, codec_par);
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        std::cerr << "[FileVideoSource] 无法打开解码器" << std::endl;
        return false;
    }

    // 5. 初始化资源
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    std::cout << "[FileVideoSource] 视频源打开成功: " << source_path 
              << " (" << codec_ctx_->width << "x" << codec_ctx_->height << ")" << std::endl;
    return true;
}

bool FileVideoSource::read(cv::Mat& frame) {
    if (!fmt_ctx_ || !codec_ctx_) return false;

    while (av_read_frame(fmt_ctx_, packet_) >= 0) {
        // 只处理视频流的数据包
        if (packet_->stream_index == video_stream_index_) {
            // 发送数据包到解码器
            if (avcodec_send_packet(codec_ctx_, packet_) < 0) {
                av_packet_unref(packet_);
                continue;
            }

            // 接收解码后的帧
            while (avcodec_receive_frame(codec_ctx_, frame_) == 0) {
                // 初始化/更新图像缩放上下文 (YUV -> BGR)
                sws_ctx_ = sws_getCachedContext(
                    sws_ctx_,
                    frame_->width, frame_->height, (AVPixelFormat)frame_->format,
                    frame_->width, frame_->height, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr
                );

                // 分配或调整 cv::Mat 内存
                if (bgr_mat_.empty() || bgr_mat_.cols != frame_->width || bgr_mat_.rows != frame_->height) {
                    bgr_mat_ = cv::Mat(frame_->height, frame_->width, CV_8UC3);
                }

                // 转换图像格式并拷贝到 cv::Mat
                uint8_t* dest_data[4] = { bgr_mat_.data, nullptr, nullptr, nullptr };
                int dest_linesize[4] = { static_cast<int>(bgr_mat_.step[0]), 0, 0, 0 };
                sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height, dest_data, dest_linesize);

                // 深拷贝给外部帧，确保指针安全
                frame = bgr_mat_.clone(); 
                av_packet_unref(packet_);
                return true;
            }
        }
        av_packet_unref(packet_);
    }
    return false; // 视频读取结束
}

void FileVideoSource::close() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); }
    if (packet_) { av_packet_free(&packet_); }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_); }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); }
}