#include "fusion/SensorFusion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace {

// 两个框的 IoU(Rect2f); 任一为空框返回 0
float iou(const cv::Rect2f& a, const cv::Rect2f& b) {
    if (a.width <= 0.0f || a.height <= 0.0f || b.width <= 0.0f || b.height <= 0.0f) return 0.0f;
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width,  b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float iw = x2 - x1;
    const float ih = y2 - y1;
    if (iw <= 0.0f || ih <= 0.0f) return 0.0f;
    const float inter = iw * ih;
    const float uni = a.width * a.height + b.width * b.height - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// 标签相容: 任一侧为空(未分类)即相容, 否则要求完全相同
inline bool labelCompatible(const std::string& a, const std::string& b) {
    return a.empty() || b.empty() || a == b;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

namespace fusion {

std::vector<sensor::SensorSample> SensorFusion::align(
    const std::vector<sensor::SensorSample>& samples, std::int64_t anchor_ms, int tol_ms) {
    std::vector<sensor::SensorSample> out;
    if (tol_ms < 0) tol_ms = 0;
    const std::int64_t tol = static_cast<std::int64_t>(tol_ms);
    for (const auto& s : samples) {
        // std::llabs 避免 int64 差值溢出; 用 |Δt| <= tol 实现"容差窗口"
        if (std::llabs(static_cast<long long>(s.timestamp_ms - anchor_ms)) <= tol) {
            out.push_back(s);
        }
    }
    return out;
}

FusionStats SensorFusion::fuse(std::vector<DetectionResult>& vision,
                               const std::vector<sensor::SensorSample>& samples,
                               std::int64_t anchor_ms) const {
    FusionStats st;
    st.samples_seen = samples.size();

    // 目前仅决策级(校验层已保证 level=="decision", 这里再兜一层防御)
    if (cfg_.level != "decision") return st;

    // ---- 1) 时间对齐 ----
    const std::vector<sensor::SensorSample> in_window =
        align(samples, anchor_ms, cfg_.time_tolerance_ms);
    st.aligned = in_window.size();

    // 展平为裸目标指针(带出 loop 外使用; in_window 生命周期覆盖本函数)
    std::vector<const sensor::SensorTarget*> targets;
    targets.reserve(in_window.size());
    for (const auto& s : in_window) {
        for (const auto& t : s.targets) targets.push_back(&t);
    }
    st.targets = targets.size();
    if (targets.empty() || vision.empty()) return st;

    std::vector<char> used(targets.size(), 0);

    // ---- 2) + 3) 关联 + 融合 ----
    for (auto& det : vision) {
        int best = -1;
        float best_score = 0.0f;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (used[i]) continue;
            const sensor::SensorTarget& t = *targets[i];
            if (!labelCompatible(det.label, t.label)) continue;

            float score = 0.0f;
            if (t.box.width > 0.0f && t.box.height > 0.0f) {
                // 传感器带框(红外): 空间关联为主, 未达阈值直接淘汰
                score = iou(cv::Rect2f(det.box), t.box);
                if (score < cfg_.match_iou) continue;
            } else {
                // 传感器无框(雷达): 无空间约束, 标签相容即可关联
                score = 1.0f;
            }
            if (score > best_score) { best_score = score; best = static_cast<int>(i); }
        }
        if (best < 0) continue;   // 该视觉目标无传感器证据 -> 保持原样

        used[static_cast<std::size_t>(best)] = 1;
        const sensor::SensorTarget& t = *targets[static_cast<std::size_t>(best)];

        det.vision_confidence = det.confidence;            // 保留原始视觉置信度
        det.sensor_confidence = t.confidence;
        if (t.distance_m >= 0.0f) det.distance_m = t.distance_m;
        if (cfg_.adopt_sensor_label && !t.label.empty()) det.label = t.label;
        det.fused = true;

        const float w = clamp01(cfg_.sensor_weight);
        det.confidence = clamp01((1.0f - w) * det.vision_confidence + w * t.confidence);
        ++st.matched;
    }

    // ---- 未关联的传感器目标 ----
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (used[i]) continue;
        ++st.unmatched_sensor;

        if (!cfg_.emit_sensor_only) continue;
        const sensor::SensorTarget& t = *targets[i];
        if (t.box.width <= 0.0f || t.box.height <= 0.0f) continue;   // 无框无法定位, 不输出

        DetectionResult d;
        d.class_id = t.class_id;
        d.label = t.label;
        d.confidence = clamp01(t.confidence);
        d.box = cv::Rect(cvRound(t.box.x), cvRound(t.box.y),
                         cvRound(t.box.width), cvRound(t.box.height));
        d.fused = true;
        d.vision_confidence = -1.0f;      // 纯传感器目标, 无视觉置信度
        d.sensor_confidence = t.confidence;
        d.distance_m = t.distance_m;
        vision.push_back(std::move(d));
        ++st.emitted_sensor_only;
    }

    return st;
}

}  // namespace fusion
