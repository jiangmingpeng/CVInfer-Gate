#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "inference/IModel.h"
#include "inference/InferenceEnginePool.h"
#include "inference/ClassificationPostProcessor.h"

// BehaviorClassifier (二级分类器, 实现 IClassifier)
// 一个 BehaviorClassifier = 一个 InferenceEnginePool + 分类后处理 + 标签。
// - 输入: 单张 ROI (外扩裁剪后的目标小图)
// - 引擎自带预处理(blobFromImage: resize/1/255/BGR->RGB), 故此处只管后处理
// - classify() 内完成 [借引擎 -> infer -> argmax], 状态语义与 YoloDetector 一致:
// 借引擎超时 -> Busy; 推理/后处理无效 -> Failed; 成功 -> Ok
//
// 用途: 作为级联(CascadeEngine)的二级模型, 对主模型的"灰区"目标做细分类
// (如 安全帽佩戴/行为识别)。pool_size 来自 ModelConfig::pool_size。
class BehaviorClassifier : public IClassifier {
public:
    BehaviorClassifier() = default;
    ~BehaviorClassifier() override = default;

    bool init(const ModelConfig& cfg) override;
    const std::string& name() const override { return name_; }
    ModelRole role() const override { return ModelRole::Classifier; }

    // 对单 ROI 分类; 返回状态, out 在 Ok 时有效
    DetectStatus classify(const cv::Mat& roi, Classification& out) override;

private:
    // 读取标签文件(逐行, 忽略空行)
    static std::vector<std::string> loadLabels(const std::string& path);

    std::string name_ = "classifier";
    ModelConfig cfg_;
    std::unique_ptr<InferenceEnginePool> pool_;
    std::unique_ptr<ClassificationPostProcessor> post_;
    std::chrono::milliseconds acquire_timeout_{2000};
};
