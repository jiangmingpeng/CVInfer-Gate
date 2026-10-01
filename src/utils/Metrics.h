#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// ============================================================
// [T43] 指标注册表 (Prometheus 文本格式)
// ------------------------------------------------------------
// 目的: 让进程能回答"现在每秒多少帧 / 丢了多少 / 告警推出去几条 / gRPC 失败几次",
//   而不是只能翻日志。输出走 Prometheus 文本格式, 抓取端零成本。
//
// 两类指标, **有意分开**:
//   * **推** (inc / setGauge): 热路径上的离散事件 —— 一次 RPC、一条告警、一次推送失败。
//     首次建指标之后只剩 map 查找 + 原子自增。
//   * **拉** (addCollector): 已经有 Stats 的地方(流水线 / DBWriter / 复核 / 去重 / 跟踪)。
//     这些统计本来就在维护自己的原子量, 再手动推一遍 = 双份维护 + 迟早对不上;
//     故在**抓取时**现算一次。
//
// ⚠️ 约定: **帧级路径不要 inc**(每帧一次 map 查找是白送的锁竞争);
//   帧级数据一律走"拉"(流水线 Stats 本身就是原子量)。
//
// 只依赖标准库 => 无新依赖, 可单测(见 tests/unit/test_metrics.cpp)。
// ============================================================
namespace metrics {

// 指标类型(渲染 HELP/TYPE 用)
enum class Type { Counter, Gauge };

class Registry {
public:
    static Registry& instance();

    // ---- 声明: 建议启动时统一声明一次, 以便带上 HELP 文本(未声明也会自动建) ----
    void declare(const std::string& name, Type type, const std::string& help,
                 const std::string& labels = "");

    // ---- 推: 计数(单调递增) ----
    // labels 传**已渲染**的片段, 如: "method=\"Detect\",code=\"OK\""
    void inc(const std::string& name, std::uint64_t n = 1);
    void inc(const std::string& name, const std::string& labels, std::uint64_t n = 1);

    // ---- 推: 设值 ----
    void setGauge(const std::string& name, double value);
    void setGauge(const std::string& name, const std::string& labels, double value);

    // ---- 拉: 抓取时求值 ----
    void addCollector(const std::string& name, Type type, const std::string& help,
                      std::function<double()> fn, const std::string& labels = "");

    // Prometheus 文本格式: 每个指标一组 # HELP / # TYPE, 指标名排序 => 输出稳定
    std::string render() const;

    // ---- 仅供测试 ----
    void clear();
    std::size_t size() const;

private:
    Registry() = default;

    struct Sample {
        std::string name;
        std::string labels;
        std::string help;
        Type type = Type::Counter;
        std::atomic<std::uint64_t> counter{0};
        std::atomic<double> gauge{0.0};
        std::function<double()> pull;   // 非空 => 抓取时求值
    };

    static std::string key(const std::string& name, const std::string& labels);
    // 调用方负责持锁
    Sample* findOrCreateLocked(const std::string& name, const std::string& labels);

    mutable std::mutex mtx_;
    std::map<std::string, std::unique_ptr<Sample>> samples_;
};

inline Registry& reg() { return Registry::instance(); }

}  // namespace metrics
