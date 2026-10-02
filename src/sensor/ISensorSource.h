#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "utils/ConfigParser.h" // SensorConfig(与解析器共用同一份定义, 故放在 utils)

// 统一传感器抽象 (T23: Phase D 地基)
// 目标: 把"视频帧"与"非视频传感器(雷达/红外)"统一为**带时间戳的采样**,
// 使上层能在同一条时间轴上做对齐与决策级融合(T25)。
//
// 三种模态在本项目中的角色:
// Video    —— 主模态: 提供图像, 由 IDetector 产出 DetectionResult(带像素框);
// Radar    —— 提供距离/方位/置信度(通常**无**像素框);
// Infrared —— 提供热目标**框** + 置信度(可独立成目标源, 也可与视觉互补)。
//
// 重要约定(实现者务必遵守):
// 1) 时间戳一律用 nowMs()(steady_clock), 跨传感器/跨线程可比;
// 不要用 system_clock(受校时影响, 会出现"时间倒流");
// 2) read() 由**单一 poller 线程**串行调用(见 MultiSensorPipeline),
// 故实现内部无需为 read() 加锁; 不同实例之间互相独立, 可并发;
// 3) read() 不得永久阻塞: 若需限速, 必须能被 close() 打断,
// 否则 MultiSensorPipeline::stop() 无法 join(见 ReplaySensorSource)。
namespace sensor {

enum class SensorKind { Video = 0, Radar, Infrared };

inline const char* sensorKindToString(SensorKind k) {
    switch (k) {
        case SensorKind::Video:    return "video";
        case SensorKind::Radar:    return "radar";
        case SensorKind::Infrared: return "infrared";
    }
    return "unknown";
}

// 字符串(来自 config.yaml sensors[].kind) -> SensorKind; 非法返回 false
inline bool parseSensorKind(const std::string& s, SensorKind& out) {
    if (s == "video")    { out = SensorKind::Video;    return true; }
    if (s == "radar")    { out = SensorKind::Radar;    return true; }
    if (s == "infrared") { out = SensorKind::Infrared; return true; }
    return false;
}

// 统一时间戳: 单调时钟毫秒。inline 定义避免为一个 3 行函数单开 .cpp。
inline std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 单个目标观测(T24/T25): 某传感器在某一时刻对某个目标的观测
struct SensorTarget {
    int class_id = -1;
    std::string label; // 语义标签(如 "person"); 空 = 该模态未分类
    float confidence = 0.0f; // 该传感器对该目标的置信度 [0,1]
    cv::Rect2f box; // 目标框(像素); 空 = 该模态不提供像素框(如雷达)
    float distance_m = -1.0f; // 测距(m); <0 = 未知
    double azimuth_deg = 0.0; // 方位角(度); 仅雷达有意义
};

// 一次采样(T23): 统一时间戳 + 可选图像(视频) + 目标列表(非视频)
struct SensorSample {
    SensorKind kind = SensorKind::Radar;
    std::int64_t timestamp_ms = 0;
    cv::Mat frame; // 仅 Video 有意义(非视频留空)
    std::vector<SensorTarget> targets; // 非视频有意义; 视频为空(由 IDetector 产出)
};

// 传感器抽象: open -> 反复 read -> close
class ISensorSource {
public:
    virtual ~ISensorSource() = default;

    virtual const std::string& name() const = 0;
    virtual SensorKind kind() const = 0;

    // 打开设备/文件; 失败返回 false —— 调用方应**降级**(跳过该传感器)而非整体失败
    virtual bool open(const SensorConfig& cfg) = 0;
    // 关闭并**唤醒**可能正在限速/等待中的 read(); 必须幂等(可重复调用)
    virtual void close() = 0;
    // 读取一次采样; false = 无更多数据(文件回放结束)或已被 close()
    virtual bool read(SensorSample& out) = 0;

    // 最近一次成功 read() 的时间戳(ms); 0 = 尚未读到。
    // 用途: 融合对齐锚点(MultiSensorPipeline)、新鲜度判断。
    virtual std::int64_t lastTimestampMs() const { return 0; }
};

} // namespace sensor
