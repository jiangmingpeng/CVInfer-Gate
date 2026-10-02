#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "inference/IModel.h"
#include "inference/InferenceEnginePool.h"
#include "inference/YoloPostProcessor.h"

// YoloDetector (T13: 把"现有单模型链路"封装成 IDetector)
// 一个 YoloDetector = 一个 InferenceEnginePool + 一个 YoloPostProcessor + labels。
// - 内部引擎池大小来自 ModelConfig::pool_size (0 时由 ModelPoolManager 兜底)
// - detect() 内完成 [借引擎 -> infer -> 后处理], 与旧 VideoPipeline worker /
// DetectionServiceImpl 的行为完全等价, 只是把这段逻辑收敛到模型内部。
//
// 与旧版等价性:
// 旧 worker:  借引擎失败 -> skip;  infer 失败/空 -> skip;  post -> push result
// 新 detect:  借引擎超时 -> Busy;  infer 失败/空 -> Failed;  成功 -> Ok + detections
// 调用方:     != Ok 即 skip  (与旧版"失败跳帧"一致)
class YoloDetector : public IDetector {
public:
    YoloDetector() = default;
    ~YoloDetector() override = default;

    bool init(const ModelConfig& cfg) override;
    const std::string& name() const override { return name_; }
    ModelRole role() const override { return ModelRole::Detector; }

    DetectStatus detect(const cv::Mat& frame,
                        std::vector<DetectionResult>& out) override;

private:
    // 读取标签文件(逐行, 忽略空行)
    static std::vector<std::string> loadLabels(const std::string& path);

    std::string name_ = "yolov8_detector";
    ModelConfig cfg_;
    std::vector<std::string> labels_;
    std::unique_ptr<InferenceEnginePool> pool_;
    std::unique_ptr<YoloPostProcessor> post_;
    std::chrono::milliseconds acquire_timeout_{2000};
};
