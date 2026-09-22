#include "service/DetectionServiceImpl.h"
#include <opencv2/opencv.hpp>

DetectionServiceImpl::DetectionServiceImpl(OpenVINOEngine& engine, 
                                           YoloPostProcessor& post_processor,
                                           const std::vector<std::string>& labels)
    : engine_(engine), post_processor_(post_processor), labels_(labels) {}

grpc::Status DetectionServiceImpl::Detect(grpc::ServerContext* context,
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

    // 2. 推理与后处理
    std::vector<ov::Tensor> outputs;
    if (!engine_.infer(img, outputs)) {
        response->set_success(false);
        response->set_message("推理失败");
        return grpc::Status::OK;
    }

    auto detections = post_processor_.process(outputs[0], img.size(), labels_);

    // 3. 组装响应
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