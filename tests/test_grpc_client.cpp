#include <iostream>
#include <fstream>
#include <opencv2/opencv.hpp>
#include <grpcpp/grpcpp.h>
#include "inference.grpc.pb.h"

int main(int argc, char** argv) {
    // 1. 连接服务端
    std::string server_address("106.15.88.152:50051");
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    auto stub = inference::DetectionService::NewStub(channel);

    // // 2. 读取一张本地图片（用之前生成的 output.jpg 测试即可）
    // cv::Mat img = cv::imread("output.jpg", cv::IMREAD_COLOR);
    // if (img.empty()) {
    //     std::cerr << "无法读取 output.jpg，请确认图片路径！" << std::endl;
    //     return -1;
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

    // 5. 发送请求
    std::cout << "向服务端发送图片，大小: " << img_buffer.size() << " 字节..." << std::endl;
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
    }

    return 0;
}