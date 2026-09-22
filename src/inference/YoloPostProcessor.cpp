#include "inference/YoloPostProcessor.h"
#include <algorithm>
#include <iostream>

YoloPostProcessor::YoloPostProcessor(float conf_threshold, float nms_threshold)
    : conf_threshold_(conf_threshold), nms_threshold_(nms_threshold) {}

float YoloPostProcessor::calculateIoU(const cv::Rect& box1, const cv::Rect& box2) {
    int x1 = std::max(box1.x, box2.x);
    int y1 = std::max(box1.y, box2.y);
    int x2 = std::min(box1.x + box1.width, box2.x + box2.width);
    int y2 = std::min(box1.y + box1.height, box2.y + box2.height);

    int intersection_area = std::max(0, x2 - x1) * std::max(0, y2 - y1);
    int union_area = box1.area() + box2.area() - intersection_area;

    return union_area == 0 ? 0.0f : static_cast<float>(intersection_area) / union_area;
}

void YoloPostProcessor::applyNMS(std::vector<DetectionResult>& detections) {
    // 按置信度从高到低排序
    std::sort(detections.begin(), detections.end(),
              [](const DetectionResult& a, const DetectionResult& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<DetectionResult> result;
    std::vector<bool> is_suppressed(detections.size(), false);

    for (size_t i = 0; i < detections.size(); ++i) {
        if (is_suppressed[i]) continue;
        result.push_back(detections[i]);

        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (is_suppressed[j]) continue;
            // 同类别才做 NMS
            if (detections[i].class_id == detections[j].class_id) {
                if (calculateIoU(detections[i].box, detections[j].box) > nms_threshold_) {
                    is_suppressed[j] = true;
                }
            }
        }
    }
    detections = result;
}

std::vector<DetectionResult> YoloPostProcessor::process(const ov::Tensor& output_tensor, 
                                                        const cv::Size& original_size,
                                                        const std::vector<std::string>& labels) {
    std::vector<DetectionResult> detections;
    
    // 获取张量数据指针
    const float* data = output_tensor.data<float>();
    ov::Shape shape = output_tensor.get_shape(); // [1, 84, 8400]
    
    int num_classes = shape[1] - 4; // 80
    int num_boxes = shape[2];       // 8400

    // 计算缩放比例
    float scale_x = static_cast<float>(original_size.width) / 640.0f;
    float scale_y = static_cast<float>(original_size.height) / 640.0f;

    // 遍历 8400 个候选框
    // 注意：张量内存布局是 [1, 84, 8400]，所以第 c 个通道第 i 个框的索引是 c * 8400 + i
    for (int i = 0; i < num_boxes; ++i) {
        // 找到最大类别概率
        float max_conf = 0.0f;
        int class_id = -1;
        for (int c = 0; c < num_classes; ++c) {
            float conf = data[(4 + c) * num_boxes + i];
            if (conf > max_conf) {
                max_conf = conf;
                class_id = c;
            }
        }

        // 过滤低置信度
        if (max_conf < conf_threshold_) continue;

        // 解析坐标 (cx, cy, w, h) -> (x1, y1, x2, y2)
        float cx = data[0 * num_boxes + i];
        float cy = data[1 * num_boxes + i];
        float w  = data[2 * num_boxes + i];
        float h  = data[3 * num_boxes + i];

        float x1 = (cx - w / 2.0f) * scale_x;
        float y1 = (cy - h / 2.0f) * scale_y;
        float x2 = (cx + w / 2.0f) * scale_x;
        float y2 = (cy + h / 2.0f) * scale_y;

        // 裁剪到图像边界内
        x1 = std::max(0.0f, std::min(x1, static_cast<float>(original_size.width - 1)));
        y1 = std::max(0.0f, std::min(y1, static_cast<float>(original_size.height - 1)));
        x2 = std::max(0.0f, std::min(x2, static_cast<float>(original_size.width - 1)));
        y2 = std::max(0.0f, std::min(y2, static_cast<float>(original_size.height - 1)));

        DetectionResult det;
        det.class_id = class_id;
        det.confidence = max_conf;
        det.box = cv::Rect(cv::Point(x1, y1), cv::Point(x2, y2));
        det.label = (class_id < labels.size()) ? labels[class_id] : "unknown";
        
        detections.push_back(det);
    }

    // 执行 NMS
    applyNMS(detections);
    return detections;
}