#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fusion/SensorFusion.h"
#include "sensor/ISensorSource.h"
#include "utils/ConfigParser.h" // FusionConfig

// MultiSensorPipeline (多模态融合编排)
// 定位: **不改动 VideoPipeline**。它作为"融合前置阶段"挂在既有 sink 回调的
// 最前面, 因此解码/抽帧/限速/多 worker/队列背压/落库全部复用 —— 与
// Phase A~C 的抽象思路一致(级联实现 IDetector、复核实现 IReviewService,
// 这里则是"在既有 sink 上叠加一个融合阶段")。
//
// 线程模型:
// * 每个非视频传感器一个 poller 线程: read() -> 入**有界**时间缓冲(满则丢最旧);
// * fuse() 由 sink 线程调用: 取缓冲快照 -> 时间对齐 -> 决策级融合(纯内存, 不阻塞);
// * 对齐锚点 = VideoSensorSource::lastTimestampMs()(视频时间基), 不可用时用 nowMs()。
//
// 与其它阶段的协作(重要):
// * 融合在 sink **最前置**执行, 故画框/落库/告警/复核看到的都是**融合后**的
// confidence; 原始视觉置信度保存在 DetectionResult::vision_confidence;
// * 融合**不改变框**、不新增类别(除非 emit_sensor_only 且传感器自带框);
// * 逐帧顺序问题(R-1)不受影响: 融合是"逐帧就地"的, 不做跨帧状态。
class MultiSensorPipeline {
public:
    struct Stats {
        std::uint64_t frames = 0; // 参与融合的帧数
        fusion::FusionStats fusion; // 融合计数(累计)
        std::uint64_t samples_pushed = 0; // 入缓冲的采样数
        std::uint64_t samples_dropped = 0; // 缓冲满被丢弃(最旧)
        std::uint64_t poll_errors = 0; // read() 返回 false 的次数(结束/出错)
        std::size_t buffer_size = 0; // 当前缓冲占用(收尾/观测用)
    };

    MultiSensorPipeline() = default;
    ~MultiSensorPipeline();

    MultiSensorPipeline(const MultiSensorPipeline&) = delete;
    MultiSensorPipeline& operator=(const MultiSensorPipeline&) = delete;

    // sensors: 已 open() 的非视频传感器(见 sensor::createSensorSource); 不可为空
    // video_meta: 视频时间基(**非拥有**, 可空 -> 退化为 nowMs())
    bool init(const FusionConfig& fusion_cfg,
              std::vector<std::shared_ptr<sensor::ISensorSource>> sensors,
              sensor::ISensorSource* video_meta);

    // sink 阶段调用: 就地融合(不阻塞)
    void fuse(std::vector<DetectionResult>& detections);

    void stop(); // 关闭传感器 + join poller(幂等, 可在 init 前/析构中调用)
    Stats stats() const;
    bool enabled() const { return enabled_.load(); }

private:
    void pollLoop(const std::shared_ptr<sensor::ISensorSource>& src);
    void pushSample(sensor::SensorSample s);
    std::vector<sensor::SensorSample> snapshot() const;

    FusionConfig cfg_;
    std::unique_ptr<fusion::SensorFusion> fusion_;
    sensor::ISensorSource* video_meta_ = nullptr;
    std::vector<std::shared_ptr<sensor::ISensorSource>> sensors_;
    std::vector<std::thread> pollers_;

    mutable std::mutex buf_mtx_;
    std::deque<sensor::SensorSample> samples_; // 时间缓冲(有界, 丢最旧)

    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};

    // 统计(原子累加; 由 sink 线程/poller 线程分别更新)
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> samples_pushed_{0};
    std::atomic<std::uint64_t> samples_dropped_{0};
    std::atomic<std::uint64_t> poll_errors_{0};
    std::atomic<std::uint64_t> f_samples_seen_{0};
    std::atomic<std::uint64_t> f_aligned_{0};
    std::atomic<std::uint64_t> f_targets_{0};
    std::atomic<std::uint64_t> f_matched_{0};
    std::atomic<std::uint64_t> f_unmatched_sensor_{0};
    std::atomic<std::uint64_t> f_emitted_{0};
};
