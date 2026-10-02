#pragma once

#include <memory>
#include <string>

#include "sensor/ISensorSource.h"
#include "sensor/InfraredSensorSource.h"
#include "sensor/RadarSensorSource.h"

namespace sensor {

// SensorSourceFactory (T24: 按配置构建非视频传感器)
// 与 ModelFactory 同构: 把"配置里的 kind 字符串"收敛到**唯一分派点**,
// 使 main / MultiSensorPipeline 都不必 switch(kind)。
//
// 约定:
// * 返回 nullptr 表示构建或 open 失败 —— 调用方应**跳过该传感器**并告警,
// 而不是让整条链路失败(一个传感器挂掉不该拖垮主视频链路);
// * kind=video **不在此构建**: 视频源由 main 持有(需与 VideoPipeline 共享
// 同一个 IVideoSource), 用 VideoSensorSource 适配(见 T23);
// * 工厂内部完成 open(), 故调用方拿到的实例即为"可用"状态。
inline std::shared_ptr<ISensorSource> createSensorSource(const SensorConfig& cfg) {
    SensorKind kind{};
    if (!parseSensorKind(cfg.kind, kind)) return nullptr;
    if (kind == SensorKind::Video) return nullptr; // 视频走 VideoSensorSource(main 适配)

    std::shared_ptr<ISensorSource> src;
    switch (kind) {
        case SensorKind::Radar:    src = std::make_shared<RadarSensorSource>();    break;
        case SensorKind::Infrared: src = std::make_shared<InfraredSensorSource>(); break;
        default: return nullptr;
    }
    if (!src->open(cfg)) return nullptr;
    return src;
}

} // namespace sensor
