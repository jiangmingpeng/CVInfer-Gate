// test_grpc_client (主服务 50051 的手动/联调客户端)
// 用法: ./build/grpc_client [addr] [timeout_ms]      (默认 127.0.0.1:50051 5000ms)
// 鉴权: 环境变量 GRPC_AUTH_TOKEN 非空 => 每个 RPC 都带
// authorization: Bearer <token>(与服务端 AuthGuard 严格对齐)。
// 退出码: 0=成功  5=UNAUTHENTICATED(与 review_client 的约定一致)
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>
#include <grpcpp/grpcpp.h>
#include <grpc/grpc.h> // GRPC_ARG_* keepalive 常量

#include "inference.grpc.pb.h"

namespace {

// token 走环境变量: 不进 shell 历史 / ps / 仓库(与 T41 的 REVIEW_AUTH_TOKEN 同纪律)
std::string authTokenFromEnv() {
    const char* t = std::getenv("GRPC_AUTH_TOKEN");
    return (t && *t) ? std::string(t) : std::string();
}

void addAuth(grpc::ClientContext& ctx, const std::string& token) {
    // 服务端比的是**整串** "Bearer " + token => 这里必须精确拼同一串
    if (!token.empty()) ctx.AddMetadata("authorization", "Bearer " + token);
}

} // namespace

int main(int argc, char** argv) {
    // 0. 参数: [server_addr] [timeout_ms]   (T8: 地址 / 超时可配)
    const std::string server_address = (argc > 1) ? argv[1] : "127.0.0.1:50051";
    const int timeout_ms = (argc > 2) ? std::atoi(argv[2]) : 5000;
    const int max_msg_mb = 16;
    // 鉴权 token(环境变量; 空 => 不带 metadata, 要求服务端也没开鉴权)
    const std::string auth_token = authTokenFromEnv();

    // 1. 连接服务端 (T8: 消息大小上限 + keepalive, 与服务端保持一致)
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(max_msg_mb * 1024 * 1024);
    args.SetMaxSendMessageSize(max_msg_mb * 1024 * 1024);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    auto channel = grpc::CreateCustomChannel(
        server_address, grpc::InsecureChannelCredentials(), args);
    auto stub = inference::DetectionService::NewStub(channel);

    // // 2. 读取一张本地图片（用之前生成的 output.jpg 测试即可）
    // cv::Mat img = cv::imread("output.jpg", cv::IMREAD_COLOR);
    // if (img.empty()) {
    // std::cerr << "无法读取 output.jpg，请确认图片路径！" << std::endl;
    // return -1;
    // }

    // // 3. 将图片编码为 JPEG 字节流
    // std::vector<uchar> img_buffer;
    // cv::imencode(".jpg", img, img_buffer);

    // 2. 读取视频的第一帧（代替读取 output.jpg）
    cv::VideoCapture cap("test.mp4");
    cv::Mat img;
    if (!cap.isOpened() || !cap.read(img)) {
        std::cerr << "无法读取 test.mp4，请确认视频路径！" << std::endl;
        return -1;
    }
    cap.release();

    // 3. 将图片编码为 JPEG 字节流
    std::vector<uchar> img_buffer;
    cv::imencode(".jpg", img, img_buffer);
    
    // 4. 构造请求
    inference::DetectRequest request;
    request.set_image_data(std::string(img_buffer.begin(), img_buffer.end()));

    inference::DetectResponse response;
    grpc::ClientContext context;
    addAuth(context, auth_token);
    // T8: 客户端 deadline, 超过 timeout_ms 直接返回 DEADLINE_EXCEEDED
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(timeout_ms));

    // 5. 发送请求
    std::cout << "向 " << server_address << " 发送图片, 大小: "
              << img_buffer.size() << " 字节, 超时: " << timeout_ms << " ms"
              << ", auth=" << (auth_token.empty() ? "off" : "on") << "..." << std::endl;
    grpc::Status status = stub->Detect(&context, request, &response);

    // 6. 处理响应
    if (status.ok()) {
        std::cout << "服务端响应: " << response.message() << std::endl;
        std::cout << "检测到目标数量: " << response.detections_size() << std::endl;
        for (int i = 0; i < response.detections_size(); ++i) {
            const auto& det = response.detections(i);
            std::cout << "  [" << i << "] " << det.label() 
                      << " 置信度: " << det.confidence() 
                      << " 坐标: (" << det.x1() << ", " << det.y1() << ") - (" 
                      << det.x2() << ", " << det.y2() << ")" << std::endl;
        }
    } else {
        std::cerr << "RPC 调用失败: " << status.error_code() 
                  << " - " << status.error_message() << std::endl;
        if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
            std::cerr << "（请求超时, 可增大第 2 个参数 timeout_ms, 或检查服务端负载）" << std::endl;
        }
        if (status.error_code() == grpc::StatusCode::UNAUTHENTICATED) {
            std::cerr << "（鉴权失败: 服务端开了鉴权, 而本客户端没带/带错 token。"
                      << "两端都要 export GRPC_AUTH_TOKEN=<同一个 token>）" << std::endl;
            return 5; // 5 = UNAUTHENTICATED(与 review_client 约定一致)
        }
    }

    return 0;
}