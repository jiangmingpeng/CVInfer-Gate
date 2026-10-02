#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "inference/IModel.h"

// CascadeEngine (T16: 级联主筛 + 二级复核, 实现 IDetector)
// 设计要点:
// * 组合而非继承具体模型 —— 持有一个主 IDetector(如 YoloDetector) 与一个
// 可选的二级 IClassifier(如 BehaviorClassifier)。它自身实现 IDetector,
// 因此 VideoPipeline / DetectionServiceImpl 无需任何改动即可用级联替换
// 单模型(这正是 Phase A 抽象层的意义)。
//
// * 只复核"灰区"目标 —— 只有 主模型置信度 ∈ [min_conf, max_conf) 且 类别命中
// trigger_labels 的目标才会裁剪 ROI 交给二级分类器; 其余目标原样透传。
// (用户约束①: 只复核灰区; 避免每帧全量二次推理拖垮帧率)
//
// * 复核失败即降级 —— 二级返回 Busy/Failed 时, 保留主模型结果而不丢弃,
// 保证"复核不可用"绝不误杀真实目标。
//
// * 决策级融合 —— 二级确认/否决只作用于"是否保留该目标"及"写入 sub_* 元数据";
// 不做特征级融合(用户约束⑤)。是否据此告警/入库由 sink 决定(Phase C)。
//
// * 线程安全 —— 多 worker 并发调用 detect(); cfg_ 只读, 统计用原子累加。
//
// 决策规则:
// accept_label 为空 -> 二级置信度 >= accept_conf 即"确认";
// accept_label 非空 -> 需 二级标签 == accept_label 且 置信度 >= accept_conf。
// 确认: 保留(可选 boost_on_confirm 提升主置信度), 写入 sub_class_id/sub_confidence/sub_label;
// 否决: drop_rejected=true 丢弃, 否则保留但带 reviewed/sub_* 标记。
class CascadeEngine : public IDetector {
public:
    // 运行期统计快照 (只读)
    struct Stats {
        std::uint64_t primary = 0; // 主模型检出的目标总数
        std::uint64_t triggered = 0; // 命中灰区、进入二次复核的目标数
        std::uint64_t confirmed = 0; // 二级确认通过
        std::uint64_t rejected = 0; // 二级否决
        std::uint64_t skipped = 0; // 复核不可用(Busy/Failed)而降级保留
    };

    CascadeEngine(std::shared_ptr<IDetector> primary,
                  std::shared_ptr<IClassifier> secondary, // 可空 => 退化为单模型
                  CascadeConfig config);

    // 级联自身无独立模型文件; init 仅用于登记名字(供多模型注册/日志)。
    bool init(const ModelConfig& cfg) override;
    const std::string& name() const override { return name_; }
    ModelRole role() const override { return ModelRole::Detector; }

    DetectStatus detect(const cv::Mat& frame, std::vector<DetectionResult>& out) override;

    Stats stats() const;

    // 判定单个检测是否命中"灰区"(触发二次复核), 供复用/测试
    bool isGrayZone(const DetectionResult& det) const;
    // 计算复核 ROI: 按 roi_padding 外扩并裁剪到图像边界; 无效返回空 Rect
    cv::Rect roiFor(const cv::Rect& box, const cv::Size& image_size) const;

private:
    std::string name_ = "cascade";
    std::shared_ptr<IDetector> primary_;
    std::shared_ptr<IClassifier> secondary_; // 为空 => 单模型直通
    CascadeConfig cfg_;

    // 统计 (多 worker 并发, 原子累加)
    std::atomic<std::uint64_t> primary_count_{0};
    std::atomic<std::uint64_t> triggered_{0};
    std::atomic<std::uint64_t> confirmed_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> skipped_{0};
};
