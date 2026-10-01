// ============================================================
// [T38] ThreadSafeQueue 单元测试
// ------------------------------------------------------------
// 为什么先测它: 这个类是**所有线程边界的公共依赖**(帧队列 / 复核队列 /
// 落库队列), 一旦改坏就是全线抖动; 而它本身是纯内存逻辑, 无任何外部依赖
// —— 最适合做"零依赖、秒级"的回归保护。
//
// 覆盖: DropOldest 丢弃语义(丢最旧) / Block 策略返 Full / close 唤醒与排空 /
//       关闭后拒绝入队 / 统计计数 / 带超时 pop / on_drop 回调 / Block 阻塞被消费者唤醒。
// ============================================================
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "utils/ThreadSafeQueue.h"

namespace {
using IntQueue = ThreadSafeQueue<int>;
}  // namespace

TEST(ThreadSafeQueue, DropOldestKeepsNewestAndCountsDrops) {
    IntQueue q(3, IntQueue::Policy::DropOldest);
    for (int i = 0; i < 5; ++i) {
        q.try_push(i);
    }

    EXPECT_EQ(q.size(), 3u);
    const auto st = q.stats();
    EXPECT_EQ(st.pushed, 5u);
    EXPECT_EQ(st.dropped, 2u);
    EXPECT_EQ(st.capacity, 3u);

    // DropOldest 的定义: 留下的是**最后**入队的 3 个(2,3,4)
    for (int expected = 2; expected <= 4; ++expected) {
        auto v = q.try_pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, expected);
    }
    EXPECT_FALSE(q.try_pop().has_value());
}

TEST(ThreadSafeQueue, BlockPolicyReportsFullInsteadOfDropping) {
    IntQueue q(2, IntQueue::Policy::Block);
    EXPECT_EQ(q.try_push(1), IntQueue::PushResult::Pushed);
    EXPECT_EQ(q.try_push(2), IntQueue::PushResult::Pushed);
    EXPECT_EQ(q.try_push(3), IntQueue::PushResult::Full);
    // Block 策略**不允许**静默丢帧
    EXPECT_EQ(q.stats().dropped, 0u);
    EXPECT_EQ(q.size(), 2u);
}

TEST(ThreadSafeQueue, CloseStillDrainsQueuedItemsThenReturnsFalse) {
    IntQueue q(4);
    q.push(7);
    q.close();

    // 关闭 ≠ 丢弃: 已入队的元素仍应被取走(消费者排空后退出)
    int v = 0;
    ASSERT_TRUE(q.pop(v));
    EXPECT_EQ(v, 7);

    // 关闭且已空 -> 立即返回 false, 不再永久阻塞
    EXPECT_FALSE(q.pop(v));
}

TEST(ThreadSafeQueue, PushAfterCloseIsRejected) {
    IntQueue q(4);
    q.close();
    EXPECT_TRUE(q.closed());
    EXPECT_EQ(q.try_push(1), IntQueue::PushResult::Closed);
    EXPECT_EQ(q.push(1), IntQueue::PushResult::Closed);
}

TEST(ThreadSafeQueue, PopWithTimeoutReportsTimeout) {
    IntQueue q(4);
    int v = 0;
    bool timed_out = false;
    const bool ok = q.pop(v, std::chrono::milliseconds(20), &timed_out);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(timed_out);
}

TEST(ThreadSafeQueue, OnDropCallbackFiresOncePerDrop) {
    std::atomic<int> drops{0};
    IntQueue q(1, IntQueue::Policy::DropOldest, [&drops] { ++drops; });
    q.try_push(1);
    q.try_push(2);
    q.try_push(3);
    EXPECT_EQ(drops.load(), 2);
    EXPECT_EQ(q.size(), 1u);
}

TEST(ThreadSafeQueue, BlockPolicyPushWaitsUntilConsumerFreesSlot) {
    IntQueue q(1, IntQueue::Policy::Block);
    ASSERT_EQ(q.push(1), IntQueue::PushResult::Pushed);

    std::thread consumer([&q] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        int v = 0;
        q.pop(v);
    });

    const auto t0 = std::chrono::steady_clock::now();
    const auto r = q.push(2);   // 队列满 -> 必须阻塞到 consumer 取走
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
    consumer.join();

    EXPECT_EQ(r, IntQueue::PushResult::Pushed);
    EXPECT_GE(waited_ms, 100);   // 确实等过, 而不是立刻返回
}
