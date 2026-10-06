#include "tracking/TargetTracker.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

// TargetTracker 单测 (目标跟踪 / track_id)
// 覆盖: id 首次分配 / 跨帧稳定、快速移动的距离兜底、超出可达范围分裂新 id、
// 标签隔离、遮挡滑行、老化退休后 **id 不复用**、min_hits 确认门限、
// 回退帧防御、enabled=false 短路、轨迹数有界、dwell 累计、多目标不串号

namespace {

using tracking::Config;
using tracking::TargetTracker;

Config makeCfg() {
    Config c;
    c.enabled = true;
    c.iou = 0.30f;
    c.dist_factor = 1.0f;
    c.max_age_ms = 1000;
    c.min_hits = 1;
    c.max_tracks = 256;
    return c;
}

DetectionResult det(const std::string& label, const cv::Rect& box) {
    DetectionResult d;
    d.class_id = 0;
    d.confidence = 0.9f;
    d.box = box;
    d.label = label;
    return d;
}

// 50x100 的框 => 半周长 75 => dist_factor=1.0 时质心允许移动 <= 75px
const cv::Rect kBox(100, 100, 50, 100); // 质心 (125,150)
const cv::Rect kBoxMoved(105, 100, 50, 100); // IoU≈0.82 => 重叠命中
const cv::Rect kBoxJumped(170, 100, 50, 100); // IoU=0, 质心距 70 <= 75 => 兜底命中

} // namespace

TEST(TargetTracker, FirstFrameAssignsFreshId) {
    TargetTracker t(makeCfg());
    std::vector<DetectionResult> d{det("person", kBox)};
    t.update(d, 1000, 0);
    EXPECT_EQ(d[0].track_id, 1);
    EXPECT_EQ(t.activeTracks(), 1u);
    EXPECT_EQ(t.stats().spawned, 1u);
    EXPECT_EQ(t.stats().frames, 1u);
}

TEST(TargetTracker, IdIsStableWhileBoxMoves) {
    TargetTracker t(makeCfg());
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0);
    std::vector<DetectionResult> d2{det("person", kBoxMoved)};
    t.update(d2, 1100, 1);
    EXPECT_EQ(d2[0].track_id, d1[0].track_id);
    EXPECT_EQ(t.stats().matched, 1u);
    EXPECT_EQ(t.stats().spawned, 1u);
}

TEST(TargetTracker, FastMoverKeepsIdViaDistanceFallback) {
    TargetTracker t(makeCfg());
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0);
    // 相邻帧框完全不重叠(IoU=0), 但质心只移了 70px => 距离兜底应接住
    std::vector<DetectionResult> d2{det("person", kBoxJumped)};
    t.update(d2, 1040, 1);
    EXPECT_EQ(d2[0].track_id, d1[0].track_id);
    EXPECT_EQ(t.stats().matched, 1u);
    EXPECT_EQ(t.stats().spawned, 1u);
}

TEST(TargetTracker, TargetBeyondReachSpawnsNewId) {
    TargetTracker t(makeCfg());
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0);
    std::vector<DetectionResult> d2{det("person", cv::Rect(400, 100, 50, 100))}; // 质心距 275
    t.update(d2, 1100, 1);
    EXPECT_EQ(d2[0].track_id, 2);
    EXPECT_EQ(t.stats().spawned, 2u);
}

TEST(TargetTracker, DifferentLabelsDoNotShareTracks) {
    TargetTracker t(makeCfg());
    std::vector<DetectionResult> d{det("person", kBox), det("car", kBox)};
    t.update(d, 1000, 0);
    EXPECT_NE(d[0].track_id, d[1].track_id);
    EXPECT_EQ(t.activeTracks(), 2u);
}

TEST(TargetTracker, TrackSurvivesBriefMissWithinMaxAge) {
    TargetTracker t(makeCfg()); // max_age = 1000
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0);
    std::vector<DetectionResult> none;
    t.update(none, 1500, 1); // 空帧(漏检): 轨迹滑行不退出
    EXPECT_EQ(t.activeTracks(), 1u);
    std::vector<DetectionResult> d2{det("person", kBox)};
    t.update(d2, 1800, 2); // 距上次命中 800ms < 1000 => 同一条轨迹
    EXPECT_EQ(d2[0].track_id, 1);
    const auto* tr = t.find(1);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->hits, 2);
    EXPECT_EQ(tr->misses, 0); // 重新命中后清零
}

TEST(TargetTracker, RetiredTrackIdIsNeverReused) {
    TargetTracker t(makeCfg()); // max_age = 1000
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0); // id = 1
    std::vector<DetectionResult> none;
    t.update(none, 2001, 1); // 1001ms 无命中 => 老化退休
    EXPECT_EQ(t.activeTracks(), 0u);
    EXPECT_EQ(t.stats().retired, 1u);
    std::vector<DetectionResult> d2{det("person", kBox)}; // 同一位置的新目标
    t.update(d2, 2100, 2);
    EXPECT_EQ(d2[0].track_id, 2); // 必须发新 id(复用会让去重漏报新目标)
}

TEST(TargetTracker, MinHitsDelaysIdUntilConfirmed) {
    Config c = makeCfg();
    c.min_hits = 2;
    TargetTracker t(c);
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0);
    EXPECT_EQ(d1[0].track_id, -1); // 首次命中: 未确认
    std::vector<DetectionResult> d2{det("person", kBoxMoved)};
    t.update(d2, 1100, 1);
    EXPECT_EQ(d2[0].track_id, 1); // 第二次命中: 确认并给出 id
}

TEST(TargetTracker, StaleFrameIsIgnoredDefensively) {
    TargetTracker t(makeCfg());
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 5);
    std::vector<DetectionResult> d2{det("person", kBox)};
    t.update(d2, 1100, 4); // seq 回退 => 忽略
    EXPECT_EQ(t.stats().stale_skipped, 1u);
    EXPECT_EQ(d2[0].track_id, -1);
    EXPECT_EQ(t.stats().matched, 0u);
}

TEST(TargetTracker, DisabledTrackerLeavesIdsUntouched) {
    Config c = makeCfg();
    c.enabled = false;
    TargetTracker t(c);
    std::vector<DetectionResult> d{det("person", kBox)};
    t.update(d, 1000, 0);
    EXPECT_EQ(d[0].track_id, -1);
    EXPECT_EQ(t.activeTracks(), 0u);
    EXPECT_EQ(t.stats().frames, 0u); // 完全短路
}

TEST(TargetTracker, TrackCountIsBoundedByMaxTracks) {
    Config c = makeCfg();
    c.max_tracks = 2;
    TargetTracker t(c);
    for (int i = 0; i < 5; ++i) {
        std::vector<DetectionResult> d{det("person", cv::Rect(i * 200, 0, 50, 50))};
        t.update(d, 1000 + i, static_cast<std::uint64_t>(i));
        EXPECT_EQ(d[0].track_id, i + 1); // id 单调递增、永不复用
    }
    EXPECT_LE(t.activeTracks(), 2u);
    EXPECT_EQ(t.stats().spawned, 5u);
    EXPECT_EQ(t.stats().retired, 3u); // 超限丢了最旧的 3 条
}

TEST(TargetTracker, DwellAccumulatesOverTrackedLifetime) {
    Config c = makeCfg();
    c.max_age_ms = 10000; // 让轨迹活够, 专测 dwell
    TargetTracker t(c);
    std::vector<DetectionResult> d1{det("person", kBox)};
    t.update(d1, 1000, 0);
    std::vector<DetectionResult> d2{det("person", kBox)};
    t.update(d2, 4000, 1);
    const auto* tr = t.find(1);
    ASSERT_NE(tr, nullptr);
    EXPECT_EQ(tr->dwellMs(), 3000);
    EXPECT_EQ(t.longestDwellMs(), 3000);
}

TEST(TargetTracker, TwoTargetsKeepDistinctIdsWhenBothMove) {
    TargetTracker t(makeCfg());
    const cv::Rect left(100, 100, 50, 100), right(400, 100, 50, 100);
    std::vector<DetectionResult> f1{det("person", left), det("person", right)};
    t.update(f1, 1000, 0);
    EXPECT_EQ(f1[0].track_id, 1);
    EXPECT_EQ(f1[1].track_id, 2);
    std::vector<DetectionResult> f2{det("person", cv::Rect(110, 100, 50, 100)),
                                    det("person", cv::Rect(390, 100, 50, 100))};
    t.update(f2, 1100, 1);
    EXPECT_EQ(f2[0].track_id, 1); // 身份跟着目标走, 不跟数组下标
    EXPECT_EQ(f2[1].track_id, 2);
}
