#pragma once

#include <string>
#include <vector>

#include <openvino/openvino.hpp>

#include "inference/IModel.h"

// ClassificationPostProcessor (分类后处理)
// 把分类模型(单张量输出)的原始张量解析为 Classification:
// [1, N] -> argmax -> (class_id, confidence, label)
//
// 归一化策略(自动判定, 无需配置):
// - 若输出"看起来像概率"(全部 ∈ [0,1] 且 Σ ≈ 1), 直接使用;
// - 否则视为 logits, 做数值稳定的 softmax。
// 这样既兼容已 softmax 的 YOLO-cls, 也兼容输出 logits 的分类网络;
// 关键是避免"对已归一化输出再做一次 softmax"造成置信度失真
// (如 [0.99,0.01] --softmax--> [0.73,0.27]), 否则会引发级联误否决。
//
// 仅支持 f32 输出; 其它类型/空输出返回 class_id = -1 (视为无效)。
class ClassificationPostProcessor {
public:
    explicit ClassificationPostProcessor(std::vector<std::string> labels);

    // 输出张量 -> 分类结果; 无效输出返回 class_id = -1
    Classification process(const ov::Tensor& output) const;

private:
    std::vector<std::string> labels_;
};
