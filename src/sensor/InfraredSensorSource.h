#pragma once

#include "sensor/ReplaySensorSource.h"

namespace sensor {

// InfraredSensorSource (红外骨架)
// 语义: 提供热目标**像素框** + 置信度——因此与视觉可直接做**空间关联**
// (IoU), 且能在低照度下补足视觉漏检。
//
// 当前实现: 复用 ReplaySensorSource 的 file/stub backend。
// 接入真实红外: 继承本类覆写 read(): 若同时提供热成像**图像**, 可先跑一个
// role=detector 的模型得到目标框, 再填 SensorTarget(见
// BehaviorClassifier/IDetector 复用思路)。
class InfraredSensorSource : public ReplaySensorSource {
public:
    InfraredSensorSource() : ReplaySensorSource("infrared", SensorKind::Infrared) {}
};

} // namespace sensor
