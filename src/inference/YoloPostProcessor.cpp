#include "inference/YoloPostProcessor.h"
#include <algorithm>
#include <cstdint>
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

    // ---- [T35] 缓存友好改造 (原实现见 git 历史) ----
    // 旧写法: for (i) for (c) data[(4+c)*num_boxes + i] —— 内层每次跨 33.6KB, 672k 次访问
    //         几乎全部 miss(实测后处理是 worker 关键路径上的主要开销之一)。
    // 新写法: 让 c 走外层, 内层 i 沿 8400 个 float(33.6KB, 能驻 L1/L2) 连续扫 ——
    //         顺序访问 + 可被硬件预取 + 自动向量化(vmaxps); 先逐列求 max 再单独判阈值。
    // 等价性: 用 `>` 比较, 平局时保留较小 class id; max 初值 0.0f 与原实现一致
    //         (全为负/零时同样会被 conf_threshold_ 滤掉, 行为不变)。
    detections.reserve(64);

    std::vector<float> best_conf(num_boxes, 0.0f);        // 33.6KB, 热数据
    std::vector<std::uint8_t> best_cls(num_boxes, 0);     // 8.4KB

    for (int c = 0; c < num_classes; ++c) {
        const float* row = data + static_cast<std::size_t>(4 + c) * num_boxes;
        for (int i = 0; i < num_boxes; ++i) {
            if (row[i] > best_conf[i]) {
                best_conf[i] = row[i];
                best_cls[i] = static_cast<std::uint8_t>(c);
            }
        }
    }

    // 坐标四行各自连续(原实现每行都重复算下标)
    const float* cx_row = data;
    const float* cy_row = data + num_boxes;
    const float* w_row  = data + 2 * num_boxes;
    const float* h_row  = data + 3 * num_boxes;
    const float max_x = static_cast<float>(original_size.width - 1);
    const float max_y = static_cast<float>(original_size.height - 1);

    for (int i = 0; i < num_boxes; ++i) {
        // 过滤低置信度(只对少数过阈框做下面的解析)
        if (best_conf[i] < conf_threshold_) continue;

        const int class_id = best_cls[i];

        // 解析坐标 (cx, cy, w, h) -> (x1, y1, x2, y2)
        const float cx = cx_row[i];
        const float cy = cy_row[i];
        const float w  = w_row[i];
        const float h  = h_row[i];

        float x1 = (cx - w / 2.0f) * scale_x;
        float y1 = (cy - h / 2.0f) * scale_y;
        float x2 = (cx + w / 2.0f) * scale_x;
        float y2 = (cy + h / 2.0f) * scale_y;

        // 裁剪到图像边界内
        x1 = std::max(0.0f, std::min(x1, max_x));
        y1 = std::max(0.0f, std::min(y1, max_y));
        x2 = std::max(0.0f, std::min(x2, max_x));
        y2 = std::max(0.0f, std::min(y2, max_y));

        DetectionResult det;
        det.class_id = class_id;
        det.confidence = best_conf[i];
        det.box = cv::Rect(cv::Point(x1, y1), cv::Point(x2, y2));
        det.label = (static_cast<std::size_t>(class_id) < labels.size()) ? labels[class_id] : "unknown";

        detections.push_back(det);
    }

    // 执行 NMS
    applyNMS(detections);
    return detections;
}