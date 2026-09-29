#pragma once
#include <grpcpp/grpcpp.h>
#include "inference.grpc.pb.h"
#include "inference/IModel.h"

// ============================================================
// DetectionServiceImpl (T7: 推理引擎池; T15: 改用 IDetector 抽象)
// ------------------------------------------------------------
// 演进:
//   - 原版: 持有裸 OpenVINOEngine&, 与流水线线程共享同一引擎 => 数据竞争。
//   - T7 : 改为每次请求从 InferenceEnginePool 借一个独立引擎 (RAII 归还)。
//   - T15: 进一步收敛为依赖 IDetector 抽象。借引擎/推理/后处理均在模型内部
//          (YoloDetector) 完成, 与流水线 worker 共享同一个 detector, 资源隔离
//          与复用能力与 T7 等价。T16+ 可直接注入级联 detector。
// ============================================================
class DetectionServiceImpl final : public inference::DetectionService::Service {
public:
    explicit DetectionServiceImpl(IDetector& detector);

    // 实现具体的 RPC 方法
    grpc::Status Detect(grpc::ServerContext* context, 
                        const inference::DetectRequest* request,
                        inference::DetectResponse* response) override;

private:
    IDetector& detector_;
};