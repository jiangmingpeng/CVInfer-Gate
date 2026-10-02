#pragma once

#include <memory>

#include "inference/IModel.h"

// ModelFactory (T12: 模型工厂)
// 依据 ModelConfig::role 构建对应的模型实现:
// detector   -> YoloDetector         (T13, 已实现)
// classifier -> BehaviorClassifier   (T17, 单 ROI 分类)
// reviewer   -> 不走本工厂, 由 review 层(T20+)实现
//
// 返回已 init 成功的模型实例; 失败返回 nullptr。
class ModelFactory {
public:
    static std::shared_ptr<IModel> create(const ModelConfig& cfg);
};
