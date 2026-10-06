#include "service/DetectionServiceImpl.h"

#include <chrono>

#include <opencv2/opencv.hpp>

#include "utils/Logger.h"
#include "utils/Metrics.h"

namespace {

// RPC 计数(带 method/code 标签)。调用频率是"每秒几次", 成本可忽略。
void countRpc(const char* method, const char* code) {
    std::string labels = "method=\"";
    labels += method;
    labels += "\",code=\"";
    labels += code;
    labels += "\"";
    metrics::Registry::instance().inc("cvinfer_grpc_requests_total", labels);
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

DetectionServiceImpl::DetectionServiceImpl(IDetector& detector, std::string auth_token,
                                           std::string version, std::int64_t start_ms)
    : detector_(detector),
      auth_(std::move(auth_token)),
      version_(std::move(version)),
      start_ms_(start_ms != 0 ? start_ms : nowMs()) {
    // 鉴权状态必须打出来: "以为开了其实没开" / "以为没开其实开了"
    // 都是排查噩梦(与复核鉴权同一条纪律)。
    CVLOG_INFO << "[鉴权] 主服务(50051): " << auth_.describe();

    // 指标声明(HELP 文本在这里给全, /metrics 就能自解释)
    auto& reg = metrics::Registry::instance();
    reg.declare("cvinfer_grpc_requests_total", metrics::Type::Counter,
                "gRPC 请求总数(按方法/结果码)", "method=\"Detect\",code=\"OK\"");
}

grpc::Status DetectionServiceImpl::Health(grpc::ServerContext* context,
                                          const inference::HealthRequest* /*request*/,
                                          inference::HealthResponse* response) {
    // 鉴权口径与 Detect 完全一致(50051 不暴露任何匿名接口)
    if (const grpc::Status st = auth_.require(*context); !st.ok()) {
        countRpc("Health", "UNAUTHENTICATED");
        CVLOG_WARN << "[鉴权] 拒绝 " << context->peer() << " 的 Health: " << st.error_message();
        return st;
    }
    countRpc("Health", "OK");

    response->set_serving(true);
    response->set_version(version_);
    response->set_uptime_ms(nowMs() - start_ms_);
    response->set_detail(std::string("detector=") + detector_.name());
    return grpc::Status::OK;
}

grpc::Status DetectionServiceImpl::Detect(grpc::ServerContext* context,
                                          const inference::DetectRequest* request,
                                          inference::DetectResponse* response) {
    // 鉴权 — 必须是本方法**第一件事**: 未通过就不碰任何业务逻辑
    // (也不把内部细节回给调用方)。token 为空时 require() 恒 OK。
    if (const grpc::Status st = auth_.require(*context); !st.ok()) {
        // 拒绝要留痕: 日志突增 = 有人在试 token, 或客户端配错了环境变量。
        countRpc("Detect", "UNAUTHENTICATED");
        CVLOG_WARN << "[鉴权] 拒绝 " << context->peer() << " 的 Detect: "
                   << st.error_message();
        return st;
    }
    // 1. 把收到的二进制数据解码为 cv::Mat
    std::vector<uchar> img_data(request->image_data().begin(), request->image_data().end());
    cv::Mat img = cv::imdecode(img_data, cv::IMREAD_COLOR);
    
    if (img.empty()) {
        countRpc("Detect", "BAD_REQUEST");
        response->set_success(false);
        response->set_message("图片解码失败");
        return grpc::Status::OK;
    }

    // 2. 借引擎/推理/后处理统一在模型内部完成 (YoloDetector); 与流水线线程安全共享
    std::vector<DetectionResult> detections;
    const DetectStatus status = detector_.detect(img, detections);
    if (status == DetectStatus::Busy) {
        countRpc("Detect", "BUSY");
        response->set_success(false);
        response->set_message("推理引擎繁忙, 请稍后重试");
        return grpc::Status::OK;
    }
    if (status != DetectStatus::Ok) {
        countRpc("Detect", "ERROR");
        response->set_success(false);
        response->set_message("推理失败");
        return grpc::Status::OK;
    }
    countRpc("Detect", "OK");

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