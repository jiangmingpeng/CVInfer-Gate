// RoiUtils 单元测试
// 为什么值得测: 级联(Phase B, CascadeEngine::roiFor) 与 大模型复核
// (Phase C, main.cpp 的送审前裁剪) **共用**这一份"外扩 + 裁剪到边界"的
// 几何逻辑。它决定"送给二级/VLM 的那张小图到底包含什么" —— 改坏不会崩,
// 只会让精度悄悄变差(最难发现的那类回归)。
//
// 覆盖: padding=0 / 正常外扩 / 贴边与越界裁剪 / 越界框判空 /
// 负 padding 容错 / 取整(lround) / crop 深拷贝 / 空输入。
//
// 另覆盖**场景 ROI**(expandAndClampContext / cropContext, 占座类复核):
// scale 以中心放大 / min_side 补足 / 边界内推 / 默认值退化为旧行为。
#include <gtest/gtest.h>

#include "utils/RoiUtils.h"

namespace {
const cv::Size kImg(640, 480);
}

TEST(RoiUtils, ZeroPaddingKeepsBoxUnchanged) {
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(100, 100, 100, 100), kImg, 0.0f);
    EXPECT_EQ(r, cv::Rect(100, 100, 100, 100));
}

TEST(RoiUtils, PositivePaddingExpandsProportionally) {
    // 宽高各 100, padding=0.5 -> 上下左右各外扩 50
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(100, 100, 100, 100), kImg, 0.5f);
    EXPECT_EQ(r, cv::Rect(50, 50, 200, 200));
}

TEST(RoiUtils, PaddingIsRoundedToNearestPixel) {
    // lround(100 * 0.15) = 15 (不是截断的 14/15 之争)
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(100, 100, 100, 100), kImg, 0.15f);
    EXPECT_EQ(r, cv::Rect(85, 85, 130, 130));
}

TEST(RoiUtils, ClampsExpansionToImageBounds) {
    // 左上角目标外扩后越界 -> 裁到 (0,0), 右/下不受限
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(0, 0, 100, 100), kImg, 0.5f);
    EXPECT_EQ(r, cv::Rect(0, 0, 150, 150));
}

TEST(RoiUtils, NegativePaddingBehavesAsZero) {
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(10, 10, 20, 20), kImg, -1.0f);
    EXPECT_EQ(r, cv::Rect(10, 10, 20, 20));
}

TEST(RoiUtils, BoxPartiallyOutsideIsClampedNotDropped) {
    // x2=700 越界 -> 收到 640; 仍是有内容的 ROI(40x100)
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(600, 100, 100, 100), kImg, 0.0f);
    EXPECT_EQ(r, cv::Rect(600, 100, 40, 100));
}

TEST(RoiUtils, FullyOutsideBoxYieldsEmptyRect) {
    const cv::Rect r = roi_utils::expandAndClamp(cv::Rect(700, 700, 50, 50), kImg, 0.0f);
    EXPECT_TRUE(r.empty());
    EXPECT_EQ(r.width, 0);
}

TEST(RoiUtils, CropReturnsDeepCopyOfRequestedRegion) {
    cv::Mat img(480, 640, CV_8UC3, cv::Scalar(10, 20, 30));
    const cv::Mat roi = roi_utils::crop(img, cv::Rect(100, 100, 50, 40), 0.0f);
    ASSERT_FALSE(roi.empty());
    EXPECT_EQ(roi.size(), cv::Size(50, 40));

    // 必须是深拷贝: 复核是**跨线程/异步**使用 ROI(见 ReviewScheduler),
    // 若共享内存, 源帧被复用后送审内容会被污染。
    img.setTo(cv::Scalar(0, 0, 0));
    EXPECT_EQ(roi.at<cv::Vec3b>(0, 0), cv::Vec3b(10, 20, 30));
}

TEST(RoiUtils, CropOnEmptyFrameOrInvalidBoxReturnsEmpty) {
    const cv::Mat empty;
    EXPECT_TRUE(roi_utils::crop(empty, cv::Rect(0, 0, 10, 10), 0.1f).empty());

    const cv::Mat img(480, 640, CV_8UC3, cv::Scalar(0));
    EXPECT_TRUE(roi_utils::crop(img, cv::Rect(700, 700, 10, 10), 0.0f).empty());
}

// ------------------------------------------------ 场景 ROI(占座/复核)
// 动机: 只裁"一本书"(可能 107x135)时, VLM 看不到桌面/座位/周围的人,
//       对"占座"这类场景问题天然无法回答 => 送审前把目标框扩成"物体所在场景"。

TEST(RoiUtilsContext, DisabledScaleAndMinSideDegradeToPlainExpand) {
    // 老配置(不写新键) => 行为必须与改造前**逐像素一致**, 否则就是静默回归
    const cv::Rect a = roi_utils::expandAndClamp(cv::Rect(100, 100, 100, 100), kImg, 0.5f);
    const cv::Rect b =
        roi_utils::expandAndClampContext(cv::Rect(100, 100, 100, 100), kImg, 0.5f, 1.0f, 0);
    EXPECT_EQ(a, b);
    EXPECT_EQ(b, cv::Rect(50, 50, 200, 200));
}

TEST(RoiUtilsContext, ScaleGrowsAroundBoxCenter) {
    // 100x100 框, 中心 (350,250); scale=3 -> 300x300, 仍以该中心对齐
    const cv::Rect r =
        roi_utils::expandAndClampContext(cv::Rect(300, 200, 100, 100), kImg, 0.0f, 3.0f, 0);
    EXPECT_EQ(r, cv::Rect(200, 100, 300, 300));
}

TEST(RoiUtilsContext, MinSideFillsSmallBoxUpToRequestedSide) {
    // 20x20 的小物件: scale=3 只到 60x60(仍看不清桌面) => min_side=320 兜底
    // 中心 (310,210) -> 320x320 => 左/上各退 160
    const cv::Rect r =
        roi_utils::expandAndClampContext(cv::Rect(300, 200, 20, 20), kImg, 0.0f, 3.0f, 320);
    EXPECT_EQ(r, cv::Rect(150, 50, 320, 320));
}

TEST(RoiUtilsContext, MinSideSmallerThanScaledSizeIsIgnored) {
    const cv::Rect r =
        roi_utils::expandAndClampContext(cv::Rect(300, 200, 100, 100), kImg, 0.0f, 3.0f, 100);
    EXPECT_EQ(r, cv::Rect(200, 100, 300, 300));
}

TEST(RoiUtilsContext, PaddingIsAppliedBeforeScaling) {
    // padding=0.1 -> 120x120 的基框(290,190); scale=2 -> 240x240
    const cv::Rect r =
        roi_utils::expandAndClampContext(cv::Rect(300, 200, 100, 100), kImg, 0.1f, 2.0f, 0);
    EXPECT_EQ(r, cv::Rect(230, 130, 240, 240));
}

TEST(RoiUtilsContext, ClampsToBoundsByShiftingInward) {
    // 目标在右下角: 放大后越界, 向内推移(而不是截短导致物体跑到图外)
    const cv::Rect r =
        roi_utils::expandAndClampContext(cv::Rect(600, 440, 20, 20), kImg, 0.0f, 3.0f, 300);
    EXPECT_EQ(r, cv::Rect(340, 180, 300, 300));
}

TEST(RoiUtilsContext, DegenerateParamsAreTolerant) {
    // scale < 1 视为不减; min_side < 0 视为 0
    const cv::Rect a =
        roi_utils::expandAndClampContext(cv::Rect(100, 100, 100, 100), kImg, 0.5f, 0.25f, -5);
    EXPECT_EQ(a, cv::Rect(50, 50, 200, 200));
}

TEST(RoiUtilsContext, FullyOutsideBoxYieldsEmptyRect) {
    const cv::Rect r =
        roi_utils::expandAndClampContext(cv::Rect(700, 700, 50, 50), kImg, 0.0f, 3.0f, 320);
    EXPECT_TRUE(r.empty());
}

TEST(RoiUtilsContext, CropContextReturnsDeepCopy) {
    cv::Mat img(480, 640, CV_8UC3, cv::Scalar(10, 20, 30));
    const cv::Mat roi =
        roi_utils::cropContext(img, cv::Rect(100, 100, 50, 40), 0.0f, 2.0f, 0);
    ASSERT_FALSE(roi.empty());
    EXPECT_EQ(roi.size(), cv::Size(100, 80)); // 50x40 -> scale 2

    img.setTo(cv::Scalar(0, 0, 0)); // 深拷贝: 源帧被复用也不污染送审内容
    EXPECT_EQ(roi.at<cv::Vec3b>(0, 0), cv::Vec3b(10, 20, 30));
}

TEST(RoiUtilsContext, CropContextOnEmptyFrameOrInvalidBoxReturnsEmpty) {
    const cv::Mat empty;
    EXPECT_TRUE(roi_utils::cropContext(empty, cv::Rect(0, 0, 10, 10), 0.1f, 3.0f, 320).empty());

    const cv::Mat img(480, 640, CV_8UC3, cv::Scalar(0));
    EXPECT_TRUE(roi_utils::cropContext(img, cv::Rect(700, 700, 10, 10), 0.0f, 3.0f, 320).empty());
}
