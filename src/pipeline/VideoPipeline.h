#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "inference/DetectionResult.h"
#include "utils/LifecycleCoordinator.h"
#include "utils/ThreadSafeQueue.h"

class IVideoSource;
class IDetector;

// ============================================================
// VideoPipeline (T5: 三阶段流水线)
// ------------------------------------------------------------
//   解码线程(抽帧/限速) --frame_queue--> N 个推理 worker --result_queue--> sink 线程
//
// 与原版 main.cpp 的单消费者线程相比:
//   1) 解码与推理分离, 推理可多 worker 并行 (配合 InferenceEnginePool)
//   2) 按 video.target_fps / video.frame_interval 抽帧限速, 从源头抑制队列膨胀
//   3) 队列容量/策略来自 video.queue.*, 不再硬编码
//   4) 通过 LifecycleCoordinator 感知关闭信号, 支持优雅退出
//   5) 提供 stats() (decoded/dropped/processed/emitted) 便于观测
//   6) [T13/T15] worker 不再直接持 engines/后处理, 改依赖 IDetector 抽象:
//      Phase A 注入 YoloDetector; T16+ 可注入 CascadeEngine(同样实现 IDetector),
//      流水线无需改动。
//
// 注意: 多 worker 下 result 完成顺序不保证与帧序一致;
//       如需严格有序, sink 可依据 Result.frame->seq 重排序。
// ============================================================
class VideoPipeline {
public:
    struct Config {
        int target_fps = 0;                    // 0 = 不限速
        int frame_interval = 1;                // 每 N 帧取 1 帧
        std::size_t queue_max_size = 24;       // 有界队列容量
        bool drop_oldest = true;               // true=DropOldest, false=Block
        int worker_threads = 2;                // 推理 worker 数
        // 注: [T15] 借引擎超时已下沉到具体模型(YoloDetector/ModelConfig),
        //     流水线不再持有该参数。
    };

    struct Frame {
        cv::Mat image;
        std::uint64_t seq = 0;
    };

    struct Result {
        std::shared_ptr<Frame> frame;
        std::vector<DetectionResult> detections;
    };

    // sink 回调: 画框 / 写视频 / 落库等都在此完成
    // [T22] frame_seq 供异步复核按帧回收结果(见 ReviewScheduler)。
    using ResultCallback = std::function<void(std::uint64_t frame_seq,
                                              const cv::Mat& frame,
                                              const std::vector<DetectionResult>& detections)>;

    VideoPipeline(IVideoSource& source,
                  IDetector& detector,
                  Config config,
                  LifecycleCoordinator& lifecycle,
                  ResultCallback on_result);
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    bool start();
    void stop();   // 关闭队列 + join 全部线程 (幂等)

    struct Stats {
        std::uint64_t decoded = 0;     // 成功入队的帧数
        std::uint64_t dropped = 0;     // 因队列满被丢弃的帧数
        std::uint64_t processed = 0;   // 完成推理的帧数
        std::uint64_t emitted = 0;     // 已交付 sink 的帧数
    };
    Stats stats() const;

private:
    void decodeLoop();
    void workerLoop();
    void sinkLoop();

    IVideoSource& source_;
    IDetector& detector_;     // [T13/T15] 推理抽象(单模型=YoloDetector; T16+可=级联)
    Config config_;
    LifecycleCoordinator& lifecycle_;
    ResultCallback on_result_;

    ThreadSafeQueue<std::shared_ptr<Frame>> frame_queue_;
    ThreadSafeQueue<std::shared_ptr<Result>> result_queue_;

    std::thread decode_thread_;
    std::vector<std::thread> workers_;
    std::thread sink_thread_;

    std::atomic<int> active_workers_{0};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> decoded_{0};
    std::atomic<std::uint64_t> processed_{0};
    std::atomic<std::uint64_t> emitted_{0};
    bool started_ = false;
};
