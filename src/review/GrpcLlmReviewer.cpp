#include "review/GrpcLlmReviewer.h"

#include <vector>

#include "utils/Logger.h"

bool GrpcLlmReviewer::init(const ReviewConfig& cfg) {
    if (cfg.endpoint.empty()) {
        CVLOG_ERROR << "[GrpcLlmReviewer] review.endpoint 为空, 无法建立复核通道。";
        return false;
    }

    timeout_ = std::chrono::milliseconds(cfg.timeout_ms > 0 ? cfg.timeout_ms : 3000);
    default_prompt_ = cfg.prompt;

    // 注意: CreateChannel 不会立即建立连接(惰性), 因此即便复核服务暂时不可达,
    //       这里也会成功; 真正的失败在 review() 中以 UNAVAILABLE 体现。
    channel_ = grpc::CreateChannel(cfg.endpoint, grpc::InsecureChannelCredentials());
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
               << ", timeout=" << timeout_.count() << "ms";
    return true;
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

    // 3. 带 deadline 调用 (超时保护)
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + timeout_);

    review::ReviewResponse rpc_resp;
    const grpc::Status st = stub_->Review(&ctx, rpc_req, &rpc_resp);
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
    return ReviewStatus::Ok;
}
