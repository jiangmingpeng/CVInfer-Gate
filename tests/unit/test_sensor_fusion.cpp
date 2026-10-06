// SensorFusion 单元测试 (Phase D 决策级融合)
// 为什么值得测: 融合直接改写 DetectionResult::confidence, 而这个置信度会
// **同时**喂给"画框 / 落库 / 告警判定 / 送 VLM 复核候选筛选"。融合算错 =
// 告警多报或少报, 且完全无声。三个步骤(时间对齐 -> 目标关联 -> 加权融合)
// 都是纯函数, 天生适合单测。
//
// 覆盖: 容差窗口边界 / 负容差 / 雷达(无框)按标签关联 / 红外(带框)按 IoU 关联 /
// 标签不相容 / 未分类标签视为相容 / 贪心匹配"一个传感器目标只用一次" /
// 权重与置信度 clamp / 采用传感器标签(仅能填补空标签) / emit_sensor_only 的有框与无框 /
// 非 decision 层直接放行 / **空视觉帧语义**(证据照常计数 + emit_sensor_only 可独立产出)。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "fusion/SensorFusion.h"
#include "inference/DetectionResult.h"
#include "sensor/ISensorSource.h"
#include "utils/ConfigParser.h"

namespace {

FusionConfig makeFusionCfg() {
    FusionConfig cfg;
    cfg.enabled = true;
    cfg.level = "decision";
    cfg.time_tolerance_ms = 50;
    cfg.match_iou = 0.30f;
    cfg.sensor_weight = 0.5f;
    cfg.emit_sensor_only = false;
    cfg.adopt_sensor_label = false;
    return cfg;
}

fusion::SensorFusion makeFusion() { return fusion::SensorFusion(makeFusionCfg()); }

DetectionResult makeVision(const std::string& label, float conf, const cv::Rect& box) {
    DetectionResult d;
    d.class_id = 0;
    d.label = label;
    d.confidence = conf;
    d.box = box;
    return d;
}

// 一路传感器采样, 内含单个目标; box 为空 = 该模态不提供像素框(典型雷达)
sensor::SensorSample makeSample(std::int64_t ts,
                                const std::string& label,
                                float conf,
                                const cv::Rect2f& box = cv::Rect2f(),
                                float distance_m = -1.0f) {
    sensor::SensorSample s;
    s.kind = sensor::SensorKind::Radar;
    s.timestamp_ms = ts;
    sensor::SensorTarget t;
    t.class_id = 0;
    t.label = label;
    t.confidence = conf;
    t.box = box;
    t.distance_m = distance_m;
    s.targets.push_back(t);
    return s;
}

} // namespace

// ---------------------------------------------------------------- 时间对齐

TEST(SensorFusionAlign, KeepsOnlySamplesWithinTolerance) {
    const std::vector<sensor::SensorSample> s{
        makeSample(1000, "person", 0.5f),
        makeSample(1030, "person", 0.5f),
        makeSample(1100, "person", 0.5f)};

    EXPECT_EQ(fusion::SensorFusion::align(s, 1000, 50).size(), 2u); // 1000, 1030
    EXPECT_EQ(fusion::SensorFusion::align(s, 1000, 0).size(), 1u); // 仅 1000
    EXPECT_EQ(fusion::SensorFusion::align(s, 1050, 25).size(), 1u); // 仅 1030
}

TEST(SensorFusionAlign, ToleranceBoundaryIsInclusive) {
    const std::vector<sensor::SensorSample> s{makeSample(1050, "person", 0.5f)};
    EXPECT_EQ(fusion::SensorFusion::align(s, 1000, 50).size(), 1u); // |Δt| == tol 仍算对齐
    EXPECT_EQ(fusion::SensorFusion::align(s, 1000, 49).size(), 0u);
}

TEST(SensorFusionAlign, NegativeToleranceBehavesAsZero) {
    const std::vector<sensor::SensorSample> s{makeSample(1000, "person", 0.5f)};
    EXPECT_EQ(fusion::SensorFusion::align(s, 1000, -5).size(), 1u);
    EXPECT_EQ(fusion::SensorFusion::align(s, 999, -5).size(), 0u);
}

// ---------------------------------------------------------------- 关联 + 融合

TEST(SensorFusionFuse, RadarWithoutBoxMatchesByLabelAndBlendsConfidence) {
    auto f = makeFusion();
    std::vector<DetectionResult> vision{makeVision("person", 0.5f, cv::Rect(10, 10, 50, 50))};
    const std::vector<sensor::SensorSample> samples{
        makeSample(1000, "person", 0.7f, cv::Rect2f(), 6.0f)};

    const auto st = f.fuse(vision, samples, 1000);

    ASSERT_EQ(vision.size(), 1u);
    EXPECT_EQ(st.matched, 1u);
    EXPECT_EQ(st.unmatched_sensor, 0u);

    EXPECT_TRUE(vision[0].fused);
    EXPECT_FLOAT_EQ(vision[0].vision_confidence, 0.5f); // 原始视觉置信度必须留痕
    EXPECT_FLOAT_EQ(vision[0].sensor_confidence, 0.7f);
    EXPECT_FLOAT_EQ(vision[0].confidence, 0.6f); // (1-0.5)*0.5 + 0.5*0.7
    EXPECT_FLOAT_EQ(vision[0].distance_m, 6.0f); // 雷达补的距离
    EXPECT_EQ(vision[0].label, "person"); // adopt_sensor_label=false
}

TEST(SensorFusionFuse, InfraredWithBoxRequiresOverlap) {
    auto f = makeFusion();

    // 完全重合 -> IoU=1 >= match_iou -> 关联
    std::vector<DetectionResult> hit{makeVision("person", 0.4f, cv::Rect(100, 100, 50, 50))};
    const auto st_hit = f.fuse(
        hit, {makeSample(1000, "person", 0.8f, cv::Rect2f(100, 100, 50, 50))}, 1000);
    EXPECT_EQ(st_hit.matched, 1u);
    EXPECT_TRUE(hit[0].fused);

    // 完全错开 -> 不关联; 视觉目标保持原样, 传感器目标计入 unmatched
    std::vector<DetectionResult> miss{makeVision("person", 0.4f, cv::Rect(400, 400, 50, 50))};
    const auto st_miss = f.fuse(
        miss, {makeSample(1000, "person", 0.8f, cv::Rect2f(0, 0, 50, 50))}, 1000);
    EXPECT_EQ(st_miss.matched, 0u);
    EXPECT_EQ(st_miss.unmatched_sensor, 1u);
    EXPECT_FALSE(miss[0].fused);
    EXPECT_FLOAT_EQ(miss[0].confidence, 0.4f); // 未融合 -> 置信度不许被动过
}

TEST(SensorFusionFuse, LabelIncompatibleDoesNotMatchButEmptyLabelDoes) {
    auto f = makeFusion();

    std::vector<DetectionResult> v1{makeVision("person", 0.5f, cv::Rect(0, 0, 10, 10))};
    const auto st1 = f.fuse(v1, {makeSample(1000, "car", 0.9f)}, 1000);
    EXPECT_EQ(st1.matched, 0u);
    EXPECT_EQ(st1.unmatched_sensor, 1u);

    // 传感器给不出细分类(空标签) -> 视为相容, 否则雷达永远无法参与融合
    std::vector<DetectionResult> v2{makeVision("person", 0.5f, cv::Rect(0, 0, 10, 10))};
    const auto st2 = f.fuse(v2, {makeSample(1000, "", 0.9f)}, 1000);
    EXPECT_EQ(st2.matched, 1u);
}

TEST(SensorFusionFuse, GreedyMatchConsumesEachSensorTargetOnlyOnce) {
    auto f = makeFusion();
    std::vector<DetectionResult> vision{makeVision("person", 0.5f, cv::Rect(0, 0, 10, 10)),
                                        makeVision("person", 0.6f, cv::Rect(100, 100, 10, 10))};
    // 只有 1 个传感器目标 -> 只能提升其中一个, 不允许"一个雷达点抬高两个视觉框"
    const auto st = f.fuse(vision, {makeSample(1000, "person", 0.9f)}, 1000);
    EXPECT_EQ(st.matched, 1u);
    EXPECT_EQ(st.unmatched_sensor, 0u);
    EXPECT_TRUE(vision[0].fused);
    EXPECT_FALSE(vision[1].fused);
}

TEST(SensorFusionFuse, OutOfWindowSamplesAreIgnored) {
    auto f = makeFusion();
    std::vector<DetectionResult> vision{makeVision("person", 0.5f, cv::Rect(10, 10, 50, 50))};
    const auto st = f.fuse(vision, {makeSample(5000, "person", 0.9f)}, 1000);

    EXPECT_EQ(st.samples_seen, 1u);
    EXPECT_EQ(st.aligned, 0u);
    EXPECT_EQ(st.matched, 0u);
    EXPECT_FALSE(vision[0].fused);
}

TEST(SensorFusionFuse, SensorWeightIsClampedToUnitRange) {
    FusionConfig cfg = makeFusionCfg();
    cfg.sensor_weight = 2.0f; // clamp01 -> 1.0: 完全采用传感器置信度
    fusion::SensorFusion g(cfg);

    std::vector<DetectionResult> vision{makeVision("person", 0.9f, cv::Rect(0, 0, 10, 10))};
    const auto st = g.fuse(vision, {makeSample(1000, "person", 1.0f)}, 1000);
    EXPECT_EQ(st.matched, 1u);
    EXPECT_FLOAT_EQ(vision[0].confidence, 1.0f);
    EXPECT_FLOAT_EQ(vision[0].vision_confidence, 0.9f);
}

// 关联的**前置条件**是标签相容(labelCompatible): 要么相等, 要么一侧为空。
// 因此 adopt_sensor_label 实际只有一种可达效果 —— 给"视觉未分类(空标签)"的目标
// 补上传感器给出的标签; 两侧标签**冲突**时根本不会关联, 也就谈不上改写。
TEST(SensorFusionFuse, AdoptSensorLabelFillsEmptyLabelButNeverOverridesConflicting) {
    FusionConfig cfg = makeFusionCfg();
    cfg.adopt_sensor_label = true;
    fusion::SensorFusion g(cfg);

    // (1) 视觉未分类(空标签) -> 相容 -> 关联 -> 标签被传感器填上
    std::vector<DetectionResult> unlabelled{makeVision("", 0.5f, cv::Rect(0, 0, 10, 10))};
    const auto st1 = g.fuse(unlabelled, {makeSample(1000, "pedestrian", 0.7f)}, 1000);
    EXPECT_EQ(st1.matched, 1u);
    EXPECT_EQ(unlabelled[0].label, "pedestrian");

    // (2) 标签冲突 -> 不相容 -> 不关联 -> 原标签保持(adopt 只能填补, 不能改写)
    std::vector<DetectionResult> labelled{makeVision("person", 0.5f, cv::Rect(0, 0, 10, 10))};
    const auto st2 = g.fuse(labelled, {makeSample(1000, "pedestrian", 0.7f)}, 1000);
    EXPECT_EQ(st2.matched, 0u);
    EXPECT_EQ(st2.unmatched_sensor, 1u);
    EXPECT_EQ(labelled[0].label, "person");
    EXPECT_FALSE(labelled[0].fused);
}

TEST(SensorFusionFuse, EmitSensorOnlyAppendsTargetsThatCarryABox) {
    FusionConfig cfg = makeFusionCfg();
    cfg.emit_sensor_only = true;
    fusion::SensorFusion g(cfg);

    // 视觉有目标但标签不相容 -> 传感器的这个目标成为"未关联" -> 追加为新目标
    std::vector<DetectionResult> vision{makeVision("person", 0.4f, cv::Rect(0, 0, 10, 10))};
    const auto st = g.fuse(
        vision, {makeSample(1000, "car", 0.9f, cv::Rect2f(400, 400, 20, 20), 3.0f)}, 1000);

    ASSERT_EQ(vision.size(), 2u);
    EXPECT_EQ(st.emitted_sensor_only, 1u);
    EXPECT_EQ(st.unmatched_sensor, 1u);
    EXPECT_FALSE(vision[0].fused);
    EXPECT_TRUE(vision[1].fused);
    EXPECT_EQ(vision[1].label, "car");
    EXPECT_FLOAT_EQ(vision[1].confidence, 0.9f);
    EXPECT_FLOAT_EQ(vision[1].vision_confidence, -1.0f); // 纯传感器目标, 无视觉置信度
    EXPECT_FLOAT_EQ(vision[1].distance_m, 3.0f);
}

TEST(SensorFusionFuse, EmitSensorOnlyRefusesTargetsWithoutBox) {
    FusionConfig cfg = makeFusionCfg();
    cfg.emit_sensor_only = true;
    fusion::SensorFusion g(cfg);

    std::vector<DetectionResult> vision{makeVision("person", 0.4f, cv::Rect(0, 0, 10, 10))};
    const auto st = g.fuse(vision, {makeSample(1000, "car", 0.9f)}, 1000); // 雷达无框

    // 无框的雷达点无法定位, 强行输出只会造出假框
    EXPECT_EQ(vision.size(), 1u);
    EXPECT_EQ(st.emitted_sensor_only, 0u);
    EXPECT_EQ(st.unmatched_sensor, 1u);
}

TEST(SensorFusionFuse, NonDecisionLevelIsNoop) {
    FusionConfig cfg = makeFusionCfg();
    cfg.level = "pixel"; // 目前只支持决策级
    fusion::SensorFusion g(cfg);

    std::vector<DetectionResult> vision{makeVision("person", 0.5f, cv::Rect(0, 0, 10, 10))};
    const auto st = g.fuse(vision, {makeSample(1000, "person", 0.9f)}, 1000);

    EXPECT_EQ(st.samples_seen, 1u);
    EXPECT_EQ(st.matched, 0u);
    EXPECT_FALSE(vision[0].fused);
}

// 空视觉帧语义
// 原实现是 `if (targets.empty() || vision.empty()) return st;`: 某帧**视觉一个目标都没有**
// 时整个融合阶段直接返回, 传感器证据既不计数也不产出 —— 而「雷达/红外测到了、视觉漏检了」
// 恰恰是最该靠融合兜住的场景, 等于把多模态的价值静默作废。
// 现在只在**没有任何传感器目标**时提前返回: 空视觉帧照常走未关联逻辑。
TEST(SensorFusionFuse, EmptyVisionStillAccountsSensorEvidence) {
    auto f = makeFusion(); // emit_sensor_only 默认 false

    std::vector<DetectionResult> vision; // 本帧视觉漏检, 但红外看到了
    const auto st = f.fuse(
        vision, {makeSample(1000, "person", 0.9f, cv::Rect2f(10, 10, 20, 20), 3.0f)}, 1000);

    EXPECT_TRUE(vision.empty()); // 未开启 emit_sensor_only -> 不产出目标
    EXPECT_EQ(st.samples_seen, 1u);
    EXPECT_EQ(st.aligned, 1u);
    EXPECT_EQ(st.targets, 1u);
    EXPECT_EQ(st.matched, 0u);
    EXPECT_EQ(st.unmatched_sensor, 1u); // ★ 关键: 证据必须被计入, 不再静默归零
    EXPECT_EQ(st.emitted_sensor_only, 0u);
}

TEST(SensorFusionFuse, EmptyVisionEmitsBoxCarryingSensorOnlyTarget) {
    FusionConfig cfg = makeFusionCfg();
    cfg.emit_sensor_only = true;
    fusion::SensorFusion g(cfg);

    std::vector<DetectionResult> vision;
    const auto st = g.fuse(
        vision, {makeSample(1000, "person", 0.9f, cv::Rect2f(10, 10, 20, 20), 3.0f)}, 1000);

    ASSERT_EQ(vision.size(), 1u); // 传感器目标独立成目标
    EXPECT_EQ(st.matched, 0u);
    EXPECT_EQ(st.unmatched_sensor, 1u);
    EXPECT_EQ(st.emitted_sensor_only, 1u);
    EXPECT_EQ(vision[0].label, "person");
    EXPECT_FLOAT_EQ(vision[0].confidence, 0.9f);
    EXPECT_FLOAT_EQ(vision[0].distance_m, 3.0f);
    EXPECT_EQ(vision[0].box, cv::Rect(10, 10, 20, 20));
    EXPECT_FLOAT_EQ(vision[0].vision_confidence, -1.0f); // 纯传感器目标
    EXPECT_TRUE(vision[0].fused);
}

// 无框雷达点在任何情况下都不输出(无法定位), 空视觉不是例外
TEST(SensorFusionFuse, EmptyVisionStillRefusesBoxlessSensorTarget) {
    FusionConfig cfg = makeFusionCfg();
    cfg.emit_sensor_only = true;
    fusion::SensorFusion g(cfg);

    std::vector<DetectionResult> vision;
    const auto st = g.fuse(vision, {makeSample(1000, "person", 0.9f)}, 1000); // 雷达无框

    EXPECT_TRUE(vision.empty());
    EXPECT_EQ(st.unmatched_sensor, 1u);
    EXPECT_EQ(st.emitted_sensor_only, 0u);
}
