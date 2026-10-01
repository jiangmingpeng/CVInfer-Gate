// ============================================================
// 复核服务联调客户端 (T37: 接真 VLM 的"探针")
// ------------------------------------------------------------
// 用途: 不启动整条视频流水线的前提下, 单独验证 Phase C 的**复核服务端**
//       (scripts/mock_review_server.py 或 vlm_review/server.py) 是否可用。
//
// 为什么它可信: 它与流水线内的 GrpcLlmReviewer 调**同一份 review.proto**、
//       发同样的 ReviewRequest(ROI JPEG + label + confidence + prompt)。
//       因此 "本客户端能跑通" ⇒ "流水线内的复核链路也能跑通"。
//
// 用法:
//   ./review_client [addr] [image_path] [prompt] [label] [conf] [timeout_ms]
//   ./review_client 127.0.0.1:50052 test_frame.jpg "判断该人员是否未佩戴安全帽"
//   省略 image_path 时会生成一张合成图(无需任何素材即可冒烟)。
//
// [T41] 鉴权: 环境变量 REVIEW_AUTH_TOKEN 非空时, 每个 RPC(含 Health)都带
//       authorization: Bearer <token> —— 与服务端 vlm_review/server.py 对称。
//       用环境变量而非命令行参数: token 不会落进 shell 历史/ps。
//
// 退出码: 0=Ok  1=Failed  2=服务不可达(Unavailable)  3=超时(Timeout)  4=入参错误
//         5=鉴权失败(UNAUTHENTICATED: token 缺失/错误)
// ============================================================
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include <grpcpp/grpcpp.h>
#include <grpc/grpc.h>   // GRPC_ARG_* keepalive 常量

#include "review.grpc.pb.h"

namespace {

std::string argOr(int argc, char** argv, int idx, const std::string& def) {
    return (argc > idx && argv[idx] && argv[idx][0] != '\0') ? std::string(argv[idx]) : def;
}

// [T41] 鉴权 token 从环境变量读(不占命令行参数 => 不落进 shell 历史/ps)
std::string authTokenFromEnv() {
    const char* t = std::getenv("REVIEW_AUTH_TOKEN");
    return (t && *t) ? std::string(t) : std::string();
}

// 与服务端对称: token 非空则每个 RPC(含 Health)都带 authorization metadata
void addAuth(grpc::ClientContext& ctx, const std::string& token) {
    if (!token.empty()) ctx.AddMetadata("authorization", "Bearer " + token);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string addr       = argOr(argc, argv, 1, "127.0.0.1:50052");
    const std::string image_path = argOr(argc, argv, 2, "");
    const std::string prompt     = argOr(argc, argv, 3, "判断该人员是否未佩戴安全帽");
    const std::string label      = argOr(argc, argv, 4, "person");
    const float confidence       = static_cast<float>(std::atof(argOr(argc, argv, 5, "0.72").c_str()));
    const int timeout_ms         = (argc > 6) ? std::atoi(argv[6]) : 20000;
    const std::string auth_token = authTokenFromEnv();   // [T41]

    // 1. channel: 与服务端对齐 keepalive / 消息上限
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(16 * 1024 * 1024);
    args.SetMaxSendMessageSize(16 * 1024 * 1024);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

    auto channel = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
    auto stub = review::ReviewService::NewStub(channel);
    if (!stub) {
        std::cerr << "[review_client] 创建 stub 失败: " << addr << std::endl;
        return 4;
    }

    auto deadline = [timeout_ms] {
        return std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
    };

    // 2. 先探活(Health): 未实现则视为可达(兼容旧服务端)
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(deadline());
        addAuth(ctx, auth_token);   // [T41] Health 也要带(与服务端对称)
        review::HealthRequest hreq;
        review::HealthResponse hresp;
        const grpc::Status hs = stub->Health(&ctx, hreq, &hresp);
        if (hs.ok()) {
            std::cout << "[review_client] 服务就绪: backend=" << hresp.backend()
                      << " model=" << hresp.model()
                      << " ready=" << (hresp.ready() ? "true" : "false");
            if (!hresp.detail().empty()) std::cout << " (" << hresp.detail() << ")";
            std::cout << std::endl;
        } else if (hs.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
            std::cout << "[review_client] 服务端未实现 Health(视为可达)。" << std::endl;
        } else {
            std::cerr << "[review_client] Health 失败: " << hs.error_code()
                      << " - " << hs.error_message() << std::endl;
            if (hs.error_code() == grpc::StatusCode::UNAVAILABLE) return 2;
            if (hs.error_code() == grpc::StatusCode::UNAUTHENTICATED) return 5;   // [T41]
        }
    }

    // 3. 准备一张 ROI 图(传入文件优先, 否则合成一张带矩形的灰图)
    cv::Mat img;
    if (!image_path.empty()) {
        img = cv::imread(image_path, cv::IMREAD_COLOR);
        if (img.empty()) {
            std::cerr << "[review_client] 无法读取图片: " << image_path << std::endl;
            return 4;
        }
        std::cout << "[review_client] 载入图片: " << image_path
                  << " (" << img.cols << "x" << img.rows << ")" << std::endl;
    } else {
        img = cv::Mat(240, 240, CV_8UC3, cv::Scalar(90, 90, 90));
        cv::rectangle(img, cv::Rect(60, 40, 120, 160), cv::Scalar(200, 200, 200), -1);
        std::cout << "[review_client] 使用合成 ROI (未提供图片路径)。" << std::endl;
    }

    std::vector<uchar> jpeg;
    cv::imencode(".jpg", img, jpeg);
    if (jpeg.empty()) {
        std::cerr << "[review_client] JPEG 编码失败。" << std::endl;
        return 4;
    }

    // 4. 组装 ReviewRequest(与 GrpcLlmReviewer 完全一致)
    review::ReviewRequest req;
    req.set_image_jpeg(jpeg.data(), jpeg.size());
    req.set_label(label);
    req.set_confidence(confidence);
    req.set_frame_seq(1);
    req.set_prompt(prompt);
    req.set_roi_meta("roi=" + std::to_string(img.cols) + "x" + std::to_string(img.rows));

    review::ReviewResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline());
    addAuth(ctx, auth_token);       // [T41]

    std::cout << "[review_client] 向 " << addr << " 送审: label=" << label
              << " conf=" << confidence << " jpeg=" << jpeg.size() << "B"
              << " timeout=" << timeout_ms << "ms"
              << (auth_token.empty() ? " auth=off" : " auth=on") << " ..." << std::endl;

    const auto t0 = std::chrono::steady_clock::now();
    const grpc::Status st = stub->Review(&ctx, req, &resp);
    const auto client_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();

    if (!st.ok()) {
        std::cerr << "[review_client] RPC 失败: " << st.error_code()
                  << " - " << st.error_message() << " (client_ms=" << client_ms << ")" << std::endl;
        if (st.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) return 3;
        if (st.error_code() == grpc::StatusCode::UNAVAILABLE)        return 2;
        if (st.error_code() == grpc::StatusCode::UNAUTHENTICATED)    return 5;   // [T41]
        return 1;
    }
    if (!resp.ok()) {
        std::cerr << "[review_client] 服务端返回 ok=false(reason=" << resp.reason() << ")" << std::endl;
        return 1;
    }

    // 5. 打印结论
    std::cout << "--- 复核结论 ---" << std::endl;
    std::cout << "confirmed : " << (resp.confirmed() ? "true (应告警)" : "false (不告警)") << std::endl;
    std::cout << "label     : " << resp.label() << std::endl;
    std::cout << "confidence: " << resp.confidence() << std::endl;
    std::cout << "model     : " << (resp.model().empty() ? "(未上报)" : resp.model()) << std::endl;
    std::cout << "server_ms : " << resp.latency_ms() << " / client_ms: " << client_ms << std::endl;
    std::cout << "reason    : " << resp.reason() << std::endl;
    return 0;
}
