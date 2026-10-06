#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "inference/IModel.h"
#include "utils/ConfigParser.h" // CascadeConfig

// ModelPoolManager (多模型池管理器 / 模型注册表)
// 背景: 原 main.cpp 只构造"单个 InferenceEnginePool"。
// 多模型级联需要"每个模型各自一套引擎池"。
//
// 本类用 ModelFactory 按配置批量构建模型并统一持有:
// - 每个 IModel(如 YoloDetector) 内部持有自己的 InferenceEnginePool
// => 即"每模型一池"; 旧的 InferenceEnginePool 保持不变, 直接复用。
// - 提供按名查找 (get / getDetector / getClassifier) 与 primaryDetector()。
// - close() 释放全部模型(引擎池随之析构), 供关闭时调用。
//
// pool_size 兜底: 若某模型 ModelConfig::pool_size <= 0, 则用
// init() 传入的 default_pool_size (通常 = app_cfg.pipeline.worker_threads),
// 以保持与旧版"引擎数 = worker 线程数"的行为一致。
class ModelPoolManager {
public:
    ModelPoolManager() = default;
    ~ModelPoolManager() = default;
    ModelPoolManager(const ModelPoolManager&) = delete;
    ModelPoolManager& operator=(const ModelPoolManager&) = delete;

    // 构建全部模型; 任一失败则整体失败并回滚(清空)
    bool init(const std::vector<ModelConfig>& models, int default_pool_size = 1);

    // 按名查找(未找到返回 nullptr)
    std::shared_ptr<IModel> get(const std::string& name) const;
    std::shared_ptr<IDetector> getDetector(const std::string& name) const;
    std::shared_ptr<IClassifier> getClassifier(const std::string& name) const;

    // 首个 detector(单模型兼容路径); 无则返回 nullptr
    std::shared_ptr<IDetector> primaryDetector() const;

    // 用已注册模型构建级联检测器(实现 IDetector):
    // primary   = getDetector(cfg.primary) 或 primaryDetector()
    // secondary = getClassifier(cfg.secondary) (为空/未找到 => 退化为单模型)
    // 无可用主检测器时返回 nullptr。
    // 注意: 返回的 CascadeEngine 持有对内部模型的 shared_ptr,
    // 故只要本管理器存活, 其依赖的模型即有效。
    std::shared_ptr<IDetector> buildCascade(const CascadeConfig& cfg) const;

    std::vector<std::string> names() const;
    std::size_t size() const { return models_.size(); }

    // 释放全部模型(引擎池随之关闭)
    void close();

private:
    std::vector<std::shared_ptr<IModel>> models_;
    std::unordered_map<std::string, std::shared_ptr<IModel>> by_name_;
};
