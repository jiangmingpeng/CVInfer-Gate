#include "service/AuthGuard.h"

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// AuthGuard 单测 (主服务 50051 鉴权)
// 与 Python 自测(vlm_review/test_auth_interceptor.py)**同用例集**, 保证
// 两个服务端的鉴权语义不飘:
// 正确 token / 完全不带 metadata / 6 种坏串(错 token、漏前缀、空 token、
// 尾空格、小写方案名、前缀攻击) / metadata 混排 / 非 ASCII / 只看第一个头。
// 另加 C++ 侧特有的两点:
// * 未启用(token 为空)必须**短路放行** => 老部署零破坏;
// * 定长比较的长度不等/长公共前缀边界。
//
// 分工(与复核鉴权同):
// * 本文件只测**纯逻辑**(不调 require()/verifyContext()), 所以 cv_unit_tests
// 继续保持"不链 gRPC、不需要任何服务"的性质 —— 这也是它能在 CI 里秒级跑完的原因;
// * "拒绝码必须是 UNAUTHENTICATED(不能退化成 UNKNOWN)"这类**拼装层**事实,
// 由 README里的真实端到端命令覆盖(`GRPC_AUTH_TOKEN=... ./grpc_client`
// 看服务端回的 code), 比单测里断言一个常量更有说服力。

namespace {

using auth_guard::Guard;
using auth_guard::Verdict;

const char* kTok = "s3cr3t";

std::vector<std::string> one(const std::string& v) { return std::vector<std::string>{v}; }

} // namespace

// 未启用 / 放行

TEST(AuthGuard, DisabledWhenTokenEmpty) {
    Guard g(""); // 空 token = 不鉴权(等价改动前行为)
    EXPECT_FALSE(g.enabled());
    EXPECT_EQ(g.verifyAll({}), Verdict::Disabled); // 不带 metadata 也放行
    EXPECT_EQ(g.verifyAll(one("Bearer whatever")), Verdict::Disabled);
}

TEST(AuthGuard, CorrectTokenAllows) {
    Guard g(kTok);
    EXPECT_TRUE(g.enabled());
    EXPECT_EQ(g.verifyAll(one("Bearer s3cr3t")), Verdict::Allow);
}

// 拒绝路径

TEST(AuthGuard, MissingMetadataIsDenied) {
    Guard g(kTok);
    EXPECT_EQ(g.verifyAll({}), Verdict::Missing);
}

TEST(AuthGuard, SixBadStringsAreAllDenied) {
    Guard g(kTok);
    const std::vector<std::string> bad = {
        "Bearer wrong", // 错 token
        "s3cr3t", // 漏了 Bearer 前缀
        "Bearer ", // 空 token
        "Bearer s3cr3t ", // 尾空格
        "bearer s3cr3t", // 方案名小写(客户端发的精确是 'Bearer ', 严格对齐)
        "Bearer s3cr3tx", // 前缀攻击: 多一个字符
    };
    for (const auto& v : bad) {
        EXPECT_EQ(g.verifyAll(one(v)), Verdict::Mismatch) << "应拒绝: " << v;
    }
}

TEST(AuthGuard, NonAsciiIsCleanlyDenied) {
    Guard g(kTok);
    // Python 版这里要吞 compare_digest 的 TypeError; C++ 是字节比较 => 天然不等。
    EXPECT_EQ(g.verifyAll(one("Bearer 中文")), Verdict::Mismatch);
}

TEST(AuthGuard, DenyVerdictsAreExactlyMissingAndMismatch) {
    // 把"哪些情况算拒绝"钉死在纯逻辑层(require() 只在这两种上返回
    // UNAUTHENTICATED; 状态码本身由 README 的端到端命令验证)。
    Guard g(kTok);
    const std::vector<Verdict> denied = {
        g.verifyAll({}), // 没带
        g.verifyAll(one("Bearer wrong")), // 带错
        g.verifyAll(one("Bearer 中文")), // 非 ASCII
    };
    for (auto v : denied) {
        EXPECT_NE(v, Verdict::Allow) << auth_guard::verdictName(v);
        EXPECT_NE(v, Verdict::Disabled) << auth_guard::verdictName(v);
    }
}

// metadata 选取语义

TEST(AuthGuard, PicksFirstAuthorizationOnly) {
    // 与 Python 的 `break` 一致: 只看第一个; 后面再"对"也不翻案(不挑能过的)。
    Guard g(kTok);
    EXPECT_EQ(g.verifyAll({"Bearer wrong", "Bearer s3cr3t"}), Verdict::Mismatch);
    EXPECT_EQ(g.verifyAll({"Bearer s3cr3t", "Bearer wrong"}), Verdict::Allow);
}

TEST(AuthGuard, PickAuthorizationIgnoresOtherKeys) {
    const std::vector<std::pair<std::string, std::string>> md = {
        {"user-agent", "grpc-c++/1.60"},
        {"authorization", "Bearer s3cr3t"},
        {"roi", "roi=240x240"},
    };
    const auto picked = Guard::pickAuthorization(md);
    ASSERT_EQ(picked.size(), 1u);
    EXPECT_EQ(picked.front(), "Bearer s3cr3t");
    EXPECT_EQ(Guard(kTok).verifyAll(picked), Verdict::Allow);
}

// 定长比较

TEST(AuthGuard, ConstantTimeEqualsSemantics) {
    EXPECT_TRUE(auth_guard::constantTimeEquals("abc", "abc"));
    EXPECT_TRUE(auth_guard::constantTimeEquals("", ""));
    EXPECT_FALSE(auth_guard::constantTimeEquals("abc", "abcd")); // 长度不等
    EXPECT_FALSE(auth_guard::constantTimeEquals("abc", "abd")); // 末字节不同
    EXPECT_FALSE(auth_guard::constantTimeEquals("", "Bearer tok"));
    // 长公共前缀: 语义上等同 compare_digest 的返回值(不因前 N-1 字节相同而漏判)
    EXPECT_FALSE(auth_guard::constantTimeEquals("Bearer 0123456789abcdef",
                                                "Bearer 0123456789abcdeX"));
}

// 启动日志

TEST(AuthGuard, DescribeTellsWhetherEnabled) {
    EXPECT_NE(Guard("").describe().find("关闭"), std::string::npos);
    EXPECT_NE(Guard(kTok).describe().find("开启"), std::string::npos);
}
