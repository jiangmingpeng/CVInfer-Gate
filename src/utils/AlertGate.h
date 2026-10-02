#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <opencv2/core.hpp>

// AlertGate (T39: 告警去重)
// 起因: 告警判定是**每帧**执行的, 而"安全帽缺失"这类告警描述的是**目标状态**,
// 不是帧事件 —— 同一个人在画面里站着不动, 会被连续帧反复告警(每秒 N 条)。
// OVERVIEW §5 把"同一目标连续帧重复告警"列为缺失项, 这里把它补上。
//
// 方案: 以 (标签 + 同一目标) 认定"同一目标", 冷却窗内只放行一次。
// * 判"同一目标"的优先级: 两边都有 track_id 时**比身份**(同 id = 同目标,
// 与框怎么移动无关); 没有 id 时退回**框重叠 >= iou** => 快速移动目标不再重复
// 告警, 且对未启用跟踪的链路零影响。
// * 冷却窗按**最近一次命中**刷新(滑动窗): 目标持续在画面里 => 只告警一次;
// 消失超过 cooldown_ms 后再次出现 => 重新告警。
// * 不同标签互不影响(人与车各自独立计时)。
// * 有界(max_entries, 满则丢最旧) => 长时间运行不会无界增长。
// * 线程安全(内部 mutex): 虽然当前 sink 是单线程, 组件本身可独立复用。
//
// 诚实边界:
// * 本组件**不产生** track_id, 只消费调用方给的 id(见 tracking::TargetTracker);
// * 没有 id 时仍是几何去重: 快速移动目标的重叠会掉出 iou 阈值 => 仍可能重复;
// * 身份去重依赖"id 永不复用"(TargetTracker 保证)。

namespace alert_gate {

struct Config {
    bool enabled = true; // 关掉 => allow() 恒为 true(完全等价于旧行为)
    float iou = 0.30f; // 判定"同一目标"的框重叠阈值 [0,1]; 0 = 退化为仅按标签
    int cooldown_ms = 5000; // 冷却窗(ms); 0 = 等于不去重
    int max_entries = 256; // 记忆条目上限(有界)
};

class AlertGate {
public:
    explicit AlertGate(Config cfg = Config{}) : cfg_(cfg) {
        // 防御: 配置层已校验, 这里兜一层(本组件可独立复用)
        if (cfg_.iou < 0.0f) cfg_.iou = 0.0f;
        if (cfg_.iou > 1.0f) cfg_.iou = 1.0f;
        if (cfg_.cooldown_ms < 0) cfg_.cooldown_ms = 0;
        if (cfg_.max_entries < 1) cfg_.max_entries = 1;
    }

    // 是否放行这次告警? 放行则记住 (label, track_id, box, now_ms)。
    // track_id >= 0 时**以身份为准**; -1 = 无身份, 退回几何重叠。
    bool allow(const std::string& label, int track_id, const cv::Rect& box, std::int64_t now_ms) {
        if (!cfg_.enabled) {
            allowed_.fetch_add(1);
            return true;
        }
        std::lock_guard<std::mutex> lk(mtx_);

        // 1) 淘汰过期条目。entries_ 恒按 last_ms 递增(命中时移到尾部), 故只看队首。
        while (!entries_.empty() &&
               (now_ms - entries_.front().last_ms) >= static_cast<std::int64_t>(cfg_.cooldown_ms)) {
            entries_.pop_front();
        }

        // 2) 找"同一目标"的历史条目: 两边都有 track_id 就比身份, 否则比框重叠
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->label != label) continue;
            const bool same = (track_id >= 0 && it->track_id >= 0)
                                  ? (it->track_id == track_id)
                                  : (iou(it->box, box) >= cfg_.iou);
            if (!same) continue;
            // 命中: 刷新冷却窗, 并用新框/新 id 覆盖(目标移动时靠身份仍能连续命中)
            Entry e = *it;
            e.box = box;
            if (track_id >= 0) e.track_id = track_id;
            e.last_ms = now_ms;
            entries_.erase(it);
            entries_.push_back(std::move(e));
            suppressed_.fetch_add(1);
            return false;
        }

        // 3) 新目标: 记录 + 放行; 超限丢最旧
        entries_.push_back(Entry{label, track_id, box, now_ms});
        while (entries_.size() > static_cast<std::size_t>(cfg_.max_entries)) {
            entries_.pop_front();
        }
        allowed_.fetch_add(1);
        return true;
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        entries_.clear();
    }

    // 兼容入口: 没有 track_id 的调用方(未启用跟踪)按几何重叠去重
    bool allow(const std::string& label, const cv::Rect& box, std::int64_t now_ms) {
        return allow(label, -1, box, now_ms);
    }

    // 观测(与项目其它组件一致: 退出时打印)
    std::uint64_t allowed() const { return allowed_.load(); }
    std::uint64_t suppressed() const { return suppressed_.load(); }
    std::size_t tracked() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return entries_.size();
    }
    const Config& config() const { return cfg_; }

    // 两框 IoU(cv::Rect 的 x,y,w,h 语义); 任一为空框返回 0
    static float iou(const cv::Rect& a, const cv::Rect& b) {
        if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) return 0.0f;
        const int x1 = std::max(a.x, b.x);
        const int y1 = std::max(a.y, b.y);
        const int x2 = std::min(a.x + a.width, b.x + b.width);
        const int y2 = std::min(a.y + a.height, b.y + b.height);
        const int iw = x2 - x1;
        const int ih = y2 - y1;
        if (iw <= 0 || ih <= 0) return 0.0f;
        const float inter = static_cast<float>(iw) * static_cast<float>(ih);
        const float uni = static_cast<float>(a.width) * static_cast<float>(a.height) +
                          static_cast<float>(b.width) * static_cast<float>(b.height) - inter;
        return uni > 0.0f ? inter / uni : 0.0f;
    }

private:
    struct Entry {
        std::string label;
        int track_id = -1; // -1 = 这条记录没有身份信息(退回几何重叠)
        cv::Rect box;
        std::int64_t last_ms = 0;
    };

    Config cfg_;
    mutable std::mutex mtx_;
    std::deque<Entry> entries_; // 按 last_ms 递增
    std::atomic<std::uint64_t> allowed_{0};
    std::atomic<std::uint64_t> suppressed_{0};
};

} // namespace alert_gate
