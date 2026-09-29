#include "inference/BehaviorClassifier.h"

#include <fstream>

#include "utils/Logger.h"

std::vector<std::string> BehaviorClassifier::loadLabels(const std::string& path) {
    std::vector<std::string> labels;
    std::ifstream infile(path);
    if (!infile) {
        CVLOG_WARN << "[BehaviorClassifier] 无法打开标签文件: " << path;
        return labels;
    }
    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) labels.push_back(line);
    }
    return labels;
}

bool BehaviorClassifier::init(const ModelConfig& cfg) {
    cfg_ = cfg;
    if (!cfg_.name.empty()) name_ = cfg_.name;

    acquire_timeout_ = std::chrono::milliseconds(cfg_.acquire_timeout_ms > 0
                                                     ? cfg_.acquire_timeout_ms
                                                     : 2000);

    std::vector<std::string> labels = loadLabels(cfg_.labels_path);
    if (labels.empty()) {
        CVLOG_WARN << "[BehaviorClassifier] 标签为空(路径: " << cfg_.labels_path
                   << "), 分类结果的 label 将为类别下标。";
    }
    post_ = std::make_unique<ClassificationPostProcessor>(std::move(labels));

    const int pool_size = cfg_.pool_size < 1 ? 1 : cfg_.pool_size;
    pool_ = std::make_unique<InferenceEnginePool>();
    if (!pool_->init(cfg_, pool_size)) {
        CVLOG_ERROR << "[BehaviorClassifier] 引擎池初始化失败: " << name_;
        pool_.reset();
        return false;
    }

    CVLOG_INFO << "[BehaviorClassifier] 初始化完成: name=" << name_
               << ", pool=" << pool_size
               << ", input=" << cfg_.input_width << "x" << cfg_.input_height;
    return true;
}

DetectStatus BehaviorClassifier::classify(const cv::Mat& roi, Classification& out) {
    out = Classification{};
    if (!pool_ || !post_ || roi.empty()) return DetectStatus::Failed;

    // 1. 借引擎 (RAII 自动归还)
    EngineGuard guard(*pool_, acquire_timeout_);
    if (!guard.valid()) return DetectStatus::Busy;

    // 2. 推理 (引擎内部完成 resize/归一化)
    std::vector<ov::Tensor> outputs;
    if (!guard->infer(roi, outputs) || outputs.empty()) return DetectStatus::Failed;

    // 3. 后处理 (张量 -> argmax)
    out = post_->process(outputs[0]);
    return out.class_id < 0 ? DetectStatus::Failed : DetectStatus::Ok;
}
