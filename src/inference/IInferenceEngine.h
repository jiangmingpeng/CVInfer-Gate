#pragma once
#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <vector>
#include <string>
#include "utils/ConfigParser.h"

class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;
    // 初始化模型
    virtual bool init(const ModelConfig& config) = 0;
    // 执行推理，输入图像，输出原始张量
    virtual bool infer(const cv::Mat& input, std::vector<ov::Tensor>& outputs) = 0;
};