#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "utils/ConfigParser.h"

class DetectionServiceImpl;

// GrpcServerSetup (T8: gRPC 超时 / 消息大小 / keepalive / 线程数)
// 把 GrpcConfig 中此前"只解析未消费"的字段真正落地到 ServerBuilder:
// - max_message_size_mb -> 收发消息上限 (默认 4MB, 大图/大响应会超限)
// - worker_threads      -> 同步服务线程配额 (0 = gRPC 默认)
// - keepalive_time_ms   -> HTTP/2 keepalive 相关 channel argument
// - timeout_ms          -> 引擎借用超时已下沉到模型 (ModelConfig::acquire_timeout_ms);
// grpc.timeout_ms 现由客户端 deadline 使用 (见 tests/test_grpc_client.cpp)
//
// 成功返回已启动的 grpc::Server; 失败返回 nullptr。
// 无论成功与否, 都会把监听地址回写到 out_address。
std::unique_ptr<grpc::Server> buildAndStartGrpcServer(const GrpcConfig& cfg,
                                                      DetectionServiceImpl& service,
                                                      std::string& out_address);
