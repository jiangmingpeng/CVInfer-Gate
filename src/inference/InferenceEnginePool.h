#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "inference/IInferenceEngine.h"
#include "utils/ConfigParser.h"

// ============================================================
// InferenceEnginePool (T4: 推理引擎池)
// ------------------------------------------------------------
// 背景: OpenVINOEngine 内部仅持有一个 ov::InferRequest, 不是线程安全的。
//       原 main.cpp 中"视频流水线线程"与"gRPC 服务线程"共用同一个引擎
//       实例 => 数据竞争 / 崩溃。本类用引擎池隔离并复用推理资源。
//
// 使用方式:
//   InferenceEnginePool pool;
//   pool.init(model_cfg, 2);
//   {
//       auto engine = pool.acquire(std::chrono::milliseconds(2000));
//       if (engine && engine->infer(img, outputs)) { ... }
//   }   // shared_ptr 析构时需手工 release, 或用 EngineGuard 自动归还
// ============================================================
class InferenceEnginePool {
public:
    InferenceEnginePool() = default;
    ~InferenceEnginePool() = default;
    InferenceEnginePool(const InferenceEnginePool&) = delete;
    InferenceEnginePool& operator=(const InferenceEnginePool&) = delete;

    // 创建 pool_size 个独立引擎实例; 任一初始化失败则整体失败并清理
    bool init(const ModelConfig& config, int pool_size);

    // 借出一个空闲引擎; 超时(或已关闭)返回 nullptr
    std::shared_ptr<IInferenceEngine> acquire(std::chrono::milliseconds timeout);

    // 归还引擎
    void release(std::shared_ptr<IInferenceEngine> engine);

    std::size_t size() const;        // 引擎总数
    std::size_t available() const;   // 当前空闲数

    // 关闭池: 唤醒所有等待者, 之后 acquire 返回 nullptr
    void close();

private:
    std::vector<std::shared_ptr<IInferenceEngine>> engines_;
    std::queue<std::shared_ptr<IInferenceEngine>> free_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;     // 等待空闲引擎
    bool closed_ = false;
};

// ============================================================
// EngineGuard (RAII): 出作用域自动归还, 避免忘记 release
// ============================================================
class EngineGuard {
public:
    EngineGuard(InferenceEnginePool& pool, std::chrono::milliseconds timeout);
    ~EngineGuard();

    EngineGuard(const EngineGuard&) = delete;
    EngineGuard& operator=(const EngineGuard&) = delete;

    bool valid() const { return static_cast<bool>(engine_); }
    IInferenceEngine* operator->() const { return engine_.get(); }
    IInferenceEngine& operator*() const { return *engine_; }

private:
    InferenceEnginePool& pool_;
    std::shared_ptr<IInferenceEngine> engine_;
};
