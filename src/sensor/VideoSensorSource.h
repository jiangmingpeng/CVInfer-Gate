#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "sensor/ISensorSource.h"
#include "video/IVideoSource.h"

namespace sensor {

// VideoSensorSource (把旧 IVideoSource 适配为统一传感器)
// 角色: 让"视频"以和其它模态一致的方式提供**统一时间戳**, 供
// MultiSensorPipeline 取对齐锚点以及日志/统计使用。
//
// 设计取舍(重要, 便于后续演进):
// * **非拥有**: 底层 IVideoSource 的打开/关闭由 main 管理, 本类只借用,
// 因此它既能作 ISensorSource(视频视图), 又完全不改 IVideoSource 契约;
// * 当前**不改动 VideoPipeline**: 帧的"捕获时刻"尚未逐帧透传到 sink。
// 更确切地说: VideoPipeline 直接持有底层 IVideoSource, **不会**调用本类的
// read(), 所以 lastTimestampMs() 在现有接线中恒为 0, MultiSensorPipeline
// 的对齐锚点实际回退为 nowMs()("融合时刻")。误差量级 ≈ 推理耗时(数十 ms),
// 对 time_tolerance_ms(默认 50ms) 可接受。
// 将来若把视频帧也接入 ISensorSource(或给 VideoPipeline::Frame 加 timestamp
// 并透传), 本类与 MultiSensorPipeline 都**无需改动**即可启用真实视频时间基
// (见笔记 Phase D 遗留)。
class VideoSensorSource final : public ISensorSource {
public:
    explicit VideoSensorSource(IVideoSource& video, std::string name = "video")
        : video_(video), name_(std::move(name)) {}

    const std::string& name() const override { return name_; }
    SensorKind kind() const override { return SensorKind::Video; }

    // 底层视频源已由调用方打开; 这里只接纳配置里的名字, **不重复打开**
    bool open(const SensorConfig& cfg) override;
    void close() override; // 非拥有: 不关闭底层

    bool read(SensorSample& out) override; // 读一帧 + 打 nowMs() 时间戳

    std::int64_t lastTimestampMs() const override { return last_ts_.load(); }
    std::uint64_t frameCount() const { return frames_.load(); }

private:
    IVideoSource& video_;
    std::string name_;
    std::atomic<std::int64_t> last_ts_{0};
    std::atomic<std::uint64_t> frames_{0};
};

} // namespace sensor
