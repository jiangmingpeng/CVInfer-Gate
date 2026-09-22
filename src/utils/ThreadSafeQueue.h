#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <opencv2/opencv.hpp>

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t max_size = 30) : max_size_(max_size), stop_(false) {}

    // 推入数据（生产者）
    void push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 如果队列满了，丢弃最旧的一帧（跳帧策略），保证实时性
        if (queue_.size() >= max_size_) {
            queue_.pop();
        }
        queue_.push(item);
        cond_var_.notify_one();
    }

    // 取出数据（消费者），返回 false 表示队列已停止且为空
    bool pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_var_.wait(lock, [this] { return !queue_.empty() || stop_; });
        if (stop_ && queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        return true;
    }

    // 停止队列
    void stop() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cond_var_.notify_all();
    }

private:
    std::queue<T> queue_;
    size_t max_size_;
    bool stop_;
    std::mutex mutex_;
    std::condition_variable cond_var_;
};