#include <chrono>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "alert/AlertNotifier.h"

// ============================================================
// 告警推送单测 (T43) —— 传输层注入, 不碰网络
// ------------------------------------------------------------
// 重点验证**异常路径** —— 推送是运维链路的最后一段, 它坏掉时往往是静默的:
//   * JSON 转义(描述里带引号/换行/中文都不能把 body 拼坏)
//   * 失败重试 + 退避; 重试耗尽后 failed / last_error 可观测
//   * HTTP 4xx/5xx 不算成功(不能把"服务端拒收"当送达)
//   * 队列满 => 丢最旧(不阻塞流水线, 不无界增长)
//   * 停机排空(有预算): 已产生的告警尽量送出去
// ============================================================

namespace {

using alert::Alert;
using alert::AlertNotifier;
using alert::Config;

Alert sample() {
    Alert a;
    a.type = "安全帽缺失";
    a.description = "含\"引号\"与\n换行";
    a.frame_seq = 7;
    a.label = "person";
    a.confidence = 0.87f;
    a.track_id = 3;
    a.ts_ms = 1234567890;
    return a;
}

Config baseConfig() {
    Config c;
    c.enabled = true;
    c.url = "http://127.0.0.1:8899/alert";
    c.timeout_ms = 500;
    c.retry_backoff_ms = 5;   // 单测里退避要短
    return c;
}

}  // namespace

TEST(AlertNotifier, JsonEscapingAndPayloadFields) {
    const std::string j = AlertNotifier::toJson(sample());
    // 有效载荷契约(下游按这个解析, 改动等于破坏兼容):
    //   source / alert_type / description / frame_seq / label / confidence /
    //   track_id / ts_ms
    EXPECT_NE(j.find("\"alert_type\":\"安全帽缺失\""), std::string::npos);
    EXPECT_NE(j.find("\\\"引号\\\""), std::string::npos);   // 引号被转义(JSON 里是 \"引号\")
    EXPECT_NE(j.find("\\n"), std::string::npos);             // 换行转义
    EXPECT_NE(j.find("\"frame_seq\":7"), std::string::npos);
    EXPECT_NE(j.find("\"label\":\"person\""), std::string::npos);
    EXPECT_NE(j.find("\"confidence\":0.8700"), std::string::npos);
    EXPECT_NE(j.find("\"track_id\":3"), std::string::npos);
    EXPECT_NE(j.find("\"source\":\"cvinfer-gate\""), std::string::npos);
    EXPECT_EQ(AlertNotifier::jsonEscape("a\tb"), "a\\tb");
    EXPECT_EQ(AlertNotifier::jsonEscape(std::string(1, '\x01')), "\\u0001");
}

TEST(AlertNotifier, DisabledConfigRejectsInitAndPush) {
    AlertNotifier n;
    Config cfg = baseConfig();
    cfg.enabled = false;
    auto transport = [](const http::Request&) { return http::Response{}; };

    EXPECT_FALSE(n.init(cfg, transport));
    EXPECT_FALSE(n.enabled());
    EXPECT_FALSE(n.push(sample()));
}

TEST(AlertNotifier, DeliversOnFirstAttempt) {
    int calls = 0;
    std::string seen_body;
    auto ok = [&](const http::Request& r) {
        ++calls;
        seen_body = r.body;
        http::Response resp;
        resp.ok = true;
        resp.status = 200;
        return resp;
    };

    AlertNotifier n;
    ASSERT_TRUE(n.init(baseConfig(), ok));
    EXPECT_TRUE(n.push(sample()));
    n.stop();

    const auto st = n.stats();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(st.pushed, 1u);
    EXPECT_EQ(st.sent, 1u);
    EXPECT_EQ(st.failed, 0u);
    EXPECT_NE(seen_body.find("安全帽缺失"), std::string::npos);
}

TEST(AlertNotifier, RetriesWithBackoffUntilSuccess) {
    int calls = 0;
    auto flaky = [&](const http::Request&) {
        http::Response resp;
        if (++calls < 3) {
            resp.ok = false;   // 前两次连接失败
            resp.error = "connection refused";
            return resp;
        }
        resp.ok = true;
        resp.status = 201;
        return resp;
    };

    Config cfg = baseConfig();
    cfg.max_retries = 3;
    AlertNotifier n;
    ASSERT_TRUE(n.init(cfg, flaky));
    n.push(sample());
    n.stop();

    const auto st = n.stats();
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(st.sent, 1u);
    EXPECT_EQ(st.retried, 2u);
    EXPECT_EQ(st.failed, 0u);
}

TEST(AlertNotifier, HttpErrorStatusCountsAsFailure) {
    auto http500 = [](const http::Request&) {
        http::Response resp;
        resp.ok = true;
        resp.status = 500;   // 有响应 ≠ 送达
        return resp;
    };

    Config cfg = baseConfig();
    cfg.max_retries = 0;
    AlertNotifier n;
    ASSERT_TRUE(n.init(cfg, http500));
    n.push(sample());
    n.stop();

    const auto st = n.stats();
    EXPECT_EQ(st.failed, 1u);
    EXPECT_NE(st.last_error.find("500"), std::string::npos);
}

TEST(AlertNotifier, DropsOldestWhenQueueFullInsteadOfBlocking) {
    auto slow = [](const http::Request&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        http::Response resp;
        resp.ok = true;
        resp.status = 200;
        return resp;
    };

    Config cfg = baseConfig();
    cfg.max_queue = 2;
    cfg.max_retries = 0;
    AlertNotifier n;
    ASSERT_TRUE(n.init(cfg, slow));

    for (int i = 0; i < 10; ++i) EXPECT_TRUE(n.push(sample()));
    n.stop();

    const auto st = n.stats();
    EXPECT_EQ(st.pushed, 10u);        // 入队都成功(被挤掉的是更旧的)
    EXPECT_GE(st.dropped, 1u);        // 队列满时确实丢了最旧
    EXPECT_EQ(st.sent + st.failed + st.dropped, 10u);   // 账要平: 没有凭空多/少的告警
}

TEST(AlertNotifier, StopIsIdempotentAndDrainsPending) {
    int calls = 0;
    auto ok = [&](const http::Request&) {
        ++calls;
        http::Response resp;
        resp.ok = true;
        resp.status = 200;
        return resp;
    };

    AlertNotifier n;
    ASSERT_TRUE(n.init(baseConfig(), ok));
    for (int i = 0; i < 5; ++i) n.push(sample());
    n.stop();
    n.stop();   // 幂等

    EXPECT_EQ(n.stats().sent, 5u);   // 停机前已入队的都送出去了
    EXPECT_EQ(calls, 5);
}

TEST(AlertNotifier, InvalidUrlIsRejected) {
    AlertNotifier n;
    Config cfg = baseConfig();
    cfg.url = "https://example.com/alert";   // 只支持明文 http://
    auto transport = [](const http::Request&) { return http::Response{}; };
    EXPECT_FALSE(n.init(cfg, transport));
}
