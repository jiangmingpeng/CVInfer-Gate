#pragma once

#include <cstdint>
#include <string>

#include <opencv2/opencv.hpp>

// IReviewService (复核服务抽象)
// 把"大模型/外部复核"抽象为一个同步调用接口: 调用方(ReviewScheduler 的
// worker 线程)会阻塞等待一次复核结果, 但整条视频流水线不阻塞。
//
// 当前实现: GrpcLlmReviewer(复用 gRPC)。
// 预留: 可替换为本地 VLM/其它后端, 只需实现本接口。

// 复核请求: 一个"灰区/告警候选"目标
struct ReviewRequest {
    std::uint64_t frame_seq = 0; // 帧序号(按此回收结果)
    cv::Mat roi; // 目标小图(已裁剪; 为空视为无效)
    int class_id = -1; // 主模型类别
    std::string label; // 主模型标签(如 "person")
    float confidence = 0.0f; // 主模型置信度
    std::string prompt; // 业务提示(可空)
};

// 复核调用状态: 把"服务不可用/超时"与"调用成功但否决"区分开,
// 便于上层选择降级策略(见 ReviewConfig::alert_on_failure)。
enum class ReviewStatus {
    Ok = 0, // 调用成功(结论见 ReviewResult::confirmed)
    Unavailable, // 服务不可达(连接失败)
    Timeout, // 超时(deadline 到期)
    Failed // 其它错误 / 响应无效
};

inline const char* reviewStatusToString(ReviewStatus s) {
    switch (s) {
        case ReviewStatus::Ok:          return "ok";
        case ReviewStatus::Unavailable: return "unavailable";
        case ReviewStatus::Timeout:     return "timeout";
        case ReviewStatus::Failed:      return "failed";
    }
    return "unknown";
}

// 复核结论(仅 status == Ok 时有意义)
struct ReviewResult {
    bool confirmed = false; // 是否确认(确认 = 应告警)
    std::string label; // 复核标签(如 "no_helmet")
    float confidence = 0.0f; // 复核置信度
    std::string reason; // 复核理由(可空)

    // 可观测(可选): 产出该结论的模型名与服务端处理耗时
    std::string model; // 复核服务上报的模型名(如 "Qwen2.5-VL-7B"); 空 = 未上报
    std::int64_t latency_ms = 0; // 服务端处理耗时(ms); 0 = 未上报
};

class IReviewService {
public:
    virtual ~IReviewService() = default;
    virtual const std::string& name() const = 0;
    // 同步执行一次复核(可阻塞, 但只阻塞调用它的复核 worker 线程)
    virtual ReviewStatus review(const ReviewRequest& req, ReviewResult& out) = 0;
};
