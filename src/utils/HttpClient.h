#pragma once

#include <map>
#include <string>

// 极简 HTTP/1.1 客户端 (只为告警 webhook 用)
// 为什么不引第三方: 本项目对"零新依赖"很敏感(交叉编译/离线环境), 而 webhook
// 只需要 `POST JSON` 这一个动作。代价是**只支持够用的子集**:
// * 只支持 http:// (明文; HTTPS 请用内网转发或加 nginx 侧车)
// * 不支持 chunked / keep-alive / 重定向 / 代理
// * 响应体读取上限 max_body(默认 64KB), 超长截断(有响应就够诊断了)
// 这些边界在 README 里写明, 避免被当成"通用 HTTP 客户端"误用。
namespace http {

struct Request {
    std::string method = "POST";
    std::string url; // http://host[:port]/path
    std::map<std::string, std::string> headers; // 额外请求头
    std::string body;
    int timeout_ms = 3000; // 连接 + 收发总预算(每步各自计时)
    std::size_t max_body = 64 * 1024;
};

struct Response {
    bool ok = false; // 是否"拿到了响应"(≠ 状态码 2xx)
    int status = 0; // HTTP 状态码
    std::string body; // 响应体(可能被截断)
    std::string error; // ok=false 时的原因(连接失败/超时/格式错误)
};

// 解析 http://host[:port][/path] => host / port / path(缺省端口 80, 缺省 path "/")
bool parseUrl(const std::string& url, std::string& host, int& port, std::string& path);

// 同步请求(阻塞直到拿到响应或超时)
Response request(const Request& req);

} // namespace http
