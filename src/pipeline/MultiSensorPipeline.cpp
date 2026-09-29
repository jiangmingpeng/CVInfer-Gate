#include "pipeline/MultiSensorPipeline.h"

#include <exception>
#include <utility>

#include "utils/Logger.h"

MultiSensorPipeline::~MultiSensorPipeline() {
    stop();
}

bool MultiSensorPipeline::init(const FusionConfig& fusion_cfg,
                               std::vector<std::shared_ptr<sensor::ISensorSource>> sensors,
                               sensor::ISensorSource* video_meta) {
    stop();   // 幂等: 重复 init 时先清理上一次的线程/缓冲

    if (sensors.empty()) {
        CVLOG_ERROR << "[MultiSensorPipeline] 未提供任何非视频传感器, 初始化失败。";
        return false;
    }

    cfg_ = fusion_cfg;
    fusion_ = std::make_unique<fusion::SensorFusion>(cfg_);
    video_meta_ = video_meta;
    sensors_ = std::move(sensors);

    running_.store(true);
    try {
        pollers_.reserve(sensors_.size());
        for (const auto& s : sensors_) {
            pollers_.emplace_back([this, s] { pollLoop(s); });
        }
    } catch (const std::exception& e) {
        CVLOG_ERROR << "[MultiSensorPipeline] 启动 poller 线程失败: " << e.what();
        stop();
        return false;
    }
    enabled_.store(true);

    CVLOG_INFO << "[MultiSensorPipeline] 初始化完成: sensors=" << sensors_.size()
               << ", tol=" << cfg_.time_tolerance_ms << "ms"
               << ", weight=" << cfg_.sensor_weight
               << ", match_iou=" << cfg_.match_iou
               << ", buffer=" << cfg_.buffer_capacity
               << (video_meta_ ? (", video=" + video_meta_->name()) : ", video=(none)");
    return true;
}

void MultiSensorPipeline::pollLoop(const std::shared_ptr<sensor::ISensorSource>& src) {
    sensor::SensorSample s;
    while (running_.load()) {
        if (src->read(s)) {
            pushSample(std::move(s));
            s = sensor::SensorSample{};   // 复位(防御: 避免下一轮残留 frame/targets)
        } else {
            // read() 返回 false: 文件回放结束(正常) 或 close() 打断(收尾)
            if (running_.load()) {
                ++poll_errors_;
                CVLOG_DEBUG << "[MultiSensorPipeline] 传感器结束/无数据: " << src->name();
            }
            break;
        }
    }
}

void MultiSensorPipeline::pushSample(sensor::SensorSample s) {
    std::lock_guard<std::mutex> lk(buf_mtx_);
    const std::size_t cap = cfg_.buffer_capacity > 0 ? cfg_.buffer_capacity : 256;
    samples_.push_back(std::move(s));
    ++samples_pushed_;
    // 有界 + 丢最旧: 与全项目一致的"实时性优先"策略, 永不阻塞 poller。
    while (samples_.size() > cap) {
        samples_.pop_front();
        ++samples_dropped_;
    }
}

std::vector<sensor::SensorSample> MultiSensorPipeline::snapshot() const {
    std::lock_guard<std::mutex> lk(buf_mtx_);
    return std::vector<sensor::SensorSample>(samples_.begin(), samples_.end());
}

void MultiSensorPipeline::fuse(std::vector<DetectionResult>& detections) {
    if (!enabled_.load() || !fusion_) return;

    // 对齐锚点: 优先用视频侧时间基。
    //   注意: 当前接线中 VideoPipeline 直接持有底层 IVideoSource, 不会调用
    //   VideoSensorSource::read(), 故 lastTimestampMs()==0 -> 实际锚点 = nowMs()
    //   ("融合时刻")。一旦视频帧时间戳接入 ISensorSource / Frame::timestamp_ms,
    //   此处会自动切换为真正的视频时间基, 本函数无需修改(见 VideoSensorSource.h)。
    std::int64_t anchor = video_meta_ ? video_meta_->lastTimestampMs() : 0;
    if (anchor <= 0) anchor = sensor::nowMs();

    // 快照 = 短临界区(仅拷贝, 不做融合); 融合在锁外进行, 不与 poller 争锁
    const std::vector<sensor::SensorSample> snap = snapshot();
    const fusion::FusionStats fs = fusion_->fuse(detections, snap, anchor);

    ++frames_;
    f_samples_seen_ += fs.samples_seen;
    f_aligned_ += fs.aligned;
    f_targets_ += fs.targets;
    f_matched_ += fs.matched;
    f_unmatched_sensor_ += fs.unmatched_sensor;
    f_emitted_ += fs.emitted_sensor_only;
}

void MultiSensorPipeline::stop() {
    enabled_.store(false);
    running_.store(false);

    // 先 close(): 唤醒可能正处限速睡眠中的 read(), 否则 join 会被拖住
    for (auto& s : sensors_) {
        if (s) s->close();
    }
    for (auto& t : pollers_) {
        if (t.joinable()) t.join();
    }
    pollers_.clear();
    sensors_.clear();

    {
        std::lock_guard<std::mutex> lk(buf_mtx_);
        samples_.clear();
    }
    fusion_.reset();
    video_meta_ = nullptr;
}

MultiSensorPipeline::Stats MultiSensorPipeline::stats() const {
    Stats s;
    s.frames = frames_.load();
    s.fusion.frames = s.frames;
    s.fusion.samples_seen = f_samples_seen_.load();
    s.fusion.aligned = f_aligned_.load();
    s.fusion.targets = f_targets_.load();
    s.fusion.matched = f_matched_.load();
    s.fusion.unmatched_sensor = f_unmatched_sensor_.load();
    s.fusion.emitted_sensor_only = f_emitted_.load();
    s.samples_pushed = samples_pushed_.load();
    s.samples_dropped = samples_dropped_.load();
    s.poll_errors = poll_errors_.load();
    {
        std::lock_guard<std::mutex> lk(buf_mtx_);
        s.buffer_size = samples_.size();
    }
    return s;
}
