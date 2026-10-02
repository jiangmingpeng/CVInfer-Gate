#include "service/GrpcServerSetup.h"

#include <algorithm>
#include <string>

#include <grpc/grpc.h> // GRPC_ARG_* keepalive 常量

#include "service/DetectionServiceImpl.h"
#include "utils/Logger.h"

std::unique_ptr<grpc::Server> buildAndStartGrpcServer(const GrpcConfig& cfg,
                                                      DetectionServiceImpl& service,
                                                      std::string& out_address) {
    out_address = "0.0.0.0:" + std::to_string(cfg.port);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(out_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // 1) 收发消息大小上限 (gRPC 默认仅 4MB, 易被大图/大响应撑爆)
    const int mb = std::max(1, cfg.max_message_size_mb);
    const int max_bytes = mb * 1024 * 1024;
    builder.SetMaxReceiveMessageSize(max_bytes);
    builder.SetMaxSendMessageSize(max_bytes);

    // 2) 同步服务线程配额 (0 = 交给 gRPC 默认调度)
    if (cfg.worker_threads > 0) {
        grpc::ResourceQuota quota;
        quota.SetMaxThreads(cfg.worker_threads);
        builder.SetResourceQuota(quota);
    }

    // 3) HTTP/2 keepalive: 探测/回收半死连接
    if (cfg.keepalive_time_ms > 0) {
        const int kt = cfg.keepalive_time_ms;
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, kt);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, std::max(1000, kt / 2));
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, kt);
    }

    CVLOG_INFO << "gRPC 配置: max_msg=" << mb << "MB, worker_threads="
               << (cfg.worker_threads > 0 ? cfg.worker_threads : -1)
               << "(<=0 表示默认), keepalive=" << cfg.keepalive_time_ms
               << "ms, rpc_timeout=" << cfg.timeout_ms << "ms"
               << ", auth=" << (cfg.auth_token.empty() ? "off" : "on");

    return builder.BuildAndStart();
}
