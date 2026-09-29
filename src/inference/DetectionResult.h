#pragma once
#include <string>
#include <opencv2/opencv.hpp>

struct DetectionResult {
    int class_id;
    float confidence;
    cv::Rect box; // x, y, width, height
    std::string label;

    // ---- [T16+] 级联/二级复核元数据 ----
    // 默认未复核(class_id=-1), 对旧链路(单模型)零影响;
    // 由 CascadeEngine 在命中灰区并完成二级分类后回写, 供告警/入库使用。
    bool reviewed = false;         // 是否经过二级分类器复核
    int sub_class_id = -1;         // 二级分类类别(-1 = 无)
    float sub_confidence = 0.0f;   // 二级分类置信度
    std::string sub_label;         // 二级分类标签

    // ---- [T25] 多模态决策级融合元数据 ----
    // 均为带默认值的加法字段, 未启用融合时保持默认, 对既有链路(单模型/级联/
    // 复核)零影响。由 fusion::SensorFusion 在 sink 最前置阶段回写。
    bool fused = false;                     // 是否与某传感器目标融合过
    float vision_confidence = -1.0f;        // 融合前的**视觉**置信度(-1 = 未融合)
    float sensor_confidence = -1.0f;        // 参与融合的传感器置信度(-1 = 无)
    float distance_m = -1.0f;               // 传感器测距(m; <0 = 未知)
};