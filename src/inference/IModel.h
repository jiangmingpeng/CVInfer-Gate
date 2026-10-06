#pragma once

#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "inference/DetectionResult.h"
#include "utils/ConfigParser.h"

// IModel / IDetector / IClassifier (模型抽象层)
// 背景: 原项目只有单一 InferenceEngine (IInferenceEngine 直接吐原始张量),
// "多模型级联 / 大模型复核" 无法表达。这里在 IInferenceEngine(裸张量)
// 之上新增一层"模型"抽象: 一个模型 = 引擎池 + 专属后处理 + 标签。
//
// 分层:
// IInferenceEngine  —— 低层: init(ModelConfig) / infer(cv::Mat, vector<ov::Tensor>&)
// IModel            —— 中层: 角色化模型基类 (init / name / role)
// ├─ IDetector    —— 整图 -> 检测框 (YOLO 等)
// └─ IClassifier  —— ROI -> 类别   (行为/细分类分类器等)
//
// 说明: 级联(CascadeEngine) 也实现 IDetector, 因此 VideoPipeline /
// DetectionServiceImpl 只依赖 IDetector 即可, 无需感知级联细节。

// 模型角色 (由 ModelConfig::role 字符串映射而来)
enum class ModelRole { Detector, Classifier, Reviewer };

inline ModelRole modelRoleFromString(const std::string& s) {
    if (s == "classifier") return ModelRole::Classifier;
    if (s == "reviewer")   return ModelRole::Reviewer;
    return ModelRole::Detector; // 默认(含未知值): 检测器
}

inline const char* modelRoleToString(ModelRole r) {
    switch (r) {
        case ModelRole::Classifier: return "classifier";
        case ModelRole::Reviewer:   return "reviewer";
        case ModelRole::Detector:
        default:                    return "detector";
    }
}

// 检测调用状态 (用于区分"引擎繁忙"与"推理失败", 同时给流水线与 gRPC 复用)
enum class DetectStatus {
    Ok = 0, // 检测完成 (即使没有任何目标也是 Ok)
    Busy, // 引擎池繁忙/借引擎超时, 可重试
    Failed // 推理或后处理失败
};

// 分类结果 (供 IClassifier 使用)
struct Classification {
    int class_id = -1;
    float confidence = 0.0f;
    std::string label;
};

// 模型基类
class IModel {
public:
    virtual ~IModel() = default;

    // 依据配置构建内部资源(引擎池/后处理等); 失败返回 false
    virtual bool init(const ModelConfig& cfg) = 0;

    // 模型名(多模型注册/查找用)
    virtual const std::string& name() const = 0;

    // 模型角色
    virtual ModelRole role() const = 0;
};

// 检测器: 整图 -> 检测框
class IDetector : public IModel {
public:
    // 返回检测状态; out 回写检测结果(Ok 时有效)
    virtual DetectStatus detect(const cv::Mat& frame,
                                std::vector<DetectionResult>& out) = 0;
};

// 分类器: ROI -> 类别
// 返回状态与 IDetector::detect 对称: 便于级联区分"引擎繁忙(Busy, 保留主结果)"
// 与"推理失败(Failed, 同样降级保留)"。
class IClassifier : public IModel {
public:
    virtual DetectStatus classify(const cv::Mat& roi, Classification& out) = 0;
};
