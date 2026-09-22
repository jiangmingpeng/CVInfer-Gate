#pragma once
#include <grpcpp/grpcpp.h>
#include "inference.grpc.pb.h"
#include "inference/OpenVINOEngine.h"
#include "inference/YoloPostProcessor.h"

class DetectionServiceImpl final : public inference::DetectionService::Service {
public:
    DetectionServiceImpl(OpenVINOEngine& engine, 
                         YoloPostProcessor& post_processor,
                         const std::vector<std::string>& labels);
    
    // 实现具体的 RPC 方法
    grpc::Status Detect(grpc::ServerContext* context, 
                        const inference::DetectRequest* request,
                        inference::DetectResponse* response) override;

private:
    OpenVINOEngine& engine_;
    YoloPostProcessor& post_processor_;
    std::vector<std::string> labels_;
};