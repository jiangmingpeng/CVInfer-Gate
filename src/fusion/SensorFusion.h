#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "inference/DetectionResult.h"
#include "sensor/ISensorSource.h"
#include "utils/ConfigParser.h"   // FusionConfig

namespace fusion {

// ============================================================
// SensorFusion (T25: 决策级融合)
// ------------------------------------------------------------
// "决策级"(decision-level) = 各模态**各自独立完成检测/判决**后再融合结论,
// 不共享中间特征(对应 Phase D 决策⑤"先做决策级融合")。
//
// 三步, 全部纯函数式(无成员可变状态 -> 易单测、天然线程安全):
//   1) 时间对齐 align():  保留 |sample.timestamp_ms - anchor| <= time_tolerance_ms 的采样;
//   2) 目标关联 associate():  视觉框 vs 传感器目标 ——
//        * 传感器给了像素框(红外): IoU >= match_iou **且** 标签相容;
//        * 传感器没给框(雷达):    退化为"标签相容"即可(无空间约束);
//        * 任一侧标签为空 => 视为相容(传感器常给不出细分类)。
//      采用**贪心**匹配: 每个传感器目标最多被一个视觉框占用, 避免一个雷达点
//      被多个视觉框争用而重复提升置信度。
//   3) 置信度融合 fuse():    c' = (1-w) * c_vision + w * c_sensor   (w = sensor_weight)
//      原始视觉置信度写入 vision_confidence, 供告警判定/追溯对比。
//
// 未关联的传感器目标: 默认**仅计数**。仅当 emit_sensor_only=true **且**该目标
// 自带像素框时才追加为新目标 —— 无框的雷达点无法定位, 强行输出会造出假框。
// 注: 「本帧视觉一个目标都没有」也照常走上面这套逻辑(不再整体提前返回) —— 否则
//     「传感器测到了、视觉漏检了」这个最该靠融合兜底的场景会被静默丢弃。
// ============================================================

struct FusionStats {
    std::uint64_t frames = 0;              // 参与融合的帧数(由编排层累加)
    std::uint64_t samples_seen = 0;        // 本轮缓冲内可见的采样数
    std::uint64_t aligned = 0;             // 落入时间窗的采样数
    std::uint64_t targets = 0;             // 时间窗内的传感器目标数
    std::uint64_t matched = 0;             // 关联成功的(视觉, 传感器)对
    std::uint64_t unmatched_sensor = 0;    // 未关联的传感器目标数
    std::uint64_t emitted_sensor_only = 0; // 因 emit_sensor_only 追加的目标数
};

class SensorFusion {
public:
    explicit SensorFusion(FusionConfig cfg) : cfg_(std::move(cfg)) {}

    const FusionConfig& config() const { return cfg_; }

    // 时间对齐(static: 便于单测与复用)
    static std::vector<sensor::SensorSample> align(const std::vector<sensor::SensorSample>& samples,
                                                  std::int64_t anchor_ms, int tol_ms);

    // 就地融合: 更新 vision 中匹配到的目标(fused/vision_confidence/sensor_confidence/
    // distance_m/label/confidence), 并返回**本轮**统计(由调用方累加)。
    FusionStats fuse(std::vector<DetectionResult>& vision,
                     const std::vector<sensor::SensorSample>& samples,
                     std::int64_t anchor_ms) const;

private:
    FusionConfig cfg_;
};

}  // namespace fusion
