#include "inference/InferenceEnginePool.h"

#include "inference/OpenVINOEngine.h"

#include <iostream>

bool InferenceEnginePool::init(const ModelConfig& config, int pool_size) {
    if (pool_size < 1) pool_size = 1;

    // 重新初始化时先清空旧资源
    engines_.clear();
    std::queue<std::shared_ptr<IInferenceEngine>>().swap(free_);
    {
        std::unique_lock<std::mutex> lock(mtx_);
        closed_ = false;
    }

    for (int i = 0; i < pool_size; ++i) {
        auto engine = std::make_shared<OpenVINOEngine>();
        if (!engine->init(config)) {
            std::cerr << "[InferenceEnginePool] 第 " << (i + 1) << "/" << pool_size
                      << " 个引擎初始化失败, 已回滚。" << std::endl;
            engines_.clear();
            return false;
        }
        engines_.push_back(engine);
    }

    for (auto& e : engines_) {
        free_.push(e);   // 初始化后全部空闲
    }

    std::cout << "[InferenceEnginePool] 初始化完成, 引擎数量: " << engines_.size() << std::endl;
    return true;
}

std::shared_ptr<IInferenceEngine> InferenceEnginePool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mtx_);
    if (closed_) return nullptr;

    const bool ok = cv_.wait_for(lock, timeout, [this] { return closed_ || !free_.empty(); });
    if (!ok || free_.empty()) return nullptr;   // 超时或已关闭

    auto engine = free_.front();
    free_.pop();
    return engine;
}

void InferenceEnginePool::release(std::shared_ptr<IInferenceEngine> engine) {
    if (!engine) return;
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (closed_) return;   // 已关闭则直接丢弃(引擎随局部 shared_ptr 析构)
        free_.push(std::move(engine));
    }
    cv_.notify_one();
}

std::size_t InferenceEnginePool::size() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return engines_.size();
}

std::size_t InferenceEnginePool::available() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return free_.size();
}

void InferenceEnginePool::close() {
    {
        std::unique_lock<std::mutex> lock(mtx_);
        closed_ = true;
    }
    cv_.notify_all();
}

EngineGuard::EngineGuard(InferenceEnginePool& pool, std::chrono::milliseconds timeout)
    : pool_(pool), engine_(pool.acquire(timeout)) {}

EngineGuard::~EngineGuard() {
    if (engine_) pool_.release(std::move(engine_));
}
