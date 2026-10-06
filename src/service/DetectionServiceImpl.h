#pragma once
#include <cstdint>
#include <string>

#include <grpcpp/grpcpp.h>
#include "inference.grpc.pb.h"
#include "inference/IModel.h"
#include "service/AuthGuard.h"

// DetectionServiceImpl (推理引擎池; 改用 IDetector 抽象)
// 演进:
// - 原版: 持有裸 OpenVINOEngine&, 与流水线线程共享同一引擎 => 数据竞争。
// - 改为每次请求从 InferenceEnginePool 借一个独立引擎 (RAII 归还)。
// - 进一步收敛为依赖 IDetector 抽象。借引擎/推理/后处理均在模型内部
// (YoloDetector) 完成, 与流水线 worker 共享同一个 detector, 资源隔离
// 与复用能力与单引擎池等价。可直接注入级联 detector。
// - [鉴权] 构造时注入 token(空 = 不校验); Detect 入口第一行显式校验。
// - [自述] 新增 Health RPC(鉴权口径与 Detect 一致), 供容器/systemd/负载均衡探活。
class DetectionServiceImpl final : public inference::DetectionService::Service {
public:
    // auth_token 为空 => 不鉴权(完全等价改动前, 零破坏)
    // version/start_ms 供 Health 自述; 带默认值 => 旧调用点不改也能编译
    explicit DetectionServiceImpl(IDetector& detector, std::string auth_token = "",
                                  std::string version = "dev", std::int64_t start_ms = 0);

    // 实现具体的 RPC 方法
    grpc::Status Detect(grpc::ServerContext* context, 
                        const inference::DetectRequest* request,
                        inference::DetectResponse* response) override;

    // 健康检查 / 自述(不碰模型, 因此探针不会被推理阻塞)
    grpc::Status Health(grpc::ServerContext* context,
                        const inference::HealthRequest* request,
                        inference::HealthResponse* response) override;

private:
    IDetector& detector_;
    auth_guard::Guard auth_; // token 为空 => require() 恒 OK
    std::string version_;
    std::int64_t start_ms_ = 0; // 进程启动(epoch ms)
};