#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

// AuthGuard (T42: 主服务 50051 鉴权)
// 起因: OVERVIEW §5 把"主服务 50051 无鉴权"列为🔴 —— 任何能连到端口的人都能
// 白嫖推理算力 / 探测服务是否在线; T41 只给复核服务(50052)补了鉴权。
//
// 语义与 T41(vlm_review/server.py::_AuthInterceptor)**逐条对齐**, 不另发明一套:
// * token 为空 => 不启用鉴权(完全等价于改动前, 零破坏);
// * 比对的是**整串** `"Bearer " + token` —— 大小写敏感, 也没有尾空格容错。
// 这是**故意**的: C++ 客户端发的是精确的 `Bearer <token>`, 服务端就做严格
// 相等, 两边只有一种"正确", 不存在"看起来像但能绕过"的模糊地带:
// "bearer s3cr3t"   (方案名小写)  => 拒绝(不做 RFC7235 式宽松解析)
// "Bearer s3cr3t "  (尾空格)      => 拒绝
// "Bearer s3cr3tx"  (前缀攻击)    => 拒绝(比整串, 不是比前缀)
// "s3cr3t"          (漏前缀)      => 拒绝
// * 取 metadata 里**第一个** authorization(与 Python 版 `break` 一致):
// 多个同名头时**不挑一个能过的** => 不给降级通道;
// * 定长比较(逐字节累加差值, 长度不同也把短的走完) => 内容比较不短路,
// 不靠首个不同字节提前返回;
// * 拒绝码用 UNAUTHENTICATED(而非 PERMISSION_DENIED): 与 T41 及 HTTP 语义
// 一致, 客户端能区分"没带/带错凭证" vs "有凭证但没权限"。
//
// 与 T41 的一处**结构性差异**(需要说明):
// Python 的拦截器必须返回"与原型同类型"的 handler, 否则 grpc 会把状态码
// 降级成 UNKNOWN。C++ 同步服务是每个方法自己 `return Status`, 所以这里把
// 校验做成"方法入口显式一行" —— 结构上不可能出现 handler 类型不匹配。
// 代价: 新增 RPC 要手动加这一行(见 DetectionServiceImpl::Detect 开头)。
// 这条作为**已知边界**写在 README 里, 而不是假装不存在。
//
// 诚实边界: 这是**明文共享密钥**(不启用 TLS) —— 只解决"谁都能调",
// 不解决窃听/重放。内网/回环够用, 公网必须 TLS 或反向代理(与 T41 同)。

namespace auth_guard {

// 判定结果(纯逻辑, 不依赖 gRPC => 可单测)
enum class Verdict {
    Allow, // 放行
    Disabled, // 未启用(token 为空) => 放行
    Missing, // 完全没有 authorization 头
    Mismatch, // 有头, 但整串不等(含小写方案名/尾空格/前缀攻击/非 ASCII)
};

inline const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::Allow:    return "allow";
        case Verdict::Disabled: return "disabled";
        case Verdict::Missing:  return "missing";
        case Verdict::Mismatch: return "mismatch";
    }
    return "?";
}

// 定长比较: 语义对齐 hmac.compare_digest —— 长度不同也把较短者走完,
// 不因首个不同字节提前返回。注: 长度本身仍会泄漏(本地共享密钥场景的
// 同一取舍), 关键是**内容**比较不短路。
inline bool constantTimeEquals(std::string_view a, std::string_view b) {
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

struct Guard {
    std::string token; // 空 = 不鉴权(默认; 与 T41 的 cfg.auth_token 同语义)

    Guard() = default;
    explicit Guard(std::string t) : token(std::move(t)) {}

    bool enabled() const { return !token.empty(); }

    // 纯逻辑入口: 传入 metadata 里**按出现顺序**的所有 authorization 值。
    // 单测只测这里 => 不需要构造真的 grpc::ServerContext。
    Verdict verifyAll(const std::vector<std::string>& auth_values) const {
        if (!enabled()) return Verdict::Disabled;
        if (auth_values.empty()) return Verdict::Missing;
        const std::string expected = "Bearer " + token;
        // 只看第一个(与 Python 的 break 一致): 后面再"正确"也不翻案。
        return constantTimeEquals(auth_values.front(), expected) ? Verdict::Allow
                                                                 : Verdict::Mismatch;
    }

    // 从 (key,value) 序列里挑出 authorization 的值(保持出现顺序) —— 与 Python 的
    // `for key, value in invocation_metadata: if key == "authorization": ... break`
    // 结构等价, 且把"挑 metadata"这一步也变成可单测的纯逻辑。
    // 注: gRPC 会把 metadata 的 key 规范化为小写, 所以按小写 key 比较。
    static std::vector<std::string> pickAuthorization(
        const std::vector<std::pair<std::string, std::string>>& md) {
        std::vector<std::string> out;
        for (const auto& kv : md) {
            if (kv.first == "authorization") out.push_back(kv.second);
        }
        return out;
    }

    // 适配层: 把 grpc 的 multimap metadata 转成序列, 交给纯逻辑判定。
    Verdict verifyContext(const grpc::ServerContext& ctx) const {
        if (!enabled()) return Verdict::Disabled;
        const auto& md = ctx.client_metadata();
        std::vector<std::pair<std::string, std::string>> kv;
        kv.reserve(md.size());
        for (const auto& entry : md) {
            kv.emplace_back(std::string(entry.first.data(), entry.first.size()),
                            std::string(entry.second.data(), entry.second.size()));
        }
        return verifyAll(pickAuthorization(kv));
    }

    // 每个 RPC 的第一行:
    // if (auto st = auth_.require(*context); !st.ok()) return st;
    grpc::Status require(const grpc::ServerContext& ctx) const {
        const Verdict v = verifyContext(ctx);
        if (v == Verdict::Allow || v == Verdict::Disabled) return grpc::Status::OK;
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "缺少或错误的 authorization metadata (应为: Bearer <token>)");
    }

    // 启动时打一行: 把"以为开了其实没开"变成显式输出(与 T41 同一条纪律)。
    std::string describe() const {
        return enabled() ? "开启(每个 RPC 都校验 authorization: Bearer <token>)"
                         : "关闭(token 为空 => 不校验, 等价改动前行为)";
    }
};

} // namespace auth_guard
