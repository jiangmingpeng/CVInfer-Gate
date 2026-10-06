// 支持真实摄像头

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "video/IVideoSource.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

// RtspVideoSource (补「可中断关闭 + 断流重连」)
// 修两个冻结缺陷:
// R-13 `close()` 打不断阻塞中的 `av_read_frame`
// 原实现: read() 阻塞在 av_read_frame 里, close() 只 free 资源 ——
// 于是“停止采集”要等 FFmpeg 自己超时(至少 stimeout=5s, 网络
// 半死时更久); 而且 free 掉 fmt_ctx_ 会让 read() 踩野指针。
// 现修法: 给 AVFormatContext 挂 interrupt_callback, 回调只读一个
// atomic<bool> 就返回 1; FFmpeg 会在网络阻塞点周期性调用它,
// 故 close() 置位后 av_read_frame 很快返回 AVERROR_EXIT。
// 另用 api_mtx_ 串行化 read()/close(), 杜绝“边读边 free”。
// R-5  断流不重连
// 原实现: av_read_frame < 0 就 return false, 整条流水线结束。
// 现修法: 在 read() 内部做**指数退避重连**(0.5s -> 8s 封顶), 对上层
// 完全透明(上层只会看到“读到帧”/“被关闭”)。
//
// ⚠️ 已知边界: 重连后若**分辨率变了**, sink 端已建好的 VideoWriter 不会跟着变
// (不是本类的职责)。换流请重启进程, 或让 sink 自己比较 frame.size() 后重建。
class RtspVideoSource : public IVideoSource {
public:
    RtspVideoSource() = default;
    ~RtspVideoSource() override;

    bool open(const std::string& source_path) override;
    bool read(cv::Mat& frame) override;
    void close() override;

    int getWidth() const override { return width_.load(); }
    int getHeight() const override { return height_.load(); }
    // 源真实帧率(来自流元数据 avg_frame_rate); 拿不到返回 0
    // 改为**缓存值**: 重连期间不会瞬时变 0, 上层容器帧率不会抖
    double getFps() const override { return last_fps_.load(); }

    // 观测: 累计成功重连次数
    std::uint64_t reconnectCount() const { return reconnects_.load(); }

private:
    bool openInternal(bool verbose); // 真正的打开(可重复调用; 不持锁)
    void closeInternal(); // 只释放 FFmpeg 资源(不持锁)
    bool tryReconnect(); // 指数退避重连(read() 内部调用, 已持锁)
    double readStreamFps() const; // 从流元数据取帧率
    static int interruptCb(void* opaque);

    std::string url_;

    // 重连参数
    int reconnect_base_ms_ = 500; // 退避起点
    int reconnect_max_ms_ = 8000; // 退避上限

    // 线程安全 / 生命周期
    std::atomic<bool> stop_{false}; // 用户要求关闭(不再继续)
    std::atomic<bool> abort_read_{false}; // 给 FFmpeg interrupt callback 看
    // 是否“成功连上过”: 启动时 open() 就失败的话, read() 不应陷入
    // 无限重连(否则旧 main 那种“读循环跑完才起 gRPC”的结构会直接卡死)。
    std::atomic<bool> connected_once_{false};
    std::atomic<std::uint64_t> reconnects_{0};
    mutable std::mutex api_mtx_; // 串行化 open/read/close

    // 对外可见的稳定元数据(重连期间不清零)
    std::atomic<int> width_{0};
    std::atomic<int> height_{0};
    std::atomic<double> last_fps_{0.0};

    // FFmpeg 资源
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int video_stream_index_ = -1;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    cv::Mat bgr_mat_;
};