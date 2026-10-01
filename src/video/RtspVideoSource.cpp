#include "video/RtspVideoSource.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include <libavutil/error.h>

namespace {

// 把 FFmpeg 的负错误码翻成人话
std::string avErr(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(rc, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

RtspVideoSource::~RtspVideoSource() { close(); }

// [T36] FFmpeg 的中断回调: 可能在**任意线程/任意时刻**被调用,
//   因此只做“读一个 atomic 就返回”这一件事(不加锁、不分配、不打印)。
int RtspVideoSource::interruptCb(void* opaque) {
    auto* self = static_cast<RtspVideoSource*>(opaque);
    return self->abort_read_.load(std::memory_order_relaxed) ? 1 : 0;
}

bool RtspVideoSource::open(const std::string& source_path) {
    // [T36] open/read/close 共用一把锁: 保证不会“一边读一边被 free”。
    std::lock_guard<std::mutex> lk(api_mtx_);
    url_ = source_path;
    stop_ = false;          // 允许重新 open
    abort_read_ = false;
    const bool ok = openInternal(/*verbose=*/true);
    if (ok) connected_once_ = true;   // 只有成功过, 后续断流才值得重连
    return ok;
}

bool RtspVideoSource::openInternal(bool verbose) {
    closeInternal();   // 幂等: 保证从干净状态开始(重连时用)

    // 关键：设置 RTSP 传输参数，防止卡死
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0); // 强制 TCP，避免 UDP 丢包
    // [T36] 超时: stimeout 是 RTSP 专用(旧名), rw_timeout 是通用读写超时;
    //   两个都设以兼容不同 FFmpeg 版本。单位都是微秒。
    av_dict_set(&opts, "stimeout", "5000000", 0);   // 5秒超时（微秒）
    av_dict_set(&opts, "rw_timeout", "5000000", 0); // 5秒(通用读写超时)
    av_dict_set(&opts, "max_delay", "500000", 0);   // 最大延迟 500ms

    // [T36] 必须自己 avformat_alloc_context() 才能挂 interrupt_callback ——
    //   avformat_open_input 内部自己分配 context, 我们插不进回调, 于是“打开阶段
    //   卡住”也没法中断(close() 只能干等 rw_timeout 到点)。
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        std::cerr << "[RtspVideoSource] avformat_alloc_context 失败" << std::endl;
        av_dict_free(&opts);
        return false;
    }
    fmt_ctx_->interrupt_callback.callback = &RtspVideoSource::interruptCb;
    fmt_ctx_->interrupt_callback.opaque = this;

    // [T31] 保留错误码: 原代码用 `!= 0` 丢掉了 rc, 只留下 FFmpeg 自己那行
    //   "[rtsp @ ...] method DESCRIBE failed: 404 Not Found", 看不懂到底出了什么事。
    const int rc = avformat_open_input(&fmt_ctx_, url_.c_str(), nullptr, &opts);
    if (rc < 0) {
        std::cerr << "[RtspVideoSource] 打开失败: " << url_
                  << "  (" << avErr(rc) << ")" << std::endl;
        if (verbose) {
            std::cerr << "  自查(按顺序):\n"
                  << "    1) 推流端是否在线? mediamtx 对“没有发布者”的路径一律返回 404 Not Found:\n"
                  << "       看 mediamtx 日志是否出现该 path 的 publishing, 或 ss -tnp | grep 8554 看有无已建立连接\n"
                  << "    2) 流是否真的可拉? ffprobe -rtsp_transport tcp -i \"" << url_ << "\"\n"
                  << "    3) mediamtx 是否只绑定了回环? ss -tlnp | grep 8554 (127.0.0.1:8554 -> 公网推流进不来; 应为 0.0.0.0:*:8554)\n"
                  << "    4) 路径名/端口是否与推流端一致(如推的是 /live 而拉的是 /live/stream)"
                  << std::endl;
        }   // if (verbose) —— 重连时不重复刷这 5 行自查
        av_dict_free(&opts);
        closeInternal();   // avformat_open_input 失败时已把 fmt_ctx_ 置空, 这里清理其余
        return false;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        std::cerr << "[RtspVideoSource] 无法获取流信息" << std::endl;
        closeInternal();
        return false;
    }

    video_stream_index_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index_ < 0) {
        std::cerr << "[RtspVideoSource] 找不到视频流" << std::endl;
        closeInternal();
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(fmt_ctx_->streams[video_stream_index_]->codecpar->codec_id);
    if (!codec) {
        std::cerr << "[RtspVideoSource] 找不到可用解码器" << std::endl;
        closeInternal();
        return false;
    }
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        std::cerr << "[RtspVideoSource] avcodec_alloc_context3 失败" << std::endl;
        closeInternal();
        return false;
    }
    avcodec_parameters_to_context(codec_ctx_, fmt_ctx_->streams[video_stream_index_]->codecpar);
    
    // 开启多线程解码
    codec_ctx_->thread_count = 4; 
    
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        std::cerr << "[RtspVideoSource] avcodec_open2 失败" << std::endl;
        closeInternal();
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
        std::cerr << "[RtspVideoSource] 分配 frame/packet 失败" << std::endl;
        closeInternal();
        return false;
    }

    // [T36] 缓存对外元数据: 重连期间 getWidth()/getFps() 不会瞬时变 0
    width_ = codec_ctx_->width;
    height_ = codec_ctx_->height;
    last_fps_ = readStreamFps();
    bgr_mat_.release();

    std::cout << "[RtspVideoSource] RTSP 流打开成功: " << url_
              << " (" << width_.load() << "x" << height_.load() << ")"
              << (last_fps_.load() > 0.0 ? "" : " [帧率未知]") << std::endl;
    return true;
}

bool RtspVideoSource::read(cv::Mat& frame) {
    // [T36] 持锁: 与 close() 互斥, 保证不会“一边 av_read_frame 一边被 free”。
    std::lock_guard<std::mutex> lk(api_mtx_);

    while (!stop_.load()) {
        // ---- 没连上(或刚断): 先重连 ----
        if (!fmt_ctx_ || !codec_ctx_) {
            // 启动时就没连上(open() 失败过): 不要把上层卡在这里无限重连 ——
            // 如实返回“没流”, 让调用方去起 gRPC / 报错(与 T36 之前行为一致)。
            if (!connected_once_.load()) return false;
            if (!tryReconnect()) return false;   // 被要求关闭
            continue;
        }

        const int rc = av_read_frame(fmt_ctx_, packet_);
        if (rc < 0) {
            av_packet_unref(packet_);
            // 主动关闭: interrupt_callback 会让阻塞中的读返回 AVERROR_EXIT
            if (stop_.load() || abort_read_.load() || rc == AVERROR_EXIT) return false;
            if (rc == AVERROR(EAGAIN)) {         // 暂时没数据, 不是断流
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // 真断流: 清理 -> 重连(read() 对外仍表现为“继续读到帧”)
            std::cout << "[RtspVideoSource] 读流中断(" << avErr(rc) << "), 开始重连..."
                      << std::endl;
            closeInternal();
            if (!tryReconnect()) return false;
            continue;
        }

        if (packet_->stream_index != video_stream_index_) {
            av_packet_unref(packet_);
            continue;
        }

        if (avcodec_send_packet(codec_ctx_, packet_) < 0) {
            av_packet_unref(packet_);
            continue;
        }
        av_packet_unref(packet_);   // send 已拷贝/引用数据, 这里可安全释放

        while (avcodec_receive_frame(codec_ctx_, frame_) == 0) {
            sws_ctx_ = sws_getCachedContext(sws_ctx_,
                frame_->width, frame_->height, (AVPixelFormat)frame_->format,
                frame_->width, frame_->height, AV_PIX_FMT_BGR24,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws_ctx_) {
                std::cerr << "[RtspVideoSource] sws_getCachedContext 失败" << std::endl;
                continue;
            }

            if (bgr_mat_.empty() || bgr_mat_.cols != frame_->width || bgr_mat_.rows != frame_->height) {
                bgr_mat_ = cv::Mat(frame_->height, frame_->width, CV_8UC3);
            }

            uint8_t* dest_data[4] = { bgr_mat_.data, nullptr, nullptr, nullptr };
            int dest_linesize[4] = { static_cast<int>(bgr_mat_.step[0]), 0, 0, 0 };
            sws_scale(sws_ctx_, frame_->data, frame_->linesize, 0, frame_->height, dest_data, dest_linesize);

            frame = bgr_mat_.clone();
            return true;
        }
    }
    return false;   // 被 close() 要求停止
}

// [T29] 源真实帧率: 从流元数据 avg_frame_rate 取(拿不到退回 r_frame_rate;
//   都不行返回 0 -> 调用方回退默认值)。RTSP 的 avg_frame_rate 常为 0/0,
//   此时 r_frame_rate 一般是 25/1 或 30/1 这类可用值。
double RtspVideoSource::readStreamFps() const {
    if (!fmt_ctx_ || video_stream_index_ < 0) return 0.0;
    const AVStream* st = fmt_ctx_->streams[video_stream_index_];
    if (!st) return 0.0;
    const AVRational r = (st->avg_frame_rate.num > 0) ? st->avg_frame_rate : st->r_frame_rate;
    if (r.num <= 0 || r.den <= 0) return 0.0;
    const double fps = av_q2d(r);
    return (fps > 0.0 && fps <= 240.0) ? fps : 0.0;
}

// [T36] 指数退避重连。调用者必须已持 api_mtx_ (从 read() 里调用)。
//   返回 true = 已重新连上; false = 被要求关闭。
bool RtspVideoSource::tryReconnect() {
    if (stop_.load()) return false;

    int delay_ms = reconnect_base_ms_;
    for (int attempt = 1; !stop_.load(); ++attempt) {
        std::cout << "[RtspVideoSource] 重连尝试 #" << attempt << ": " << url_ << std::endl;

        closeInternal();
        if (openInternal(/*verbose=*/false)) {
            reconnects_.fetch_add(1);
            std::cout << "[RtspVideoSource] 重连成功(累计 " << reconnects_.load()
                      << " 次), " << width_.load() << "x" << height_.load() << std::endl;
            return true;
        }
        if (stop_.load()) return false;

        // 退避: **分片睡眠** —— 否则 close() 要等满整个退避时间才能拿到锁
        std::cout << "[RtspVideoSource] 重连失败, " << delay_ms << "ms 后重试" << std::endl;
        for (int slept = 0; slept < delay_ms && !stop_.load();) {
            const int slice = std::min(100, delay_ms - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            slept += slice;
        }
        delay_ms = std::min(delay_ms * 2, reconnect_max_ms_);
    }
    return false;
}

// [T36] 只释放 FFmpeg 资源: 不动 stop_/abort_read_, 也不清对外元数据缓存
//   (重连期间 getWidth()/getFps() 要保持稳定)。
void RtspVideoSource::closeInternal() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); }
    if (packet_) { av_packet_free(&packet_); }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_); }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); }
    video_stream_index_ = -1;
    bgr_mat_.release();
}

void RtspVideoSource::close() {
    // [T36] 顺序很重要:
    //   1) 先置位 —— interrupt_callback 会在 FFmpeg 的下一个阻塞点返回 1,
    //      让卡在 av_read_frame / avformat_open_input 里的那次调用尽快退出;
    //   2) 再抢锁 —— 等 read() 真正退出临界区, 此时 free 才是安全的。
    stop_ = true;
    abort_read_ = true;
    std::lock_guard<std::mutex> lk(api_mtx_);
    closeInternal();
    width_ = 0;
    height_ = 0;
    last_fps_ = 0.0;
}