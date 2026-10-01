#include "utils/Metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <utility>

namespace metrics {

namespace {

const char* typeName(Type t) { return t == Type::Counter ? "counter" : "gauge"; }

// HELP 文本里的反斜杠/换行需要转义(Prometheus 文本格式约定)
std::string escapeHelp(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string formatValue(double v, Type t) {
    char buf[64];
    if (t == Type::Counter) {
        // 计数器按整数输出(负值不可能出现; 防御性截断为 0)
        const auto n = v < 0.0 ? 0ULL
                               : static_cast<unsigned long long>(v + 0.5);
        std::snprintf(buf, sizeof(buf), "%llu", n);
    } else {
        if (std::isnan(v) || std::isinf(v)) return "0";
        std::snprintf(buf, sizeof(buf), "%.6g", v);
    }
    return std::string(buf);
}

}  // namespace

Registry& Registry::instance() {
    static Registry inst;
    return inst;
}

std::string Registry::key(const std::string& name, const std::string& labels) {
    // 用不可见分隔符拼 key => 同一指标名的所有样本在 std::map 里**相邻**,
    // render() 顺序遍历即可自然分组(不必额外排序)。
    return name + '\x1f' + labels;
}

Registry::Sample* Registry::findOrCreateLocked(const std::string& name,
                                                const std::string& labels) {
    const std::string k = key(name, labels);
    auto it = samples_.find(k);
    if (it != samples_.end()) return it->second.get();

    auto s = std::make_unique<Sample>();
    s->name = name;
    s->labels = labels;
    Sample* raw = s.get();
    samples_.emplace(k, std::move(s));
    return raw;
}

void Registry::declare(const std::string& name, Type type, const std::string& help,
                       const std::string& labels) {
    std::lock_guard<std::mutex> lock(mtx_);
    Sample* s = findOrCreateLocked(name, labels);
    s->type = type;
    if (!help.empty()) s->help = help;
    s->pull = nullptr;   // 声明为"推"式(会被后续 inc/setGauge 使用)
}

void Registry::inc(const std::string& name, std::uint64_t n) {
    inc(name, std::string(), n);
}

void Registry::inc(const std::string& name, const std::string& labels, std::uint64_t n) {
    std::lock_guard<std::mutex> lock(mtx_);
    Sample* s = findOrCreateLocked(name, labels);
    if (s->pull) return;   // 拉式指标不接受手推(避免两套数据打架)
    s->counter.fetch_add(n, std::memory_order_relaxed);
}

void Registry::setGauge(const std::string& name, double value) {
    setGauge(name, std::string(), value);
}

void Registry::setGauge(const std::string& name, const std::string& labels, double value) {
    std::lock_guard<std::mutex> lock(mtx_);
    Sample* s = findOrCreateLocked(name, labels);
    if (s->pull) return;
    s->type = Type::Gauge;
    s->gauge.store(value, std::memory_order_relaxed);
}

void Registry::addCollector(const std::string& name, Type type, const std::string& help,
                            std::function<double()> fn, const std::string& labels) {
    if (!fn) return;
    std::lock_guard<std::mutex> lock(mtx_);
    Sample* s = findOrCreateLocked(name, labels);
    s->type = type;
    s->help = help;
    s->pull = std::move(fn);
}

std::string Registry::render() const {
    std::lock_guard<std::mutex> lock(mtx_);

    // 先按名字汇总 HELP/TYPE(同名的多个标签样本共用一份) —— 这样即使最先遇到的
    // 那个样本没带 help, 也不会漏掉 # HELP 行。
    std::map<std::string, std::pair<std::string, Type>> meta;
    for (const auto& kv : samples_) {
        const Sample& s = *kv.second;
        auto it = meta.find(s.name);
        if (it == meta.end()) meta.emplace(s.name, std::make_pair(s.help, s.type));
        else if (it->second.first.empty() && !s.help.empty()) it->second.first = s.help;
    }

    std::ostringstream oss;
    std::string current;
    for (const auto& kv : samples_) {          // key = name + '\x1f' + labels => 按名字天然分组
        const Sample& s = *kv.second;
        double value = 0.0;
        if (s.pull) {
            try {
                value = s.pull();
            } catch (...) {
                value = 0.0;   // 抓取端绝不能因为一个回调抛异常就整体 500
            }
        } else if (s.type == Type::Counter) {
            value = static_cast<double>(s.counter.load(std::memory_order_relaxed));
        } else {
            value = s.gauge.load(std::memory_order_relaxed);
        }

        if (s.name != current) {
            current = s.name;
            const auto it = meta.find(s.name);
            if (it != meta.end() && !it->second.first.empty())
                oss << "# HELP " << s.name << ' ' << escapeHelp(it->second.first) << '\n';
            oss << "# TYPE " << s.name << ' '
                << typeName(it != meta.end() ? it->second.second : s.type) << '\n';
        }
        oss << s.name;
        if (!s.labels.empty()) oss << '{' << s.labels << '}';
        oss << ' ' << formatValue(value, s.type) << '\n';
    }
    return oss.str();
}

void Registry::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    samples_.clear();
}

std::size_t Registry::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return samples_.size();
}

}  // namespace metrics
