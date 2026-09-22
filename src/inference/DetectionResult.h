#pragma once
#include <string>
#include <opencv2/opencv.hpp>

struct DetectionResult {
    int class_id;
    float confidence;
    cv::Rect box; // x, y, width, height
    std::string label;
};