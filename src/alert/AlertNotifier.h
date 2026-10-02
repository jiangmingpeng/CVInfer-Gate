#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "utils/ConfigParser.h"
#include "utils/HttpClient.h"

// 告警推送 (webhook)
// 缺口: 此前告警**只写 MySQL**(alerts 表) —— 检测再准、去重再好, 没人会知道。
// 这是安防链路的最后一公里。
//
// 设计原则(与流水线其它组件的口径保持一致):
// * **绝不阻塞流水线**: push() 只做入队(有界, 满则丢最旧, 与帧队列同策略),
// 真正的 HTTP 在独立线程里做。
// * **失败不致命**: 重试 + 指数退避; 最终失败只记 WARN + 计数, 不影响推理。
// * **可测**: 传输层(Transport)可注入 => 单测用假 transport 验重试/丢弃/统计,
// 不依赖网络; 端到端再用真 HTTP(见 README 的 python mock).
// * **可观测**: stats() 暴露 pushed/sent/failed/dropped, 供 /metrics 抓取。
//
// 发送内容(下游集成契约 —— 改动即破坏兼容, 已在 test_alert_notifier.cpp 钉住):
// POST <url>   Content-Type: application/json
// { "source":"cvinfer-gate", "alert_type":"安全帽缺失", "description":"...",
// "frame_seq":123, "label":"person", "confidence":0.8700,
// "track_id":3, "ts_ms":1730000000000 }
// ts_ms 为 0 时由发送方补当前时间; 2xx = 送达, 其它(含 4xx/5xx)按失败重试。
//
// 边界(README 会写明): 只支持 http://(无 TLS)、不保证顺序、不落盘重发
// (进程被 kill 时队列里未发的告警会丢 —— 数据库里那条记录才是"账",
// webhook 是"通知", 通知丢失可由 DB 侧补偿)。
namespace alert {

struct Alert {
    std::string type; // 告警类型(如 "安全帽缺失")
    std::string description; // 人类可读描述(与落库字段一致)
    std::uint64_t frame_seq = 0; // 触发的帧号
    std::string label; // 目标标签(可空)
    float confidence = 0.0f; // 置信度(可空)
    int track_id = -1; // 目标 id(无 = -1)
    std::int64_t ts_ms = 0; // 触发时间(epoch ms; 0 = 发送时补)
};

// 配置直接复用 ConfigParser 的 AlertPushConfig(yaml 里就是 alert.push 段):
// 单一事实来源 —— 否则又多一份"必须手动同步"的字段表, 迟早对不上。
using Config = AlertPushConfig;

class AlertNotifier {
public:
    // 注入式传输层(单测用假实现; 默认走真实 HTTP)
    using Transport = std::function<http::Response(const http::Request&)>;

    AlertNotifier() = default;
    ~AlertNotifier();
    AlertNotifier(const AlertNotifier&) = delete;
    AlertNotifier& operator=(const AlertNotifier&) = delete;

    // 配置不合法(未启用/URL 非法)时返回 false, 调用方应降级为"只落库"
    bool init(const Config& cfg);
    bool init(const Config& cfg, Transport transport); // 测试用

    bool enabled() const { return enabled_; }

    // 非阻塞入队; 队列满时丢**最旧**(返回值: true=已入队, false=丢弃)
    bool push(const Alert& a);
    bool push(const std::string& type, const std::string& description);

    void stop(); // 幂等: 排空(有超时)后停线程

    struct Stats {
        std::uint64_t pushed = 0; // 入队成功
        std::uint64_t sent = 0; // HTTP 2xx
        std::uint64_t failed = 0; // 重试耗尽仍失败
        std::uint64_t dropped = 0; // 队列满被丢
        std::uint64_t retried = 0; // 重试次数
        std::int64_t last_ok_ms = 0;
        std::int64_t last_err_ms = 0;
        std::string last_error;
    };
    Stats stats() const;

    // 供单测直接验证(不涉及网络)
    static std::string jsonEscape(const std::string& s);
    static std::string toJson(const Alert& a);

private:
    void workerLoop();

    mutable std::mutex mtx_; // 保护 queue_ / stats_ / stopping_
    std::condition_variable cv_;
    std::deque<Alert> queue_;
    Stats stats_;
    bool stopping_ = false;

    Config cfg_;
    Transport transport_;
    bool enabled_ = false;
    std::thread worker_;
};

} // namespace alert
