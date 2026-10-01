#include "review/ReviewScheduler.h"

#include <exception>

#include "utils/Logger.h"

ReviewScheduler::~ReviewScheduler() {
    stop();
}

bool ReviewScheduler::init(const ReviewConfig& cfg,
                           std::shared_ptr<IReviewService> service,
                           OutcomeCallback on_outcome) {
    stop();   // 重置旧资源

    if (!service) {
        CVLOG_ERROR << "[ReviewScheduler] 复核服务为空, 初始化失败。";
        return false;
    }
    cfg_ = cfg;
    service_ = std::move(service);
    on_outcome_ = std::move(on_outcome);

    const std::size_t cap = cfg_.queue_size < 1 ? 128 : cfg_.queue_size;
    // DropOldest: 复核积压时丢弃最旧任务, 绝不阻塞 submit 方(实时优先)。
    queue_ = std::make_unique<ThreadSafeQueue<ReviewRequest>>(
        cap, ThreadSafeQueue<ReviewRequest>::Policy::DropOldest);

    int n = cfg_.worker_threads < 1 ? 1 : cfg_.worker_threads;
    try {
        workers_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    } catch (const std::exception& e) {
        CVLOG_ERROR << "[ReviewScheduler] 启动复核线程失败: " << e.what();
        stop();
        return false;
    }
    enabled_ = true;

    CVLOG_INFO << "[ReviewScheduler] 初始化完成: service=" << service_->name()
               << ", queue=" << cap << ", workers=" << n;
    return true;
}

bool ReviewScheduler::submit(ReviewRequest job) {
    if (!enabled_.load() || !queue_) return false;
    const auto r = queue_->try_push(std::move(job));   // 非阻塞
    if (r == ThreadSafeQueue<ReviewRequest>::PushResult::Closed) return false;
    ++submitted_;
    return true;
}

void ReviewScheduler::stop() {
    if (queue_) queue_->close();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
    enabled_ = false;
}

void ReviewScheduler::workerLoop() {
    ReviewRequest job;
    while (queue_ && queue_->pop(job)) {
        ReviewResult res;
        const ReviewStatus st = service_ ? service_->review(job, res) : ReviewStatus::Failed;
        ++reviewed_;

        ReviewOutcome out;
        out.frame_seq = job.frame_seq;
        out.status = st;

        switch (st) {
            case ReviewStatus::Ok:
                out.confirmed  = res.confirmed;
                out.label      = res.label;
                out.confidence = res.confidence;
                out.reason     = res.reason;
                out.model      = res.model;              // [T37]
                out.latency_ms = res.latency_ms;         // [T37]
                if (res.confirmed) ++confirmed_; else ++rejected_;
                out.alert = res.confirmed;              // 确认才告警
                break;
            case ReviewStatus::Timeout:
                ++timeout_;
                out.alert = cfg_.alert_on_failure;      // 兜底策略(默认不告警)
                break;
            case ReviewStatus::Unavailable:
                ++unavailable_;
                out.alert = cfg_.alert_on_failure;
                break;
            case ReviewStatus::Failed:
            default:
                ++failed_;
                out.alert = cfg_.alert_on_failure;
                break;
        }

        if (on_outcome_) {
            try {
                on_outcome_(out);
            } catch (const std::exception& e) {
                CVLOG_WARN << "[ReviewScheduler] 结果回调异常: " << e.what();
            }
        }
    }
}

ReviewScheduler::Stats ReviewScheduler::stats() const {
    Stats s;
    s.submitted   = submitted_.load();
    s.dropped     = queue_ ? queue_->stats().dropped : 0;
    s.reviewed    = reviewed_.load();
    s.confirmed   = confirmed_.load();
    s.rejected    = rejected_.load();
    s.timeout     = timeout_.load();
    s.unavailable = unavailable_.load();
    s.failed      = failed_.load();
    return s;
}
