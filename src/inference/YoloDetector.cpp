#include "inference/YoloDetector.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>

#include "utils/Logger.h"

namespace {
// 后处理分段计时(每 100 帧打一行均值)
std::atomic<std::uint64_t> g_post_ns{0};
std::atomic<std::uint64_t> g_post_calls{0};
} // namespace

std::vector<std::string> YoloDetector::loadLabels(const std::string& path) {
    std::vector<std::string> labels;
    std::ifstream infile(path);
    if (!infile) {
        CVLOG_WARN << "[YoloDetector] 无法打开标签文件: " << path;
        return labels;
    }
    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) labels.push_back(line);
    }
    return labels;
}

bool YoloDetector::init(const ModelConfig& cfg) {
    cfg_ = cfg;
    if (!cfg_.name.empty()) name_ = cfg_.name;

    // 借引擎超时: 用模型自身配置, 缺省 2000ms
    acquire_timeout_ = std::chrono::milliseconds(cfg_.acquire_timeout_ms > 0
                                                     ? cfg_.acquire_timeout_ms
                                                     : 2000);

    // 后处理(阈值来自模型配置)
    post_ = std::make_unique<YoloPostProcessor>(cfg_.conf_threshold, cfg_.nms_threshold);

    // 标签
    labels_ = loadLabels(cfg_.labels_path);
    if (labels_.empty()) {
        CVLOG_WARN << "[YoloDetector] 标签为空(路径: " << cfg_.labels_path
                   << "), 检测结果的 label 将为 unknown。";
    }

    // 引擎池 (pool_size <= 0 时按 1 兜底; 通常由 ModelPoolManager 预先填好)
    const int pool_size = cfg_.pool_size < 1 ? 1 : cfg_.pool_size;
    pool_ = std::make_unique<InferenceEnginePool>();
    if (!pool_->init(cfg_, pool_size)) {
        CVLOG_ERROR << "[YoloDetector] 引擎池初始化失败: " << name_;
        pool_.reset();
        return false;
    }

    CVLOG_INFO << "[YoloDetector] 初始化完成: name=" << name_
               << ", pool=" << pool_size
               << ", labels=" << labels_.size()
               << ", conf=" << cfg_.conf_threshold
               << ", nms=" << cfg_.nms_threshold;
    return true;
}

DetectStatus YoloDetector::detect(const cv::Mat& frame,
                                  std::vector<DetectionResult>& out) {
    out.clear();
    if (!pool_ || !post_ || frame.empty()) return DetectStatus::Failed;

    // 1. 借引擎 (RAII 自动归还)
    EngineGuard guard(*pool_, acquire_timeout_);
    if (!guard.valid()) return DetectStatus::Busy;

    // 2. 推理
    std::vector<ov::Tensor> outputs;
    if (!guard->infer(frame, outputs) || outputs.empty()) return DetectStatus::Failed;

    // 3. 后处理 (YOLO 输出 -> 检测框)
    const auto t_post0 = std::chrono::steady_clock::now();
    out = post_->process(outputs[0], frame.size(), labels_);

    // 后处理分段计时(每 100 帧一行均值)
    {
        const std::uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t_post0)
                                     .count();
        g_post_ns.fetch_add(ns, std::memory_order_relaxed);
        const std::uint64_t n = g_post_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n % 100 == 0) {
            std::printf("[T35] 后处理耗时/帧: %.2f ms  (累计 %llu 帧)\n",
                        static_cast<double>(g_post_ns.load()) / (1e6 * static_cast<double>(n)),
                        static_cast<unsigned long long>(n));
            std::fflush(stdout);
        }
    }
    return DetectStatus::Ok;
}
