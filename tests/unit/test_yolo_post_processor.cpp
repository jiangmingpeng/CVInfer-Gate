// YoloPostProcessor 单元测试 —— 本项目"最不能错"的一段纯计算
// 为什么值得测: 解码(cxcywh -> xyxy) + 置信度过滤 + 手写 NMS + 边界裁剪
// 全部在这一个函数里, 它直接决定"框画在哪、有没有漏框/重框", 而且
// 刚为性能改写过它的内存访问顺序(行列互换 + 先求 max 再判阈值)。
// 这种"为了快而重写"的代码, 正是最需要回归保护的地方 —— 而且它**不需要
// 任何模型文件**: 张量可以手工构造。
//
// 张量布局(与实现严格一致): shape = [1, 4 + num_classes, num_boxes]
// row 0..3      = cx, cy, w, h          (每行沿 num_boxes 连续)
// row 4..4+C-1  = 该类别的分数
// 取值: data[row * num_boxes + box_index]
// ⚠️ 输入张量必须清零: 实现用 "> best_conf(初值 0)" 求每列最大类别分,
// 残留的随机大值会凭空造出目标。
//
// 覆盖: 单框解码+标签 / 阈值过滤 / 每框取最高分类别 / 同类重叠抑制(留高分) /
// 异类重叠不抑制 / NMS 阈值是**严格大于**的边界 / 上下边界裁剪 /
// 按原图尺寸缩放 / 标签不足回退 unknown / 多框 / 全零输入。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <openvino/openvino.hpp>

#include "inference/YoloPostProcessor.h"

namespace {

constexpr std::size_t kChannels = 6; // 4 + 2 个类别
constexpr std::size_t kBoxes = 4;

ov::Tensor makeZeroedOutput() {
    ov::Tensor t(ov::element::f32, ov::Shape{1, kChannels, kBoxes});
    float* p = t.data<float>();
    std::fill(p, p + kChannels * kBoxes, 0.0f);
    return t;
}

void setBox(ov::Tensor& t, std::size_t i, float cx, float cy, float w, float h) {
    float* p = t.data<float>();
    p[0 * kBoxes + i] = cx;
    p[1 * kBoxes + i] = cy;
    p[2 * kBoxes + i] = w;
    p[3 * kBoxes + i] = h;
}

void setScore(ov::Tensor& t, std::size_t i, std::size_t class_id, float score) {
    t.data<float>()[(4 + class_id) * kBoxes + i] = score;
}

std::vector<std::string> labels() { return {"person", "book"}; }

constexpr float kConf = 0.25f;
constexpr float kNms = 0.45f;

} // namespace

TEST(YoloPostProcessor, DecodesSingleBoxAndAttachesLabel) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 320.0f, 320.0f, 100.0f, 100.0f);
    setScore(t, 0, 1, 0.9f);

    const auto dets = pp.process(t, cv::Size(640, 640), labels());

    ASSERT_EQ(dets.size(), 1u);
    EXPECT_EQ(dets[0].class_id, 1);
    EXPECT_FLOAT_EQ(dets[0].confidence, 0.9f);
    EXPECT_EQ(dets[0].label, "book");
    // cxcywh(320,320,100,100) -> xyxy(270,270)-(370,370); 原图 640 -> 缩放 1.0
    EXPECT_EQ(dets[0].box, cv::Rect(270, 270, 100, 100));
}

TEST(YoloPostProcessor, FiltersDetectionsBelowConfidenceThreshold) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 100.0f, 100.0f, 20.0f, 20.0f);
    setScore(t, 0, 0, 0.24f); // < 0.25
    EXPECT_TRUE(pp.process(t, cv::Size(640, 640), labels()).empty());
}

TEST(YoloPostProcessor, PicksHighestScoringClassPerBox) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 100.0f, 100.0f, 20.0f, 20.0f);
    setScore(t, 0, 0, 0.60f);
    setScore(t, 0, 1, 0.80f);

    const auto dets = pp.process(t, cv::Size(640, 640), labels());
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_EQ(dets[0].class_id, 1);
    EXPECT_FLOAT_EQ(dets[0].confidence, 0.80f);
}

TEST(YoloPostProcessor, SuppressesOverlappingSameClassKeepingTopScore) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    // 两框几乎重合(IoU ≈ 0.82 > 0.45) 且同类 -> 只保留高分那个
    setBox(t, 0, 320.0f, 320.0f, 100.0f, 100.0f);
    setScore(t, 0, 0, 0.90f);
    setBox(t, 1, 325.0f, 325.0f, 100.0f, 100.0f);
    setScore(t, 1, 0, 0.80f);

    const auto dets = pp.process(t, cv::Size(640, 640), labels());
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_FLOAT_EQ(dets[0].confidence, 0.90f);
}

TEST(YoloPostProcessor, KeepsOverlappingBoxesOfDifferentClasses) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 320.0f, 320.0f, 100.0f, 100.0f);
    setScore(t, 0, 0, 0.90f);
    setBox(t, 1, 325.0f, 325.0f, 100.0f, 100.0f);
    setScore(t, 1, 1, 0.80f);

    // NMS 只在**同类别**内做, 否则"人"会把"书"吃掉
    EXPECT_EQ(pp.process(t, cv::Size(640, 640), labels()).size(), 2u);
}

TEST(YoloPostProcessor, NmsThresholdComparisonIsStrictlyGreater) {
    // IoU 恰好 == 阈值(0.5) 时**不**抑制: 实现是 `if (iou > nms_threshold_)`
    YoloPostProcessor pp(kConf, 0.5f);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 50.0f, 50.0f, 100.0f, 100.0f); // -> (0,0)-(100,100), 面积 10000
    setScore(t, 0, 0, 0.90f);
    setBox(t, 1, 50.0f, 25.0f, 100.0f, 50.0f); // -> (0,0)-(100,50),  交 5000 / 并 10000 = 0.5
    setScore(t, 1, 0, 0.80f);

    EXPECT_EQ(pp.process(t, cv::Size(640, 640), labels()).size(), 2u);
}

TEST(YoloPostProcessor, ClampsLowerBoundToZero) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 10.0f, 10.0f, 100.0f, 100.0f); // x1 = y1 = -40 -> 0
    setScore(t, 0, 0, 0.9f);

    const auto dets = pp.process(t, cv::Size(640, 640), labels());
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_EQ(dets[0].box.x, 0);
    EXPECT_EQ(dets[0].box.y, 0);
    EXPECT_EQ(dets[0].box.x + dets[0].box.width, 60);
    EXPECT_EQ(dets[0].box.y + dets[0].box.height, 60);
}

TEST(YoloPostProcessor, ClampsUpperBoundToWidthMinusOne) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 630.0f, 630.0f, 100.0f, 100.0f); // x2 = y2 = 680 -> 639 (= W-1)
    setScore(t, 0, 0, 0.9f);

    const auto dets = pp.process(t, cv::Size(640, 640), labels());
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_EQ(dets[0].box.x, 580);
    EXPECT_EQ(dets[0].box.x + dets[0].box.width, 639);
    EXPECT_EQ(dets[0].box.y + dets[0].box.height, 639);
}

TEST(YoloPostProcessor, ScalesCoordinatesFromModelInputToOriginalSize) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 320.0f, 320.0f, 100.0f, 100.0f);
    setScore(t, 0, 0, 0.9f);

    // 模型输入 640x640, 原图 1280x640 -> x 缩放 2.0, y 缩放 1.0
    const auto dets = pp.process(t, cv::Size(1280, 640), labels());
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_EQ(dets[0].box, cv::Rect(540, 270, 200, 100));
}

TEST(YoloPostProcessor, FallsBackToUnknownLabelWhenLabelsAreTooShort) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 320.0f, 320.0f, 50.0f, 50.0f);
    setScore(t, 0, 1, 0.9f);

    const auto dets = pp.process(t, cv::Size(640, 640), {"only_one_label"});
    ASSERT_EQ(dets.size(), 1u);
    EXPECT_EQ(dets[0].label, "unknown");
}

TEST(YoloPostProcessor, HandlesMultipleNonOverlappingBoxes) {
    YoloPostProcessor pp(kConf, kNms);
    ov::Tensor t = makeZeroedOutput();
    setBox(t, 0, 100.0f, 100.0f, 40.0f, 40.0f);
    setScore(t, 0, 0, 0.90f);
    setBox(t, 1, 500.0f, 500.0f, 40.0f, 40.0f);
    setScore(t, 1, 0, 0.80f);
    setBox(t, 2, 300.0f, 100.0f, 40.0f, 40.0f);
    setScore(t, 2, 1, 0.70f);

    EXPECT_EQ(pp.process(t, cv::Size(640, 640), labels()).size(), 3u);
}

TEST(YoloPostProcessor, AllZeroTensorProducesNoDetections) {
    YoloPostProcessor pp(kConf, kNms);
    // 全零(模型输出被清零/异常)时不能凭空造出目标
    EXPECT_TRUE(pp.process(makeZeroedOutput(), cv::Size(640, 640), labels()).empty());
}
