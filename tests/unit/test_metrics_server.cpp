#include "service/MetricsServer.h"

#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "utils/HttpClient.h"

// /metrics 端点单测 (T43)
// 分两层:
// 1) 纯函数(请求行解析 / 响应拼装) —— 覆盖畸形请求, 不碰 socket
// 2) 真 socket 集成 —— 起服务, 用自研 HttpClient 真的抓一次(200/404/健康)
// 端口取 19100 + pid%200: 避开常用端口; 万一被占用则 GTEST_SKIP(不让 CI 因环境抖动红)。

namespace {

using metrics::HttpServer;

TEST(MetricsServer, ParsesRequestTarget) {
    EXPECT_EQ(HttpServer::parseRequestTarget("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n"),
              "/metrics");
    EXPECT_EQ(HttpServer::parseRequestTarget("HEAD /healthz HTTP/1.0\r\n\r\n"), "/healthz");
    EXPECT_EQ(HttpServer::parseRequestTarget("GET /metrics?format=text HTTP/1.1\r\n"), "/metrics");
}

TEST(MetricsServer, RejectsMalformedOrNonReadableRequests) {
    EXPECT_EQ(HttpServer::parseRequestTarget("POST /metrics HTTP/1.1\r\n"), "");
    EXPECT_EQ(HttpServer::parseRequestTarget("GET http://x/metrics HTTP/1.1\r\n"), "");
    EXPECT_EQ(HttpServer::parseRequestTarget("GARBAGE"), "");
    EXPECT_EQ(HttpServer::parseRequestTarget(""), "");
}

TEST(MetricsServer, BuildsWellFormedResponse) {
    const std::string r = HttpServer::buildResponse(200, "text/plain", "hi");
    EXPECT_EQ(r.rfind("HTTP/1.1 200 OK", 0), 0u);
    EXPECT_NE(r.find("Content-Length: 2\r\n"), std::string::npos);
    EXPECT_NE(r.find("\r\n\r\nhi"), std::string::npos);
    EXPECT_EQ(HttpServer::statusText(404), "Not Found");
    EXPECT_EQ(HttpServer::statusText(500), "Internal Server Error");
}

TEST(MetricsServer, ServesMetricsHealthzAnd404OverRealSocket) {
    const int port = 19100 + (static_cast<int>(::getpid()) % 200);
    HttpServer srv;
    HttpServer::Config cfg;
    cfg.enabled = true;
    cfg.bind = "127.0.0.1"; // 单测只在回环上跑, 不对外
    cfg.port = port;

    if (!srv.start(cfg, [] { return std::string("# TYPE demo gauge\ndemo 1\n"); })) {
        GTEST_SKIP() << "端口 " << port << " 被占用, 跳过 socket 集成测试";
    }
    EXPECT_TRUE(srv.running());

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    http::Request req;
    req.method = "GET";
    req.timeout_ms = 2000;

    req.url = base + "/metrics";
    http::Response m = http::request(req);
    EXPECT_TRUE(m.ok);
    EXPECT_EQ(m.status, 200);
    EXPECT_NE(m.body.find("demo 1"), std::string::npos);

    req.url = base + "/healthz";
    http::Response h = http::request(req);
    EXPECT_TRUE(h.ok);
    EXPECT_EQ(h.status, 200);
    EXPECT_NE(h.body.find("ok"), std::string::npos);

    req.url = base + "/nope";
    http::Response n = http::request(req);
    EXPECT_TRUE(n.ok);
    EXPECT_EQ(n.status, 404);

    srv.stop();
    srv.stop(); // 幂等
    EXPECT_FALSE(srv.running());
}

TEST(MetricsServer, DisabledConfigDoesNotListen) {
    HttpServer srv;
    HttpServer::Config cfg;
    cfg.enabled = false;
    cfg.port = 19199;
    EXPECT_FALSE(srv.start(cfg, [] { return std::string(); }));
    EXPECT_FALSE(srv.running());
}

} // namespace
