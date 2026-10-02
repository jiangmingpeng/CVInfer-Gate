#pragma once
#include <string>
#include <opencv2/opencv.hpp>

struct DetectionResult {
    int class_id;
    float confidence;
    cv::Rect box; // x, y, width, height
    std::string label;

    // 级联/二级复核元数据
    // 默认未复核(class_id=-1), 对旧链路(单模型)零影响;
    // 由 CascadeEngine 在命中灰区并完成二级分类后回写, 供告警/入库使用。
    bool reviewed = false; // 是否经过二级分类器复核
    int sub_class_id = -1; // 二级分类类别(-1 = 无)
    float sub_confidence = 0.0f; // 二级分类置信度
    std::string sub_label; // 二级分类标签

    // 多模态决策级融合元数据
    // 均为带默认值的加法字段, 未启用融合时保持默认, 对既有链路(单模型/级联/
    // 复核)零影响。由 fusion::SensorFusion 在 sink 最前置阶段回写。
    bool fused = false; // 是否与某传感器目标融合过
    float vision_confidence = -1.0f; // 融合前的**视觉**置信度(-1 = 未融合)
    float sensor_confidence = -1.0f; // 参与融合的传感器置信度(-1 = 无)
    float distance_m = -1.0f; // 传感器测距(m; <0 = 未知)

    // 目标跟踪元数据
    // 加法字段(默认 -1 = 未跟踪), 对未启用跟踪的链路零影响。
    // 由 tracking::TargetTracker 在 sink 阶段就地回写(与融合同位置, 融合之后)。
    // 注: id 单调递增且永不复用 —— 复用会让告警去重把新目标误判成旧目标。
    // 注: gRPC 响应(proto)未带该字段, 见 docs/OVERVIEW 的待办。
    int track_id = -1; // 跨帧稳定目标 id(-1 = 无)
};