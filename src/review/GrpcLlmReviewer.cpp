#include "review/GrpcLlmReviewer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <grpc/grpc.h> // GRPC_ARG_* keepalive 常量

#include "utils/Logger.h"

namespace {
// 探活 deadline: 取 min(timeout, 1500ms) 且至少 200ms, 避免拖慢启动。
std::chrono::milliseconds probeDeadline(std::chrono::milliseconds timeout) {
    std::int64_t ms = timeout.count() > 0 ? timeout.count() : 1500;
    ms = std::min<std::int64_t>(ms, 1500);
    ms = std::max<std::int64_t>(ms, 200);
    return std::chrono::milliseconds(ms);
}
} // namespace

bool GrpcLlmReviewer::init(const ReviewConfig& cfg) {
    if (cfg.endpoint.empty()) {
        CVLOG_ERROR << "[GrpcLlmReviewer] review.endpoint 为空, 无法建立复核通道。";
        return false;
    }

    timeout_ = std::chrono::milliseconds(cfg.timeout_ms > 0 ? cfg.timeout_ms : 3000);
    default_prompt_ = cfg.prompt;

    // 传输/鉴权参数(带兜底默认值)
    health_check_        = cfg.health_check;
    max_message_size_mb_ = cfg.max_message_size_mb > 0 ? cfg.max_message_size_mb : 16;
    keepalive_time_ms_   = cfg.keepalive_time_ms > 0 ? cfg.keepalive_time_ms : 0;
    auth_token_          = cfg.auth_token;

    // 注意: CreateCustomChannel 不会立即建立连接(惰性), 因此即便复核服务暂时
    // 不可达, 这里也会成功; 真正的失败在 review() 中以 UNAVAILABLE 体现。
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(max_message_size_mb_ * 1024 * 1024);
    args.SetMaxSendMessageSize(max_message_size_mb_ * 1024 * 1024);
    if (keepalive_time_ms_ > 0) {
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, keepalive_time_ms_);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
        args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    }
    channel_ = grpc::CreateCustomChannel(cfg.endpoint, grpc::InsecureChannelCredentials(), args);
    if (!channel_) {
        CVLOG_ERROR << "[GrpcLlmReviewer] 创建 channel 失败: " << cfg.endpoint;
        return false;
    }
    stub_ = review::ReviewService::NewStub(channel_);
    if (!stub_) {
        CVLOG_ERROR << "[GrpcLlmReviewer] 创建 stub 失败: " << cfg.endpoint;
        return false;
    }

    CVLOG_INFO << "[GrpcLlmReviewer] 初始化完成: endpoint=" << cfg.endpoint
               << ", timeout=" << timeout_.count() << "ms"
               << ", max_msg=" << max_message_size_mb_ << "MB"
               << ", auth=" << (auth_token_.empty() ? "off" : "on");

    // 启动探活(仅日志, 不阻断): 让"复核服务是否就绪"在启动时一目了然。
    if (health_check_) {
        std::string detail;
        if (probe(detail)) {
            CVLOG_INFO << "[GrpcLlmReviewer] 复核服务就绪: " << detail;
        } else {
            CVLOG_WARN << "[GrpcLlmReviewer] 复核服务暂不可用(" << detail
                       << "); 运行期将按 alert_on_failure 兜底, 不阻塞流水线。";
        }
    }
    return true;
}

bool GrpcLlmReviewer::probe(std::string& detail) {
    detail.clear();
    if (!stub_) { detail = "stub 未初始化"; return false; }

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + probeDeadline(timeout_));
    if (!auth_token_.empty()) {
        ctx.AddMetadata("authorization", "Bearer " + auth_token_);
    }

    review::HealthRequest req;
    review::HealthResponse resp;
    const grpc::Status st = stub_->Health(&ctx, req, &resp);
    if (!st.ok()) {
        if (st.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
            detail = "服务端未实现 Health(视为可达)";
            return true;
        }
        detail = "Health 调用失败: " + st.error_message();
        return false;
    }
    detail = "backend=" + resp.backend() + ", model=" + resp.model();
    if (!resp.detail().empty()) detail += ", " + resp.detail();
    return resp.ready();
}

ReviewStatus GrpcLlmReviewer::review(const ReviewRequest& req, ReviewResult& out) {
    out = ReviewResult{};
    if (!stub_ || req.roi.empty()) return ReviewStatus::Failed;

    // 1. ROI -> JPEG (传输体积远小于原始像素)
    std::vector<uchar> jpeg;
    try {
        if (!cv::imencode(".jpg", req.roi, jpeg) || jpeg.empty()) {
            return ReviewStatus::Failed;
        }
    } catch (const cv::Exception& e) {
        CVLOG_WARN << "[GrpcLlmReviewer] ROI 编码失败: " << e.what();
        return ReviewStatus::Failed;
    }

    // 2. 组装请求
    review::ReviewRequest rpc_req;
    rpc_req.set_image_jpeg(jpeg.data(), jpeg.size());
    rpc_req.set_label(req.label);
    rpc_req.set_confidence(req.confidence);
    rpc_req.set_frame_seq(req.frame_seq);
    rpc_req.set_prompt(req.prompt.empty() ? default_prompt_ : req.prompt);
    // 送审 ROI 的像素尺寸(仅留痕/调试; 服务端可忽略)
    rpc_req.set_roi_meta("roi=" + std::to_string(req.roi.cols) + "x" +
                         std::to_string(req.roi.rows));

    // 3. 带 deadline 调用 (超时保护) + 可选鉴权 metadata
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);
    if (!auth_token_.empty()) {
        ctx.AddMetadata("authorization", "Bearer " + auth_token_);
    }

    const auto t0 = std::chrono::steady_clock::now();
    review::ReviewResponse rpc_resp;
    const grpc::Status st = stub_->Review(&ctx, rpc_req, &rpc_resp);
    const long long client_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
    if (!st.ok()) {
        if (st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) return ReviewStatus::Timeout;
        if (st.error_code() == grpc::StatusCode::UNAVAILABLE)        return ReviewStatus::Unavailable;
        return ReviewStatus::Failed;
    }
    if (!rpc_resp.ok()) return ReviewStatus::Failed;

    out.confirmed  = rpc_resp.confirmed();
    out.label      = rpc_resp.label();
    out.confidence = rpc_resp.confidence();
    out.reason     = rpc_resp.reason();
    // 可观测: 模型名取服务端上报; 耗时优先服务端上报, 否则用客户端测量值
    out.model      = rpc_resp.model();
    out.latency_ms = rpc_resp.latency_ms() > 0 ? rpc_resp.latency_ms() : client_ms;

    CVLOG_DEBUG << "[GrpcLlmReviewer] review ok: confirmed=" << out.confirmed
                << " label=" << out.label
                << " model=" << (out.model.empty() ? "(未上报)" : out.model)
                << " server_ms=" << rpc_resp.latency_ms()
                << " client_ms=" << client_ms;
    return ReviewStatus::Ok;
}
