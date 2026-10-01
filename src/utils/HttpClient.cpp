#include "utils/HttpClient.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace http {

namespace {

// 带超时的 connect: 先非阻塞连, 再 select 等可写(这一步是"连不上"最常见的卡点)
bool connectWithTimeout(int fd, const sockaddr* addr, socklen_t addrlen, int timeout_ms) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, addr, addrlen);
    if (rc == 0) {
        ::fcntl(fd, F_SETFL, flags);
        return true;
    }
    if (errno != EINPROGRESS) return false;

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = ::select(fd + 1, nullptr, &wset, nullptr, &tv);
    if (rc <= 0) return false;   // 超时(0) 或出错(-1)

    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return false;

    ::fcntl(fd, F_SETFL, flags);
    return true;
}

bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

bool parseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    static const std::string scheme = "http://";
    if (url.rfind(scheme, 0) != 0) return false;   // 只支持 http://

    const std::string rest = url.substr(scheme.size());
    const std::size_t slash = rest.find('/');
    const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        try {
            port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        host = authority;
        port = 80;
    }
    if (host.empty() || port <= 0 || port > 65535) return false;
    return true;
}

Response request(const Request& req) {
    Response resp;

    std::string host, path;
    int port = 80;
    if (!parseUrl(req.url, host, port, path)) {
        resp.error = "URL 非法(仅支持 http://host[:port]/path): " + req.url;
        return resp;
    }

    // 1. 建连(DNS + TCP, 均受 timeout_ms 约束)
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        resp.error = "DNS 解析失败: " + host;
        if (res) ::freeaddrinfo(res);
        return resp;
    }

    int fd = -1;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connectWithTimeout(fd, p->ai_addr, static_cast<socklen_t>(p->ai_addrlen),
                               req.timeout_ms)) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        resp.error = "连接失败: " + host + ":" + port_str;
        return resp;
    }

    // 2. 收发超时(避免服务端"收下不回"把推送线程永久挂住)
    timeval tv{};
    tv.tv_sec = req.timeout_ms / 1000;
    tv.tv_usec = (req.timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // 3. 拼请求(HTTP/1.1 + Connection: close => 之后可以一路 recv 到 EOF)
    std::string out = req.method + " " + path + " HTTP/1.1\r\n";
    out += "Host: " + host + ":" + port_str + "\r\n";
    out += "User-Agent: cvinfer-gate/1.0\r\n";
    out += "Accept: */*\r\n";
    out += "Connection: close\r\n";
    bool has_content_type = false;
    for (const auto& kv : req.headers) {
        if (kv.first == "Content-Type") has_content_type = true;
        out += kv.first + ": " + kv.second + "\r\n";
    }
    if (!req.body.empty() && !has_content_type) out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n\r\n";
    out += req.body;

    if (!sendAll(fd, out)) {
        resp.error = std::string("发送失败: ") + std::strerror(errno);
        ::close(fd);
        return resp;
    }

    // 4. 读响应直到 EOF / 上限(有 Connection: close, 正常服务端会主动关)
    std::string raw;
    char buf[4096];
    while (raw.size() < req.max_body) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            raw.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            resp.error = "读响应超时";
            break;
        }
        break;   // n == 0 => 对端关闭(正常结束)
    }
    ::close(fd);

    if (raw.empty()) {
        if (resp.error.empty()) resp.error = "空响应";
        return resp;
    }

    // 5. 解析状态行与 body
    const std::size_t lf = raw.find('\n');
    if (lf == std::string::npos) {
        resp.error = "响应无状态行";
        return resp;
    }
    const std::string status_line = raw.substr(0, lf);
    int status = 0;
    if (std::sscanf(status_line.c_str(), "HTTP/%*d.%*d %d", &status) != 1) {
        resp.error = "状态行无法解析: " + status_line;
        return resp;
    }
    resp.status = status;
    resp.ok = true;

    const std::size_t sep = raw.find("\r\n\r\n");
    if (sep != std::string::npos) resp.body = raw.substr(sep + 4);
    return resp;
}

}  // namespace http
