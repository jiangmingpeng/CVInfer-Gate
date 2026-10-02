#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "inference/DetectionResult.h"

// TargetTracker (T40: 目标跟踪 / track_id)
// 起因: 全链没有 track_id —— OVERVIEW §5 的 🔴 之一。后果有两条:
// 1) 告警去重只能靠"框重叠" => 快速移动的目标照样重复告警;
// 2) 停留/徘徊这类**行为分析**没有立足点(它需要"同一个目标持续了多久")。
//
// 方案: 一个纯逻辑、无外部依赖的**关联式**跟踪器(IoU 贪心 + 质心距离兜底),
// 就地给每个检测回写 det.track_id。刻意**不做**卡尔曼滤波与外观(re-ID)特征:
// 先用最小复杂度把 track_id 打通, 让去重与行为分析有立足点。
//
// 为什么可以"有状态": VideoPipeline 的 sink 是**单线程**, 且 的
// sinkLoop 重排缓冲保证回调拿到的 frame_seq **单调递增**(丢帧只造成 seq 跳变,
// 不会乱序) => 跟踪器在 sink 里被顺序调用, 天然无并发。仍防御性忽略回退帧。
//
// 与告警去重的关系: 有了 track_id, 去重应**以身份为准**(同 id = 同目标, 与框
// 怎么移动无关); 几何重叠只作为"没有 id 时"的兜底 —— 见 AlertGate。
//
// 诚实边界:
// * 无外观特征 => 遮挡/交叉后可能"换 id"(ID switch), 这是关联式跟踪的通病;
// * id 单调递增且**永不复用** —— 否则去重会把新目标误判成旧目标;
// * dwell 只统计"连续被跟踪的时长", 不含失配(遮挡)期间;
// * 跟踪发生在 sink => 只影响告警/统计, 回写的是本帧 detections 的副本,
// gRPC 返回给客户端的检测结果**不含** track_id(proto 未加字段)。

namespace tracking {

struct Config {
    bool enabled = true;
    float iou = 0.30f; // 关联: 框重叠阈值(与 SensorFusion/AlertGate 同口径)
    float dist_factor = 1.0f; // 关联兜底: 质心距离 <= dist_factor * 框半周长; <=0 = 关闭
    int max_age_ms = 1000; // 失配后轨迹保留时长(容忍短暂遮挡/漏检)
    int min_hits = 1; // 连续命中多少次后才输出 track_id(>1 = 抑制瞬时误检)
    std::size_t max_tracks = 256; // 轨迹上限(有界, 超限丢最旧)
};

class TargetTracker {
public:
    struct Track {
        int id = 0;
        std::string label;
        cv::Rect box;
        std::int64_t first_ms = 0; // 首次命中时刻
        std::int64_t last_ms = 0; // 最近一次命中时刻
        int hits = 0; // 累计命中次数
        int misses = 0; // 连续失配次数(>0 表示正在"滑行")
        std::int64_t dwellMs() const { return last_ms - first_ms; }
    };

    struct Stats {
        std::uint64_t frames = 0; // 处理过的帧数
        std::uint64_t matched = 0; // 关联成功的检测数
        std::uint64_t spawned = 0; // 新建轨迹数
        std::uint64_t retired = 0; // 老化回收的轨迹数
        std::uint64_t stale_skipped = 0; // 被忽略的回退帧数
        std::size_t active = 0; // 当前轨迹数
    };

    explicit TargetTracker(Config cfg = Config{}) : cfg_(cfg) {
        // 防御: 配置层已校验, 这里兜一层(组件可独立复用)
        if (cfg_.iou < 0.0f) cfg_.iou = 0.0f;
        if (cfg_.iou > 1.0f) cfg_.iou = 1.0f;
        if (cfg_.dist_factor < 0.0f) cfg_.dist_factor = 0.0f;
        if (cfg_.max_age_ms < 0) cfg_.max_age_ms = 0;
        if (cfg_.min_hits < 1) cfg_.min_hits = 1;
        if (cfg_.max_tracks < 1) cfg_.max_tracks = 1;
    }

    // 就地给 dets 回写 track_id(无轨迹 / 未确认 = -1)。必须顺序调用。
    void update(std::vector<DetectionResult>& dets, std::int64_t now_ms, std::uint64_t seq);

    // 观测 / 测试
    Stats stats() const;
    std::size_t activeTracks() const { return tracks_.size(); }
    const std::vector<Track>& tracks() const { return tracks_; }
    const Track* find(int id) const {
        for (const auto& t : tracks_) {
            if (t.id == id) return &t;
        }
        return nullptr;
    }
    // 当前轨迹里"连续被跟踪最久"的时长(行为分析的雏形)
    std::int64_t longestDwellMs() const {
        std::int64_t best = 0;
        for (const auto& t : tracks_) best = std::max(best, t.dwellMs());
        return best;
    }
    const Config& config() const { return cfg_; }

private:
    void retireExpired(std::int64_t now_ms);
    void enforceCap();

    Config cfg_;
    std::vector<Track> tracks_;
    int next_id_ = 1; // 从 1 开始(-1 已被"无 id"占用)
    bool has_seq_ = false;
    std::uint64_t last_seq_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t matched_ = 0;
    std::uint64_t spawned_ = 0;
    std::uint64_t retired_ = 0;
    std::uint64_t stale_skipped_ = 0;
};

// 局部几何工具: "框"的口径与 SensorFusion/AlertGate 保持一致
// (刻意不抽公共头: 就十来行, 且各模块对框的语义将来可能分化)
namespace detail {
inline float iouOf(const cv::Rect& a, const cv::Rect& b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) return 0.0f;
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    if (x2 <= x1 || y2 <= y1) return 0.0f;
    const float inter = static_cast<float>(x2 - x1) * static_cast<float>(y2 - y1);
    const float uni = static_cast<float>(a.width) * static_cast<float>(a.height) +
                      static_cast<float>(b.width) * static_cast<float>(b.height) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// 框的"半周长": 作为质心距离兼底的尺度(比对角线宽容, 对细长人形更合理)
inline float halfPerimeter(const cv::Rect& r) {
    return 0.5f * (static_cast<float>(r.width) + static_cast<float>(r.height));
}

// 质心距离的**平方**(避免 sqrt; 门槛也按平方比)
inline float centroidDistSq(const cv::Rect& a, const cv::Rect& b) {
    const float ax = static_cast<float>(a.x) + 0.5f * static_cast<float>(a.width);
    const float ay = static_cast<float>(a.y) + 0.5f * static_cast<float>(a.height);
    const float bx = static_cast<float>(b.x) + 0.5f * static_cast<float>(b.width);
    const float by = static_cast<float>(b.y) + 0.5f * static_cast<float>(b.height);
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}
} // namespace detail

inline void TargetTracker::update(std::vector<DetectionResult>& dets, std::int64_t now_ms,
                                  std::uint64_t seq) {
    if (!cfg_.enabled) return;
    // 防御: 回退帧会打乱轨迹。的 sink 重排已保证升序, 这里只做兜底。
    if (has_seq_ && seq <= last_seq_) {
        ++stale_skipped_;
        return;
    }
    has_seq_ = true;
    last_seq_ = seq;
    ++frames_;

    // 1) 先老化: 过期轨迹必须在匹配前退场 —— 否则"离开又回来"的目标会被当成
    // 同一条轨迹复用, 去重也就会漏报新目标。
    retireExpired(now_ms);

    // 2) 关联: 同标签候选, IoU 贪心为主, 质心距离兼底(救快速移动目标)。
    struct Cand {
        float score;
        std::size_t ti;
        std::size_t di;
    };
    std::vector<Cand> cands;
    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
        for (std::size_t di = 0; di < dets.size(); ++di) {
            if (tracks_[ti].label != dets[di].label) continue;
            const float iou = detail::iouOf(tracks_[ti].box, dets[di].box);
            if (iou >= cfg_.iou) {
                cands.push_back(Cand{iou, ti, di});
                continue;
            }
            if (cfg_.dist_factor > 0.0f) {
                const float reach = cfg_.dist_factor * detail::halfPerimeter(dets[di].box);
                if (detail::centroidDistSq(tracks_[ti].box, dets[di].box) <= reach * reach) {
                    cands.push_back(Cand{0.0f, ti, di}); // 兼底得分低于任何 IoU 命中
                }
            }
        }
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.score > b.score; });

    std::vector<bool> track_used(tracks_.size(), false);
    std::vector<bool> det_used(dets.size(), false);
    for (const auto& c : cands) {
        if (track_used[c.ti] || det_used[c.di]) continue; // 贪心: 一对一
        track_used[c.ti] = true;
        det_used[c.di] = true;
        Track& t = tracks_[c.ti];
        t.box = dets[c.di].box; // 用新框续接, 下一帧才接得住
        t.last_ms = now_ms;
        ++t.hits;
        t.misses = 0;
        dets[c.di].track_id = (t.hits >= cfg_.min_hits) ? t.id : -1;
        ++matched_;
    }

    // 3) 未关联的轨迹: 记失配 —— 必须在"新建轨迹"之前做, 否则新轨迹会被误记。
    for (std::size_t ti = 0; ti < tracks_.size(); ++ti) {
        if (!track_used[ti]) ++tracks_[ti].misses;
    }

    // 4) 未关联的检测: 新建轨迹。id 单调递增、永不复用。
    for (std::size_t di = 0; di < dets.size(); ++di) {
        if (det_used[di]) continue;
        Track t;
        t.id = next_id_++;
        t.label = dets[di].label;
        t.box = dets[di].box;
        t.first_ms = now_ms;
        t.last_ms = now_ms;
        t.hits = 1;
        tracks_.push_back(std::move(t));
        ++spawned_;
        dets[di].track_id = (cfg_.min_hits <= 1) ? tracks_.back().id : -1;
    }

    enforceCap();
}

inline TargetTracker::Stats TargetTracker::stats() const {
    Stats s;
    s.frames = frames_;
    s.matched = matched_;
    s.spawned = spawned_;
    s.retired = retired_;
    s.stale_skipped = stale_skipped_;
    s.active = tracks_.size();
    return s;
}

inline void TargetTracker::retireExpired(std::int64_t now_ms) {
    const auto is_stale = [&](const Track& t) {
        return (now_ms - t.last_ms) > static_cast<std::int64_t>(cfg_.max_age_ms);
    };
    const auto first_dead = std::remove_if(tracks_.begin(), tracks_.end(), is_stale);
    retired_ += static_cast<std::uint64_t>(tracks_.end() - first_dead);
    tracks_.erase(first_dead, tracks_.end());
}

// 轨迹数超限: 丢"最久没被命中"的那条 => 长时间运行也不会无界增长
inline void TargetTracker::enforceCap() {
    while (tracks_.size() > cfg_.max_tracks) {
        const auto worst = std::min_element(
            tracks_.begin(), tracks_.end(),
            [](const Track& a, const Track& b) { return a.last_ms < b.last_ms; });
        if (worst == tracks_.end()) break;
        tracks_.erase(worst);
        ++retired_;
    }
}

} // namespace tracking
