// RoiUtils 单元测试
// 为什么值得测: 级联(Phase B, CascadeEngine::roiFor) 与 大模型复核
// (Phase C, main.cpp 的送审前裁剪) **共用**这一份"外扩 + 裁剪到边界"的
// 几何逻辑。它决定"送给二级/VLM 的那张小图到底包含什么" —— 改坏不会崩,
// 只会让精度悄悄变差(最难发现的那类回归)。
//
// 覆盖: padding=0 / 正常外扩 / 贴边与越界裁剪 / 越界框判空 /
// 负 padding 容错 / 取整(lround) / crop 深拷贝 / 空输入。
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
