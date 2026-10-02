#pragma once

#include "sensor/ReplaySensorSource.h"

namespace sensor {

// RadarSensorSource (T24: 雷达骨架)
// 语义: 提供"距离 + 方位 + 置信度"(通常**无像素框**), 在决策级融合中作为
// 对视觉目标的一路**独立证据**(T25)。典型价值:
// 视觉说 "person 0.55"(灰区、不敢告警), 雷达在同方位测到 6m 处有
// 目标 0.7 -> 融合后置信度提升, 告警更可信(或反之被拉低)。
//
// 当前实现: 直接复用 ReplaySensorSource 的 file/stub backend(无硬件可联调)。
// 接入真实雷达: 继承本类覆写 read()(UDP/串口取一帧 -> SensorTarget),
// open() 中建立连接; 时间戳一律用 nowMs()。
class RadarSensorSource : public ReplaySensorSource {
public:
    RadarSensorSource() : ReplaySensorSource("radar", SensorKind::Radar) {}
};

} // namespace sensor
