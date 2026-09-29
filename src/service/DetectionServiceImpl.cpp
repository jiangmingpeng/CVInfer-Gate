#include "service/DetectionServiceImpl.h"
#include <opencv2/opencv.hpp>

DetectionServiceImpl::DetectionServiceImpl(IDetector& detector)
    : detector_(detector) {}

grpc::Status DetectionServiceImpl::Detect(grpc::ServerContext* /*context*/,
                                          const inference::DetectRequest* request,
                                          inference::DetectResponse* response) {
    // 1. 把收到的二进制数据解码为 cv::Mat
    std::vector<uchar> img_data(request->image_data().begin(), request->image_data().end());
    cv::Mat img = cv::imdecode(img_data, cv::IMREAD_COLOR);
    
    if (img.empty()) {
        response->set_success(false);
        response->set_message("图片解码失败");
        return grpc::Status::OK;
    }

    // 2. 借引擎/推理/后处理统一在模型内部完成 (YoloDetector); 与流水线线程安全共享
    std::vector<DetectionResult> detections;
    const DetectStatus status = detector_.detect(img, detections);
    if (status == DetectStatus::Busy) {
        response->set_success(false);
        response->set_message("推理引擎繁忙, 请稍后重试");
        return grpc::Status::OK;
    }
    if (status != DetectStatus::Ok) {
        response->set_success(false);
        response->set_message("推理失败");
        return grpc::Status::OK;
    }

    // 4. 组装响应
    response->set_success(true);
    response->set_message("检测成功");
    for (const auto& det : detections) {
        auto* res = response->add_detections();
        res->set_class_id(det.class_id);
        res->set_label(det.label);
        res->set_confidence(det.confidence);
        res->set_x1(det.box.x);
        res->set_y1(det.box.y);
        res->set_x2(det.box.x + det.box.width);
        res->set_y2(det.box.y + det.box.height);
    }

    return grpc::Status::OK;
}