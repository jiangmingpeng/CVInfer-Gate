#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "review.grpc.pb.h"        // 由 proto/review.proto 生成(构建目录)
#include "review/IReviewService.h"
#include "utils/ConfigParser.h"

// ============================================================
// GrpcLlmReviewer (T20: 复用 gRPC 的大模型复核客户端)
// ------------------------------------------------------------
// 依据 review.endpoint 建立 channel, 调用 review::ReviewService::Review:
//   目标 ROI -> JPEG -> ReviewRequest -> (外部大模型服务) -> ReviewResponse
//
// 关键点:
//   - CreateChannel 不会立即连接, 故复核服务暂不可达时 init() 仍成功;
//     真正调用时以 UNAVAILABLE/DEADLINE_EXCEEDED 体现, 由上层降级处理,
//     绝不阻塞流水线。
//   - 每次调用设置 deadline = review.timeout_ms (满足"带超时"约束)。
//   - 线程安全: Stub 可被多线程并发调用(每个调用各用独立 ClientContext)。
// ============================================================
class GrpcLlmReviewer : public IReviewService {
public:
    GrpcLlmReviewer() = default;
    ~GrpcLlmReviewer() override = default;

    // 建立 channel/stub; 失败返回 false
    bool init(const ReviewConfig& cfg);

    const std::string& name() const override { return name_; }
    ReviewStatus review(const ReviewRequest& req, ReviewResult& out) override;

    // [T37] 探活: 调用 Health RPC 判断复核服务是否就绪。
    //   true  = 就绪, 或服务端未实现 Health(视为“可达”, 兼容任意旧服务端);
    //   false = 不可达 / 未就绪(detail 给出原因; 仅用于日志, 不阻断运行)。
    bool probe(std::string& detail);

private:
    std::string name_ = "grpc_llm_reviewer";
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<review::ReviewService::Stub> stub_;
    std::chrono::milliseconds timeout_{3000};
    std::string default_prompt_;

    // ---- [T37] 接入真实 VLM 服务端的传输/鉴权参数 ----
    bool health_check_ = true;             // init() 时是否探测一次 Health
    int max_message_size_mb_ = 16;         // gRPC 收发上限(MB)
    int keepalive_time_ms_ = 20000;        // HTTP/2 keepalive 时间(0 = 不设置)
    std::string auth_token_;               // 非空则每次调用带 authorization metadata
};
