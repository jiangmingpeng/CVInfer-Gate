#include "inference/CascadeEngine.h"

#include <algorithm>

#include "utils/Logger.h"
#include "utils/RoiUtils.h"   // roi_utils::expandAndClamp (与 T22 大模型复核共用)

CascadeEngine::CascadeEngine(std::shared_ptr<IDetector> primary,
                             std::shared_ptr<IClassifier> secondary,
                             CascadeConfig config)
    : primary_(std::move(primary)),
      secondary_(std::move(secondary)),
      cfg_(std::move(config)) {}

bool CascadeEngine::init(const ModelConfig& cfg) {
    if (!cfg.name.empty()) name_ = cfg.name;
    return true;   // 主/二级组件已在构造时注入
}

bool CascadeEngine::isGrayZone(const DetectionResult& det) const {
    // 置信度低于下界: 太弱, 不浪费二级算力(直接交由后续策略处理)
    if (det.confidence < cfg_.min_conf) return false;
    // 置信度达到上界: 已足够确定, 无需复核
    if (det.confidence >= cfg_.max_conf) return false;
    // 类别白名单(空 = 所有类别)
    if (!cfg_.trigger_labels.empty()) {
        const auto& v = cfg_.trigger_labels;
        if (std::find(v.begin(), v.end(), det.label) == v.end()) return false;
    }
    return true;
}

cv::Rect CascadeEngine::roiFor(const cv::Rect& box, const cv::Size& image_size) const {
    return roi_utils::expandAndClamp(box, image_size, cfg_.roi_padding);
}

DetectStatus CascadeEngine::detect(const cv::Mat& frame, std::vector<DetectionResult>& out) {
    out.clear();
    if (!primary_ || frame.empty()) return DetectStatus::Failed;

    // 1. 主筛: 与单模型语义一致
    std::vector<DetectionResult> primary_results;
    const DetectStatus pst = primary_->detect(frame, primary_results);
    if (pst != DetectStatus::Ok) return pst;

    primary_count_ += primary_results.size();

    // 2. 无二级分类器 => 退化为单模型直通(行为与 YoloDetector 完全相同)
    if (!secondary_) {
        out = std::move(primary_results);
        return DetectStatus::Ok;
    }

    std::uint64_t triggered = 0, confirmed = 0, rejected = 0, skipped = 0;

    for (auto& det : primary_results) {
        // 非灰区: 原样透传, 绝不复核
        if (!isGrayZone(det)) {
            out.push_back(std::move(det));
            continue;
        }
        ++triggered;

        const cv::Rect roi = roiFor(det.box, frame.size());
        if (roi.width <= 0 || roi.height <= 0) {
            out.push_back(std::move(det));   // ROI 无效: 降级保留
            continue;
        }

        Classification cls;
        const DetectStatus cst = secondary_->classify(frame(roi), cls);

        det.reviewed = true;
        if (cst != DetectStatus::Ok) {
            // 复核不可用: 保留主模型结果, 不丢弃(降级, 绝不误杀)
            ++skipped;
            out.push_back(std::move(det));
            continue;
        }

        // 记录二级结果, 供告警/入库使用(Phase C)
        det.sub_class_id = cls.class_id;
        det.sub_confidence = cls.confidence;
        det.sub_label = cls.label;

        const bool label_ok = cfg_.accept_label.empty() || (cls.label == cfg_.accept_label);
        const bool pass = label_ok && (cls.confidence >= cfg_.accept_conf);

        if (pass) {
            ++confirmed;
            if (cfg_.boost_on_confirm) det.confidence = cls.confidence;
            out.push_back(std::move(det));
        } else {
            ++rejected;
            if (!cfg_.drop_rejected) {
                out.push_back(std::move(det));   // 保留但已带 reviewed/sub_* 标记
            }
            // drop_rejected = true => 丢弃该目标
        }
    }

    triggered_ += triggered;
    confirmed_ += confirmed;
    rejected_ += rejected;
    skipped_ += skipped;
    return DetectStatus::Ok;
}

CascadeEngine::Stats CascadeEngine::stats() const {
    Stats s;
    s.primary = primary_count_.load();
    s.triggered = triggered_.load();
    s.confirmed = confirmed_.load();
    s.rejected = rejected_.load();
    s.skipped = skipped_.load();
    return s;
}
