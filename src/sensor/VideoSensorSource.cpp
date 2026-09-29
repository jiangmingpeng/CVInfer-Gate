#include "sensor/VideoSensorSource.h"

namespace sensor {

bool VideoSensorSource::open(const SensorConfig& cfg) {
    // 非拥有适配器: 底层 IVideoSource 的生命周期(open/close)由 main 负责。
    // 这里若再次 video_.open() 会破坏底层状态(例如把已解码的视频重置到首帧),
    // 因此只接纳配置里的显示名。
    if (!cfg.name.empty()) name_ = cfg.name;
    last_ts_.store(0);
    frames_.store(0);
    return true;
}

void VideoSensorSource::close() {
    // 非拥有: 只清空本地计数, 不关闭底层视频源(main 会在收尾时统一 close)。
    last_ts_.store(0);
}

bool VideoSensorSource::read(SensorSample& out) {
    cv::Mat frame;
    if (!video_.read(frame)) return false;

    out.kind = SensorKind::Video;
    out.frame = frame;
    out.targets.clear();
    out.timestamp_ms = nowMs();

    last_ts_.store(out.timestamp_ms);
    ++frames_;
    return true;
}

}  // namespace sensor
