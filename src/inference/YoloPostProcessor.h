#pragma once
#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <vector>
#include "inference/DetectionResult.h"

class YoloPostProcessor {
public:
    YoloPostProcessor(float conf_threshold, float nms_threshold);
    ~YoloPostProcessor() = default;

    // 核心处理函数：输入原始张量，原图尺寸，输出解析好的结果
    std::vector<DetectionResult> process(const ov::Tensor& output_tensor, 
                                         const cv::Size& original_size,
                                         const std::vector<std::string>& labels);

private:
    float conf_threshold_;
    float nms_threshold_;

    // 计算两个框的 IOU
    float calculateIoU(const cv::Rect& box1, const cv::Rect& box2);
    // 执行 NMS
    void applyNMS(std::vector<DetectionResult>& detections);
};

//和Dete一起的