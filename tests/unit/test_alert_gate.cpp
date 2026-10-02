#include "utils/AlertGate.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

// AlertGate 单测 (T39: 告警去重)
// 覆盖: 滑动冷却窗(持续目标只告警一次 / 到期后重新告警) / 标签隔离 /
// 框重叠判定 / 退化输入(空框, cooldown=0) / enabled=false 短路 /
// 有界内存(丢最旧) / 多线程并发只放行一次 / IoU 数值

namespace {

using alert_gate::AlertGate;
using alert_gate::Config;

Config makeCfg() {
    Config c;
    c.enabled = true;
    c.iou = 0.30f;
    c.cooldown_ms = 5000;
    c.max_entries = 256;
    return c;
}

const cv::Rect kBox(100, 100, 50, 100); // 一个人
const cv::Rect kBoxShifted(105, 100, 50, 100); // 同一个人稍移动(IoU≈0.82 >= 0.30)

} // namespace

TEST(AlertGate, FirstAlertIsAllowedAndStatsAreCounted) {
    AlertGate g(makeCfg());
    EXPECT_TRUE(g.allow("person", kBox, 1000));
    EXPECT_EQ(g.allowed(), 1u);
    EXPECT_EQ(g.suppressed(), 0u);
    EXPECT_EQ(g.tracked(), 1u);
}

TEST(AlertGate, SameTargetWithinCooldownIsSuppressed) {
    AlertGate g(makeCfg());
    ASSERT_TRUE(g.allow("person", kBox, 1000));
    EXPECT_FALSE(g.allow("person", kBoxShifted, 1200)); // 同标签 + 框重叠 => 同一目标
    EXPECT_EQ(g.allowed(), 1u);
    EXPECT_EQ(g.suppressed(), 1u);
}

TEST(AlertGate, SlidingWindowKeepsPersistentTargetSilent) {
    AlertGate g(makeCfg());
    ASSERT_TRUE(g.allow("person", kBox, 0));
    // 连续 60 秒每秒命中一次: 冷却窗被不断刷新 => 始终抑制(只告警一次)
    for (int i = 1; i <= 60; ++i) {
        EXPECT_FALSE(g.allow("person", kBox, i * 1000)) << "t=" << i;
    }
    EXPECT_EQ(g.allowed(), 1u);
    EXPECT_EQ(g.suppressed(), 60u);
}

TEST(AlertGate, TargetIsAlertedAgainAfterCooldownElapsed) {
    AlertGate g(makeCfg()); // cooldown = 5000
    ASSERT_TRUE(g.allow("person", kBox, 1000));
    // 目标随后“消失”(这几秒内不再调用 allow) => 窗口不被刷新, 到期即重新放行。
    // 注意与滑动窗的区别: 若中间再命中一次, 窗口从那次命中重新计时
    // (见 SlidingWindowKeepsPersistentTargetSilent)。
    EXPECT_TRUE(g.allow("person", kBox, 6000)); // 1000+5000 => 恰好到期(边界 >=)
    EXPECT_EQ(g.allowed(), 2u);
    EXPECT_EQ(g.suppressed(), 0u);
}

TEST(AlertGate, DifferentLabelsDoNotSuppressEachOther) {
    AlertGate g(makeCfg());
    EXPECT_TRUE(g.allow("person", kBox, 1000));
    EXPECT_TRUE(g.allow("car", kBox, 1000)); // 同一位置但不同标签
    EXPECT_EQ(g.allowed(), 2u);
    EXPECT_EQ(g.suppressed(), 0u);
}

TEST(AlertGate, DistantTargetsOfSameLabelAreIndependent) {
    AlertGate g(makeCfg());
    EXPECT_TRUE(g.allow("person", kBox, 1000));
    EXPECT_TRUE(g.allow("person", cv::Rect(400, 100, 50, 100), 1000)); // 画面另一侧
    EXPECT_EQ(g.allowed(), 2u);
}

TEST(AlertGate, ZeroSizedBoxNeverMatchesAnything) {
    AlertGate g(makeCfg());
    EXPECT_TRUE(g.allow("person", cv::Rect(0, 0, 0, 0), 1000));
    EXPECT_TRUE(g.allow("person", cv::Rect(0, 0, 0, 0), 1100)); // 退化框 IoU=0 => 不判重
    EXPECT_EQ(g.allowed(), 2u);
}

TEST(AlertGate, DisabledGateAlwaysAllowsAndRemembersNothing) {
    Config c = makeCfg();
    c.enabled = false;
    AlertGate g(c);
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(g.allow("person", kBox, 1000 + i));
    EXPECT_EQ(g.allowed(), 5u);
    EXPECT_EQ(g.suppressed(), 0u);
    EXPECT_EQ(g.tracked(), 0u); // 不记忆 => 不占内存
}

TEST(AlertGate, ZeroCooldownDegeneratesToNoDedup) {
    Config c = makeCfg();
    c.cooldown_ms = 0;
    AlertGate g(c);
    EXPECT_TRUE(g.allow("person", kBox, 1000));
    EXPECT_TRUE(g.allow("person", kBox, 1000)); // 窗口为 0 => 不抑制
    EXPECT_EQ(g.suppressed(), 0u);
}

TEST(AlertGate, MemoryIsBoundedByMaxEntries) {
    Config c = makeCfg();
    c.max_entries = 2;
    AlertGate g(c);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(g.allow("person", cv::Rect(i * 100, 0, 50, 50), 1000)); // 互不重叠
    }
    EXPECT_EQ(g.allowed(), 5u);
    EXPECT_LE(g.tracked(), 2u); // 有界: 超限丢最旧
}

TEST(AlertGate, ResetClearsMemory) {
    AlertGate g(makeCfg());
    ASSERT_TRUE(g.allow("person", kBox, 1000));
    ASSERT_EQ(g.tracked(), 1u);
    g.reset();
    EXPECT_EQ(g.tracked(), 0u);
    EXPECT_TRUE(g.allow("person", kBox, 1100)); // 记忆已清空 => 重新放行
}

TEST(AlertGate, ConcurrentAllowOnSameTargetLetsExactlyOneThrough) {
    AlertGate g(makeCfg());
    constexpr int kThreads = 4;
    constexpr int kCalls = 200;
    std::atomic<int> passed{0};
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&g, &passed] {
            for (int i = 0; i < kCalls; ++i) {
                if (g.allow("person", kBox, 1000)) passed.fetch_add(1);
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(passed.load(), 1); // 同一目标并发 => 只放行一次
    EXPECT_EQ(g.allowed(), 1u);
    EXPECT_EQ(g.suppressed(), static_cast<std::uint64_t>(kThreads * kCalls - 1));
}

TEST(AlertGate, SameTrackIdIsSuppressedEvenWhenBoxMovedFar) {
    AlertGate g(makeCfg()); // cooldown = 5000, iou = 0.30
    // 同一个 track_id: 框已移到完全不重叠的位置, 仍判同一目标
    ASSERT_TRUE(g.allow("person", 7, kBox, 1000));
    EXPECT_FALSE(g.allow("person", 7, cv::Rect(400, 400, 50, 100), 1200));
    EXPECT_EQ(g.allowed(), 1u);
    EXPECT_EQ(g.suppressed(), 1u);
}

TEST(AlertGate, DifferentTrackIdAtSameBoxIsNotSuppressed) {
    AlertGate g(makeCfg());
    // 关键差异: 框一模一样但 id 不同 => 是新目标
    // (旧几何实现会把"换个目标站到同一位置"误判成重复告警)
    ASSERT_TRUE(g.allow("person", 1, kBox, 1000));
    EXPECT_TRUE(g.allow("person", 2, kBox, 1100));
    EXPECT_EQ(g.allowed(), 2u);
    EXPECT_EQ(g.suppressed(), 0u);
}

TEST(AlertGate, FallsBackToIouWhenTrackIdMissingOnOneSide) {
    AlertGate g(makeCfg());
    // 一边有 id 一边没有(跟踪器刚启用 / 目标未确认) => 退回几何重叠
    ASSERT_TRUE(g.allow("person", kBox, 1000)); // 无 id
    EXPECT_FALSE(g.allow("person", 5, kBox, 1100)); // 有 id, 对面没有 => 比框
    EXPECT_FALSE(g.allow("person", kBox, 1200)); // 仍无 id => 比框
    EXPECT_EQ(g.allowed(), 1u);
    EXPECT_EQ(g.suppressed(), 2u);
}

TEST(AlertGate, IouMatchesManualValue) {
    EXPECT_NEAR(AlertGate::iou(cv::Rect(0, 0, 10, 10), cv::Rect(5, 0, 10, 10)), 1.0f / 3.0f, 1e-6);
    EXPECT_FLOAT_EQ(AlertGate::iou(cv::Rect(0, 0, 10, 10), cv::Rect(0, 0, 10, 10)), 1.0f);
    EXPECT_FLOAT_EQ(AlertGate::iou(cv::Rect(0, 0, 10, 10), cv::Rect(100, 0, 10, 10)), 0.0f);
    // 对称性
    EXPECT_FLOAT_EQ(AlertGate::iou(cv::Rect(5, 0, 10, 10), cv::Rect(0, 0, 10, 10)),
                    AlertGate::iou(cv::Rect(0, 0, 10, 10), cv::Rect(5, 0, 10, 10)));
}
