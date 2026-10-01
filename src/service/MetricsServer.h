#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>

// ============================================================
// [T43] 指标端点 (极简 HTTP, 只为 /metrics 与 /healthz)
// ------------------------------------------------------------
// 为什么自己写而不是拉一个 HTTP 库: 与 HttpClient 同理 —— 需要的能力只有一个
//   "GET /metrics 返回文本", 引 web 框架的代价(依赖/构建/攻击面)远大于收益。
//
// 暴露的路径:
//   GET /metrics  -> Prometheus 文本格式(抓取用)
//   GET /healthz  -> "ok" (存活探针; 进程级, 不碰模型/数据库)
//   其它          -> 404
//
// ⚠️ 安全口径: /metrics 与 /healthz 都是**无鉴权**的(监控系统不方便带 token),
//   且会泄露帧率/队列/告警量等运行信息 => 只在**受信网络**暴露:
//     * 容器内 bind 0.0.0.0 可以, 但 compose 默认**不**把端口发布到宿主机
//       (用 expose 而不是 ports), 让 Prometheus 走容器网络抓取;
//     * 裸机部署时把 metrics.bind 设成 127.0.0.1(或防火墙只放行抓取端)。
// ============================================================
namespace metrics {

class HttpServer {
public:
    struct Config {
        bool enabled = false;
        std::string bind = "0.0.0.0";   // 建议生产改 127.0.0.1 或只放行抓取端
        int port = 9100;
    };

    HttpServer() = default;
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // metrics_body: 抓取时调用(内部走 Registry::render())
    bool start(const Config& cfg, std::function<std::string()> metrics_body);
    void stop();                       // 幂等
    bool running() const { return running_.load(); }
    int port() const { return port_; }

    // ---- 纯函数(便于单测, 不涉及 socket) ----
    // "GET /metrics HTTP/1.1\r\n..." -> "/metrics"; 非法/非 HTTP 返回空串
    static std::string parseRequestTarget(const std::string& raw_request);
    static std::string statusText(int status);
    static std::string buildResponse(int status, const std::string& content_type,
                                     const std::string& body);

private:
    void serveLoop();
    void handleClient(int fd);
    void closeListen();

    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::function<std::string()> metrics_body_;
};

}  // namespace metrics
