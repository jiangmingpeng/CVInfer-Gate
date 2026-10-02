#include "pipeline/VideoPipeline.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>

#include "inference/IModel.h"
#include "video/IVideoSource.h"

VideoPipeline::VideoPipeline(IVideoSource& source,
                             IDetector& detector,
                             Config config,
                             LifecycleCoordinator& lifecycle,
                             ResultCallback on_result)
    : source_(source),
      detector_(detector),
      config_(config),
      lifecycle_(lifecycle),
      on_result_(std::move(on_result)),
      frame_queue_(config_.queue_max_size,
                   config_.drop_oldest
                       ? ThreadSafeQueue<std::shared_ptr<Frame>>::Policy::DropOldest
                       : ThreadSafeQueue<std::shared_ptr<Frame>>::Policy::Block),
      result_queue_(config_.queue_max_size,
                    ThreadSafeQueue<std::shared_ptr<Result>>::Policy::DropOldest) {}

VideoPipeline::~VideoPipeline() {
    stop();
}

bool VideoPipeline::start() {
    if (started_) return true;
    if (config_.worker_threads < 1) config_.worker_threads = 1;
    if (config_.frame_interval < 1) config_.frame_interval = 1;

    running_ = true;
    lifecycle_.registerChild();
    active_workers_ = config_.worker_threads;

    try {
        decode_thread_ = std::thread([this] { decodeLoop(); });
        workers_.reserve(static_cast<std::size_t>(config_.worker_threads));
        for (int i = 0; i < config_.worker_threads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
        sink_thread_ = std::thread([this] { sinkLoop(); });
    } catch (const std::exception&) {
        stop(); // 启动失败则回滚
        return false;
    }

    started_ = true;
    return true;
}

void VideoPipeline::stop() {
    running_ = false;

    frame_queue_.close();
    if (decode_thread_.joinable()) decode_thread_.join();

    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();

    result_queue_.close();
    if (sink_thread_.joinable()) sink_thread_.join();

    if (started_) {
        lifecycle_.unregisterChild();
        started_ = false;
    }
}

VideoPipeline::Stats VideoPipeline::stats() const {
    Stats s;
    s.decoded = decoded_.load();
    s.dropped = frame_queue_.stats().dropped;
    s.processed = processed_.load();
    s.emitted = emitted_.load();
    return s;
}

// 阶段一: 解码 + 抽帧 + 限速
void VideoPipeline::decodeLoop() {
    const int interval = std::max(1, config_.frame_interval);
    auto next_tick = std::chrono::steady_clock::now();
    std::uint64_t index = 0;
    cv::Mat frame;

    while (running_.load() && !lifecycle_.isShutdownRequested() && source_.read(frame)) {
        // 抽帧: 非采样帧直接跳过, 不入队 (避免无谓的 clone 开销)
        if ((index % static_cast<std::uint64_t>(interval)) != 0) {
            ++index;
            continue;
        }

        auto pf = std::make_shared<Frame>();
        pf->image = frame.clone(); // 源缓冲可能复用, 必须深拷贝
        pf->seq = index++;
        frame_queue_.push(std::move(pf));
        ++decoded_;

        // 限速: 让解码节奏匹配目标帧率, 从源头抑制队列膨胀
        if (config_.target_fps > 0) {
            next_tick += std::chrono::milliseconds(1000 / config_.target_fps);
            std::this_thread::sleep_until(next_tick);
        }
    }

    frame_queue_.close(); // 解码结束 -> 通知 worker 收尾
}

// 阶段二: 推理 worker (统一走 IDetector 抽象, 借引擎/推理/后处理在模型内部完成)
void VideoPipeline::workerLoop() {
    std::shared_ptr<Frame> frame;
    while (frame_queue_.pop(frame)) {
        std::vector<DetectionResult> detections;
        // Busy/Failed 均跳过该帧, 与旧版"借不到引擎/推理失败即 skip"等价
        if (detector_.detect(frame->image, detections) != DetectStatus::Ok) {
            continue;
        }
        ++processed_;

        auto result = std::make_shared<Result>();
        result->frame = frame;
        result->detections = std::move(detections);
        result_queue_.push(std::move(result));
    }

    // 最后一个退出的 worker 关闭结果队列, 驱动 sink 收尾
    if (--active_workers_ == 0) {
        result_queue_.close();
    }
}

// 阶段三: sink (画框 / 写视频 / 落库)
// 阶段三: sink (按帧序保序 -> 画框 / 写视频 / 落库)
// 多 worker 并发推理, 结果完成顺序与帧序不一致; 直接写视频会出现
// “画面回跳/抖动”。这里用一个小重排缓冲: 期望 next_seq, 乱序结果先暂存,
// 能连续吐出就吐出; 若暂存数超过 kMaxReorder(说明有帧已被 drop_oldest 丢掉,
// 对应的 seq 永远等不到), 则放弃等待、按 seq 从小到大吐出, 保证不无限阻塞。
// 单 worker / 无丢帧时退化为“来一帧吐一帧”, 零额外延迟。
void VideoPipeline::sinkLoop() {
    constexpr std::size_t kMaxReorder = 8;
    std::map<std::uint64_t, std::shared_ptr<Result>> pending;
    std::uint64_t next_seq = 0;

    auto emit = [this](const std::shared_ptr<Result>& r) {
        if (on_result_ && r && r->frame) {
            // 透传 frame_seq, 供异步复核按帧回收结果
            on_result_(r->frame->seq, r->frame->image, r->detections);
            ++emitted_;
        }
    };

    std::shared_ptr<Result> result;
    while (result_queue_.pop(result)) {
        if (!result || !result->frame) continue;
        if (pending.empty()) next_seq = result->frame->seq; // 首帧/跳变后重新对齐
        pending[result->frame->seq] = std::move(result);
        while (!pending.empty() &&
               (pending.begin()->first == next_seq || pending.size() > kMaxReorder)) {
            auto it = pending.begin();
            emit(it->second);
            next_seq = it->first + 1;
            pending.erase(it);
        }
    }

    for (auto& kv : pending) emit(kv.second); // 收尾: 按 seq 从小到大输出剩余
}
