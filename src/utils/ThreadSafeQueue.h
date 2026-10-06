#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

// ThreadSafeQueue<T>  (有界 + 策略 + 统计 + 超时)
// 与原版差异:
// 1) 队列策略可配: DropOldest(默认, 实时) / Block(离线不丢帧)
// 2) 新增统计 pushed/popped/dropped, 丢弃可见(on_drop 回调)
// 3) pop 支持超时, 避免消费者永久阻塞
// 4) 新增 try_pop() 非阻塞出队
// 5) close() 语义统一, stop() 保留为兼容别名
// 6) push/pop 采用移动语义; 头文件不再依赖 OpenCV
//
// 兼容性: 原用法 ThreadSafeQueue<cv::Mat> q(10); q.push(...);
// q.pop(...); q.stop(); 全部保持不变。
template <typename T>
class ThreadSafeQueue {
public:
    enum class Policy { DropOldest, Block };
    enum class PushResult { Pushed, Dropped, Full, Closed };

    struct Stats {
        std::uint64_t pushed = 0; // 累计入队
        std::uint64_t popped = 0; // 累计出队
        std::uint64_t dropped = 0; // 累计丢弃(仅 DropOldest)
        std::size_t   size = 0; // 当前长度
        std::size_t   capacity = 0; // 容量上限
    };

    // 注意: on_drop 回调在持锁状态下被调用, 请保持轻量, 切勿再访问本队列
    explicit ThreadSafeQueue(std::size_t capacity = 24,
                             Policy policy = Policy::DropOldest,
                             std::function<void()> on_drop = nullptr)
        : capacity_(capacity == 0 ? 1 : capacity),
          policy_(policy),
          on_drop_(std::move(on_drop)) {}

    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    // 非阻塞入队 (Block 策略下满则返回 Full)
    PushResult try_push(T item) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (closed_) return PushResult::Closed;

        bool dropped = false;
        if (queue_.size() >= capacity_) {
            if (policy_ == Policy::DropOldest) {
                queue_.pop(); // 丢弃最旧帧
                ++dropped_;
                dropped = true;
                if (on_drop_) on_drop_();
            } else {
                return PushResult::Full;
            }
        }
        queue_.push(std::move(item));
        ++pushed_;
        lock.unlock();
        not_empty_.notify_one();
        return dropped ? PushResult::Dropped : PushResult::Pushed;
    }

    // 阻塞入队: DropOldest 委托 try_push; Block 等待空位或被关闭
    PushResult push(T item) {
        if (policy_ == Policy::DropOldest) {
            return try_push(std::move(item));
        }
        std::unique_lock<std::mutex> lock(mtx_);
        not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
        if (closed_) return PushResult::Closed;
        queue_.push(std::move(item));
        ++pushed_;
        lock.unlock();
        not_empty_.notify_one();
        return PushResult::Pushed;
    }

    // 阻塞出队; 返回 false 表示队列已关闭且为空
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mtx_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false; // closed_ && empty
        item = std::move(queue_.front());
        queue_.pop();
        ++popped_;
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // 带超时出队; timed_out 非空时回写是否因超时返回
    bool pop(T& item, std::chrono::milliseconds timeout, bool* timed_out = nullptr) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (timed_out) *timed_out = false;
        const bool ok = not_empty_.wait_for(
            lock, timeout, [this] { return closed_ || !queue_.empty(); });
        if (!ok) {
            if (timed_out) *timed_out = true;
            return false;
        }
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        ++popped_;
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // 非阻塞出队
    std::optional<T> try_pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        if (queue_.empty()) return std::nullopt;
        std::optional<T> out(std::move(queue_.front()));
        queue_.pop();
        ++popped_;
        lock.unlock();
        not_full_.notify_one();
        return out;
    }

    // 关闭队列: 唤醒所有等待者; 生产者不再入队, 消费者排空后退出
    void close() {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    // 兼容旧接口
    void stop() { close(); }

    bool closed() const {
        std::unique_lock<std::mutex> lock(mtx_);
        return closed_;
    }

    std::size_t size() const {
        std::unique_lock<std::mutex> lock(mtx_);
        return queue_.size();
    }

    Stats stats() const {
        std::unique_lock<std::mutex> lock(mtx_);
        Stats s;
        s.pushed = pushed_;
        s.popped = popped_;
        s.dropped = dropped_;
        s.size = queue_.size();
        s.capacity = capacity_;
        return s;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable not_empty_; // 消费者等待
    std::condition_variable not_full_; // Block 策略下生产者等待
    std::queue<T> queue_;
    std::size_t capacity_;
    Policy policy_;
    std::function<void()> on_drop_;
    bool closed_ = false;
    std::uint64_t pushed_ = 0;
    std::uint64_t popped_ = 0;
    std::uint64_t dropped_ = 0;
};