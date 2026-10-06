#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "review/IReviewService.h"
#include "utils/ConfigParser.h"
#include "utils/ThreadSafeQueue.h"

// 复核结论(异步回投给上层)
struct ReviewOutcome {
    std::uint64_t frame_seq = 0; // 回收键: 关联回具体帧
    ReviewStatus status = ReviewStatus::Failed; // 调用状态
    bool confirmed = false; // 原始复核结论(status==Ok 时有效)
    bool alert = false; // 最终是否应告警(已套用兜底策略)
    std::string label; // 复核标签
    float confidence = 0.0f; // 复核置信度
    std::string reason; // 复核理由
    std::string model; // 产出结论的模型名(可空)
    std::int64_t latency_ms = 0; // 服务端处理耗时(ms; 0 = 未上报)
};

// ReviewScheduler (异步复核调度器)
// 目标: 让"大模型复核"完全不阻塞解码/推理/sink 线程。
// - submit(): 非阻塞入队(有界 DropOldest, 绝不阻塞调用方)
// - N 个 worker 线程: 出队 -> IReviewService::review -> 回调 on_outcome
// - 单次调用超时由 IReviewService 实现(deadline)负责
// - 结果按 frame_seq 回收(ReviewOutcome::frame_seq), 供上层关联到具体帧
//
// 线程模型: submit() 可从任意线程调用; on_outcome 在复核 worker 线程执行,
// 故回调内应保持轻量(写告警是异步入队, 已足够轻)。
class ReviewScheduler {
public:
    using OutcomeCallback = std::function<void(const ReviewOutcome&)>;

    struct Stats {
        std::uint64_t submitted = 0; // 入队尝试
        std::uint64_t dropped = 0; // 队满被丢弃(QueuePolicy=DropOldest)
        std::uint64_t reviewed = 0; // 完成调用(含失败)
        std::uint64_t confirmed = 0;
        std::uint64_t rejected = 0;
        std::uint64_t timeout = 0;
        std::uint64_t unavailable = 0;
        std::uint64_t failed = 0;
    };

    ReviewScheduler() = default;
    ~ReviewScheduler();

    ReviewScheduler(const ReviewScheduler&) = delete;
    ReviewScheduler& operator=(const ReviewScheduler&) = delete;

    bool init(const ReviewConfig& cfg,
              std::shared_ptr<IReviewService> service,
              OutcomeCallback on_outcome);

    bool submit(ReviewRequest job); // 非阻塞; 不可用时返回 false
    void stop(); // 关闭队列 + join 全部复核线程(幂等)
    Stats stats() const;
    bool enabled() const { return enabled_.load(); }

private:
    void workerLoop();

    ReviewConfig cfg_;
    std::shared_ptr<IReviewService> service_;
    OutcomeCallback on_outcome_;
    std::unique_ptr<ThreadSafeQueue<ReviewRequest>> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> enabled_{false};

    std::atomic<std::uint64_t> submitted_{0};
    std::atomic<std::uint64_t> reviewed_{0};
    std::atomic<std::uint64_t> confirmed_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> timeout_{0};
    std::atomic<std::uint64_t> unavailable_{0};
    std::atomic<std::uint64_t> failed_{0};
};
