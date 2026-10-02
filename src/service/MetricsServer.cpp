#include "service/MetricsServer.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "utils/Logger.h"

namespace metrics {

namespace {
constexpr std::size_t kMaxRequestBytes = 4096;
constexpr int kReadTimeoutMs = 1000; // 单个客户端最多给它 1s 把请求发完
} // namespace

HttpServer::~HttpServer() {
    stop();
}

std::string HttpServer::statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

std::string HttpServer::buildResponse(int status, const std::string& content_type,
                                      const std::string& body) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + statusText(status) + "\r\n";
    out += "Content-Type: " + content_type + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

std::string HttpServer::parseRequestTarget(const std::string& raw_request) {
    // 只看第一行: METHOD SP TARGET SP VERSION
    const std::size_t lf = raw_request.find('\n');
    if (lf == std::string::npos) return {};
    std::string line = raw_request.substr(0, lf);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    const std::size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return {};
    const std::size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return {};
    const std::string method = line.substr(0, sp1);
    if (method != "GET" && method != "HEAD") return {}; // 只读接口

    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/') return {};
    const std::size_t q = target.find('?'); // 忽略 query(如 ?format=text)
    if (q != std::string::npos) target = target.substr(0, q);
    return target;
}

bool HttpServer::start(const Config& cfg, std::function<std::string()> metrics_body) {
    if (running_.load()) return true;
    if (!cfg.enabled) return false;
    if (cfg.port <= 0 || cfg.port > 65535) {
        CVLOG_ERROR << "[指标] 端口非法: " << cfg.port;
        return false;
    }
    metrics_body_ = std::move(metrics_body);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        CVLOG_ERROR << "[指标] socket 创建失败: " << std::strerror(errno);
        return false;
    }
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); // 重启后端口可立即复用

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(cfg.port));
    if (cfg.bind.empty() || cfg.bind == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, cfg.bind.c_str(), &addr.sin_addr) != 1) {
        CVLOG_ERROR << "[指标] bind 地址非法: " << cfg.bind;
        closeListen();
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CVLOG_ERROR << "[指标] bind 失败(" << cfg.bind << ":" << cfg.port
                    << "): " << std::strerror(errno);
        closeListen();
        return false;
    }
    if (::listen(listen_fd_, 8) != 0) {
        CVLOG_ERROR << "[指标] listen 失败: " << std::strerror(errno);
        closeListen();
        return false;
    }

    // 非阻塞 + select 轮询: 既能及时响应 stop(), 又不用 self-pipe/自连接的把戏
    const int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    port_ = cfg.port;
    running_.store(true);
    thread_ = std::thread([this] { serveLoop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join(); // serveLoop 最多 100ms 内看到标志退出
    closeListen();
}

void HttpServer::closeListen() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void HttpServer::serveLoop() {
    CVLOG_INFO << "[指标] /metrics 端点已启动, 端口 " << port_
               << " (无鉴权, 仅建议在内网/受信网络暴露)";
    while (running_.load()) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listen_fd_, &rset);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000; // 100ms: 让 stop() 及时生效
        const int rc = ::select(listen_fd_ + 1, &rset, nullptr, nullptr, &tv);
        if (rc <= 0) continue; // 超时(正常) 或被打断

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (client < 0) continue;
        handleClient(client);
        ::close(client);
    }
}

void HttpServer::handleClient(int fd) {
    timeval tv{};
    tv.tv_sec = kReadTimeoutMs / 1000;
    tv.tv_usec = (kReadTimeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string raw;
    char buf[1024];
    while (raw.size() < kMaxRequestBytes) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            if (raw.find("\r\n\r\n") != std::string::npos) break; // 请求头收全即可(GET 无 body)
            continue;
        }
        break;
    }

    const std::string target = parseRequestTarget(raw);
    std::string response;
    if (target.empty()) {
        response = buildResponse(400, "text/plain; charset=utf-8", "bad request\n");
    } else if (target == "/metrics") {
        std::string body;
        try {
            body = metrics_body_ ? metrics_body_() : std::string();
        } catch (...) {
            response = buildResponse(500, "text/plain; charset=utf-8", "render failed\n");
            ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
            return;
        }
        // Prometheus 文本格式的官方 content-type
        response = buildResponse(200, "text/plain; version=0.0.4; charset=utf-8", body);
    } else if (target == "/healthz") {
        response = buildResponse(200, "text/plain; charset=utf-8", "ok\n");
    } else {
        response = buildResponse(404, "text/plain; charset=utf-8",
                                 "not found (可用: /metrics, /healthz)\n");
    }
    ::send(fd, response.data(), response.size(), MSG_NOSIGNAL);
}

} // namespace metrics
