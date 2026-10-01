#include <algorithm>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>
#include <grpcpp/grpcpp.h>

#include "utils/ConfigParser.h"
#include "utils/Logger.h"
#include "utils/LifecycleCoordinator.h"
#include "video/IVideoSource.h"
#include "video/FileVideoSource.h"
#include "video/RtspVideoSource.h"
#include "inference/IModel.h"
#include "inference/CascadeEngine.h"
#include "inference/ModelPoolManager.h"
#include "pipeline/VideoPipeline.h"
#include "review/GrpcLlmReviewer.h"
#include "review/IReviewService.h"
#include "review/ReviewScheduler.h"
#include "pipeline/MultiSensorPipeline.h"
#include "sensor/ISensorSource.h"
#include "sensor/SensorSourceFactory.h"
#include "sensor/VideoSensorSource.h"
#include "database/DBWriter.h"
#include "service/DetectionServiceImpl.h"
#include "service/GrpcServerSetup.h"
#include "utils/RoiUtils.h"
#include "utils/AlertGate.h"
#include "tracking/TargetTracker.h"

// ============================================================
// CVInfer-Gate 主程序 (T7: 装配收口)
// ------------------------------------------------------------
// 与旧版 main.cpp 的差异:
//   1) T2: 引入 Logger, 统一分级日志, 取代散落的 std::cout/cerr
//   2) T6: 引入 LifecycleCoordinator + SignalWatcher, 主线程与
//      gRPC/流水线子线程通过 condition_variable 协作, Ctrl+C 优雅关闭
//   3) gRPC 服务在独立线程启动, 与视频流水线"并行"
//      (旧版是视频跑完才启动 gRPC, 且无法优雅退出)
//   4) T4: 引入 InferenceEnginePool, 推理引擎被流水线 worker 与 gRPC
//      共享, 消除"多线程共用单个 ov::InferRequest"的数据竞争
//   5) T5: 视频处理交棒给 VideoPipeline (解码/抽帧/限速/多 worker/落库)
//   6) 保留原业务规则: 画框写视频、检测入库、person>0.8 触发告警
//   7) T12-T15: 推理链路由"单引擎池"升级为"多模型注册 + IDetector 抽象":
//      ModelPoolManager 按 model_config 批量构建模型(每模型独立引擎池),
//      流水线/gRPC 只依赖 IDetector, 为 T16+ 的多模型级联铺路。
//   8) T16-T19: 当 config.yaml 中 cascade.enabled=true 时, 用 CascadeEngine
//      (同样实现 IDetector)替换单模型: 主筛灰区目标 -> 二级分类器复核。
//      流水线/gRPC 代码无需改动(这正是 Phase A 抽象层的价值)。
//   9) T20-T22: 当 config.yaml 中 review.enabled=true 时, 告警候选目标被裁剪
//      ROI 后异步送大模型复核(复用 gRPC), **复核确认后才写告警**; 画框/写视频/
//      检测入库仍用本地结果实时进行, 复核不阻塞任何流水线线程。
//  10) T23-T26: 当 config.yaml 中 fusion.enabled=true 时, 在 sink **最前置**叠加
//      多模态决策级融合: 雷达/红外采样经 poller 存进有界时间缓冲, 每帧按
//      fusion.time_tolerance_ms 时间对齐 + 目标关联 + 加权置信度融合。
//      视频仍是主模态(画框/落库/告警的框都来自视觉), 融合只调置信度/补测距;
//  11) T39: 告警去重(alert.dedup): 告警判定按帧执行, 而"安全帽缺失"描述的是**目标
//      状态** => 同一静止目标会被连续帧反复告警。新增 AlertGate(标签 + 框重叠 +
//      冷却窗), 在**告警链路上**去重(送审处 / 写告警处), 画框与落库不受影响。
// ============================================================

namespace {

// [T22] 判定某检测是否为"送复核"的候选(与 review.trigger 条件一致)
bool isReviewCandidate(const DetectionResult& det, const ReviewConfig& rc) {
    if (det.confidence < rc.min_conf || det.confidence >= rc.max_conf) return false;
    if (rc.trigger_labels.empty()) return true;
    return std::find(rc.trigger_labels.begin(), rc.trigger_labels.end(), det.label) !=
           rc.trigger_labels.end();
}

// [T39] 单调时钟(ms): 冷却窗必须用单调钟 —— 系统时间被回拨/校正时,
//   steady_clock 不会跳变(否则可能永久抑制或去重失效)。
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// [T27] 配置路径可注入: `--config <path>` / `CVINFER_CONFIG`(优先级: 命令行 > 环境变量 > 默认)
//   动机: config/config.yaml 是运行时唯一入口, 但 CMake 的 POST_BUILD 会用它**覆盖**
//   build/config/config.yaml, 导致"改了 build 下的配置, 一 build 就被还原"。
//   有了这个开关, 就可以用 config/config.test.yaml 等测试配置运行, 互不干扰。
std::string resolvePath(int argc, char** argv, const char* flag, const char* env,
                        const char* default_path) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == flag && i + 1 < argc) return argv[++i];
        const std::string prefix = std::string(flag) + "=";
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    if (const char* e = std::getenv(env)) {
        if (*e) return e;
    }
    return default_path;
}

void printUsage(const char* argv0) {
    std::cout << "用法: " << argv0 << " [选项]\n"
              << "  --config <path>        系统配置(默认 config/config.yaml, 或环境变量 CVINFER_CONFIG)\n"
              << "  --model-config <path>  模型配置(默认 config/model_config.yaml, 或 CVINFER_MODEL_CONFIG)\n"
              << "  --help                 显示本帮助\n";
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    // 0. 加载配置 (路径可注入; 见 resolvePath 注释)
    const std::string app_cfg_path =
        resolvePath(argc, argv, "--config", "CVINFER_CONFIG", "config/config.yaml");
    const std::string model_cfg_path =
        resolvePath(argc, argv, "--model-config", "CVINFER_MODEL_CONFIG", "config/model_config.yaml");

    ConfigParser config_parser;
    if (!config_parser.loadAppConfig(app_cfg_path)) return -1;
    if (!config_parser.loadModelConfig(model_cfg_path)) return -1;

    const auto& app_cfg = config_parser.getAppConfig();

    // ---- T2: 初始化分级日志 ----
    Logger::instance().init(app_cfg.log);
    CVLOG_INFO << "=== CVInfer-Gate 启动 ===";

    // ---- T6: 信号监听必须早于其它线程, 使其继承信号掩码 ----
    LifecycleCoordinator lifecycle;
    SignalWatcher signal_watcher([&lifecycle](int sig) {
        CVLOG_WARN << "收到信号 " << sig << ", 开始优雅关闭...";
        lifecycle.requestShutdown();
    });
    if (!signal_watcher.start()) {
        CVLOG_ERROR << "信号监听启动失败";
        return -1;
    }

    // ---- T9: 数据库 (连接池 + 异步落库) ----
    // [T36] DBWriter::init() 已改为“连不上库也**不**失败”: 降级模式启动(记录先落
    //   本地 CSV), 后台按 database.reconnect_interval_ms 自动重建连接池, 恢复
    //   后回传; 启动只做 1 次快速连库尝试(不再白等 ~20s)。
    //   故这里**不再 return -1** —— 网关必须能“无库运行”。
    //   注: init() 现在恒返回 true, 本分支仅在极端异常时触发。
    DBWriter db_writer;
    if (!db_writer.init(app_cfg)) {
        CVLOG_ERROR << "数据库初始化异常! 将以【无落库】模式启动, 请检查 config.yaml。";
    }

    // 1. 根据配置选择视频源
    std::unique_ptr<IVideoSource> video_source;
    if (app_cfg.video.source_type == "rtsp") {
        video_source = std::make_unique<RtspVideoSource>();
    } else {
        video_source = std::make_unique<FileVideoSource>();
    }
    const bool video_ok = video_source->open(app_cfg.video.source_path);
    if (!video_ok) {
        CVLOG_WARN << "视频源打开失败, 跳过本地视频流水线, 但 gRPC 微服务仍会启动!";
    }
    
    // 2. 获取源帧率: file 从容器元数据, rtsp 从流元数据(avg_frame_rate)
    double source_fps = 30.0;
    if (video_ok) {
        if (app_cfg.video.source_type == "file") {
            cv::VideoCapture meta_cap(app_cfg.video.source_path);
            const double meta_fps = meta_cap.get(cv::CAP_PROP_FPS);
            if (meta_fps > 0 && meta_fps <= 120.0) source_fps = meta_fps;
            meta_cap.release();
        } else {
            const double src_fps = video_source->getFps();
            if (src_fps > 0.0 && src_fps <= 240.0) source_fps = src_fps;
        }
    }

    // [T29] 输出视频的容器帧率 = **抽帧后**的有效帧率, 否则回看会快放:
    //   源 30fps + frame_interval=3 -> sink 实际只拿到 10fps 的帧, 若容器仍写 30,
    //   视频就会 3 倍速播放(实测 RTSP 场景因丢了 65% 的帧, 写出了 8 倍速视频)。
    const int interval = app_cfg.video.frame_interval > 0 ? app_cfg.video.frame_interval : 1;
    double fps = source_fps / static_cast<double>(interval);
    if (app_cfg.video.target_fps > 0 && app_cfg.video.target_fps < fps) {
        fps = app_cfg.video.target_fps;   // 限速比抽帧更严时, 以限速为准
    }
    if (fps <= 0.0) fps = 30.0;
    CVLOG_INFO << "视频帧率: 源=" << source_fps << "fps, frame_interval=" << interval
               << ", 输出容器=" << fps << "fps";

    // 3. T12-T15: 多模型注册 (Phase A 走单模型路径; 级联在 T16+)
    //    ModelPoolManager 按配置批量构建模型: 每个模型(如 YoloDetector)内部持有
    //    独立引擎池, 流水线 worker 与 gRPC 共享同一个 detector, 资源隔离/复用等价于旧版。
    ModelPoolManager model_manager;
    if (!model_manager.init(config_parser.getModelConfigs(), app_cfg.pipeline.worker_threads)) {
        CVLOG_ERROR << "模型初始化失败!";
        db_writer.stop();
        return -1;
    }
    // [T18] 选择检测器: 启用级联则用 CascadeEngine, 否则回退单模型。
    //   级联内部仍依赖 ModelPoolManager 中的模型(共享指针保证生命周期)。
    std::shared_ptr<IDetector> detector;
    if (app_cfg.cascade.enabled) {
        detector = model_manager.buildCascade(app_cfg.cascade);
        if (!detector) {
            CVLOG_WARN << "级联构建失败, 回退单模型检测器。";
        }
    }
    if (!detector) {
        detector = model_manager.primaryDetector();
    }
    if (!detector) {
        CVLOG_ERROR << "未找到可用的检测器模型, 请检查 model_config.yaml 的 role 配置!";
        db_writer.stop();
        return -1;
    }
    CVLOG_INFO << "使用检测器: " << detector->name()
               << (app_cfg.cascade.enabled ? " (级联模式)" : " (单模型模式)");

    // 3.5 [T20-T22] 大模型异步复核 (可选; 复用 gRPC)
    //   仅作用于"告警"链路: 命中 review.trigger 的候选目标裁剪 ROI 后异步送审,
    //   复核确认后才写告警; 画框/写视频/检测入库完全不受影响。
    std::unique_ptr<ReviewScheduler> review_scheduler;
    if (app_cfg.review.enabled) {
        auto reviewer = std::make_shared<GrpcLlmReviewer>();
        if (!reviewer->init(app_cfg.review)) {
            CVLOG_WARN << "复核服务初始化失败(endpoint=" << app_cfg.review.endpoint
                       << "), 告警回退为本地规则。";
        } else {
            // 结果回调(在复核 worker 线程执行): 只做"写告警"(异步入队, 轻量)
            auto on_outcome = [&db_writer, &app_cfg](const ReviewOutcome& out) {
                if (!out.alert) return;
                std::string desc;
                if (out.status == ReviewStatus::Ok) {
                    desc = "复核确认: label=" + out.label +
                           ", conf=" + std::to_string(out.confidence);
                    if (!out.reason.empty()) desc += ", reason=" + out.reason;
                } else {
                    desc = std::string("复核不可用(") + reviewStatusToString(out.status) +
                           "), 按兜底策略告警";
                }
                desc += ", frame=" + std::to_string(out.frame_seq);
                db_writer.writeAlert(app_cfg.review.alert_type, desc);
            };

            review_scheduler = std::make_unique<ReviewScheduler>();
            if (!review_scheduler->init(app_cfg.review, reviewer, on_outcome)) {
                CVLOG_WARN << "复核调度器初始化失败, 告警回退为本地规则。";
                review_scheduler.reset();
            }
        }
    }

    // 3.6 [T23-T26] 多模态决策级融合 (可选)
    //   不改动 VideoPipeline: 融合作为 sink **最前置阶段**就地执行 ——
    //   poller 线程只做 read->入时间缓冲(有界丢最旧), fuse() 只做一次内存快照 +
    //   纯计算, 因此对流水线是"不阻塞"的。
    //   生命周期: video_sensor 必须先于 fusion_stage 声明(后者持有其裸指针),
    //   故析构顺序为 fusion_stage -> video_sensor, 不会悬空。
    std::unique_ptr<sensor::VideoSensorSource> video_sensor;   // 视频时间基(非拥有底层)
    std::unique_ptr<MultiSensorPipeline> fusion_stage;
    if (app_cfg.fusion.enabled) {
        std::vector<std::shared_ptr<sensor::ISensorSource>> sensors;
        for (const auto& sc : app_cfg.sensors) {
            auto src = sensor::createSensorSource(sc);   // 内部已完成 open()
            if (!src) {
                // 单路传感器失败只降级跳过, 不影响主视频链路
                CVLOG_WARN << "传感器构建失败, 已跳过: " << sc.name
                           << " (kind=" << sc.kind << ", backend=" << sc.backend << ")";
                continue;
            }
            sensors.push_back(std::move(src));
        }
        if (sensors.empty()) {
            CVLOG_WARN << "没有可用的非视频传感器, 关闭多模态融合。";
        } else {
            video_sensor = std::make_unique<sensor::VideoSensorSource>(*video_source);
            SensorConfig vcfg;   // 视频源由 main 持有, 这里只借名(不重复 open)
            vcfg.kind = "video";
            vcfg.name = "video";
            video_sensor->open(vcfg);

            fusion_stage = std::make_unique<MultiSensorPipeline>();
            if (!fusion_stage->init(app_cfg.fusion, std::move(sensors), video_sensor.get())) {
                CVLOG_WARN << "多模态融合初始化失败, 关闭融合。";
                fusion_stage.reset();
            }
        }
    }

    // 4. 初始化视频写入器 (AVI + MJPG; 视频不可用时降级为“不写结果视频”, gRPC 仍能启动)
    // [T28] RTSP 兼容: 某些网络流 open() 成功但拿不到分辨率(codec_ctx_->width==0),
    //   旧逻辑会把 0x0 交给 VideoWriter -> 打不开 -> 直接 return -1 退出,
    //   于是“流连上了却启不来服务”。现在改为**降级**: 尺寸未知时不写结果视频,
    //   推理/落库/告警/gRPC 全部照常。
    // [T31] 关键细节: 尺寸未知时**根本不要构造 VideoWriter** ——
    //   传 cv::Size(0,0) 会让 OpenCV 依次试 GStreamer/CV_IMAGES/FFMPEG 后端,
    //   每个后端都抛异常并打印 [ERROR:0] 噪声(实测: GStreamer 断言
    //   “frameSize.width > 0” + CV_IMAGES “can't find starting number: output.avi”),
    //   还可能留下一个“已打开但写不出”的 0x0 writer。故改为延迟 open。
    const int video_width  = video_ok ? video_source->getWidth() : 0;
    const int video_height = video_ok ? video_source->getHeight() : 0;
    bool wrote_video = false;        // [T31] 末尾据此决定是否打印“结果视频已保存”
    cv::VideoWriter video_writer;    // [T31] 延迟 open(尺寸未知时保持关闭, 不触发后端探测)
    if (video_width > 0 && video_height > 0) {
        std::cout << "原视频分辨率: " << video_width << "x" << video_height << std::endl;
        video_writer.open("output.avi",
                          cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                          fps, cv::Size(video_width, video_height));
        wrote_video = video_writer.isOpened();
        if (!wrote_video) {
            // [T28] 不再 return -1: 编码器不可用时降级为“不写结果视频”, 其余照常
            CVLOG_WARN << "无法初始化 VideoWriter(output.avi), 继续运行(仅不写结果视频)。";
        }
    } else if (video_ok) {
        CVLOG_WARN << "视频源未提供分辨率(source_type=" << app_cfg.video.source_type
                   << "), 本次不写结果视频(output.avi)。";
    }
    // (!video_ok 时前面已 warn 过“视频源打开失败”, 此处不重复刷屏)

    // 4.5 [T39] 告警去重闸门 (同标签 + 框重叠 + 冷却窗)
    //   告警判定是每帧执行的, 而"安全帽缺失"描述的是目标状态 => 同一静止目标会被
    //   连续帧反复告警。闸门只作用于**告警链路**(送审处 + 写告警处), 画框/落库不受影响。
    //   sink 回调会在多个 worker 线程并发执行, 故 AlertGate 内部自带 mutex
    //   (已单测: 同目标并发调用只放行一次)。
    alert_gate::Config gate_cfg;
    gate_cfg.enabled     = app_cfg.alert.dedup.enabled;
    gate_cfg.iou         = app_cfg.alert.dedup.iou;
    gate_cfg.cooldown_ms = app_cfg.alert.dedup.cooldown_ms;
    gate_cfg.max_entries = app_cfg.alert.dedup.max_entries;
    alert_gate::AlertGate alert_gate(gate_cfg);
    if (gate_cfg.enabled) {
        CVLOG_INFO << "告警去重: 已启用 (iou=" << gate_cfg.iou
                   << ", cooldown=" << gate_cfg.cooldown_ms << "ms)";
    } else {
        CVLOG_INFO << "告警去重: 已禁用(每帧都可能告警)";
    }

    // 4.6 [T40] 目标跟踪: 给每个目标一个跨帧稳定的 track_id。
    //   位置必须在 sink 内、**融合之后**: 跟踪的应是"最终参与告警的那批目标"。
    //   安全性: sink 是单线程, 且 [T29] 的重排缓冲保证 frame_seq 单调递增 =>
    //   有状态的跟踪器在这里被顺序调用, 天然无并发。
    tracking::Config trk_cfg;
    trk_cfg.enabled     = app_cfg.tracking.enabled;
    trk_cfg.iou         = app_cfg.tracking.iou;
    trk_cfg.dist_factor = app_cfg.tracking.dist_factor;
    trk_cfg.max_age_ms  = app_cfg.tracking.max_age_ms;
    trk_cfg.min_hits    = app_cfg.tracking.min_hits;
    trk_cfg.max_tracks  = app_cfg.tracking.max_tracks;
    std::unique_ptr<tracking::TargetTracker> tracker;
    if (trk_cfg.enabled) {
        tracker = std::make_unique<tracking::TargetTracker>(trk_cfg);
        CVLOG_INFO << "目标跟踪: 已启用 (iou=" << trk_cfg.iou
                   << ", max_age=" << trk_cfg.max_age_ms << "ms)";
    } else {
        CVLOG_INFO << "目标跟踪: 已禁用(告警去重将退回几何重叠判定)";
    }

    // 5. T5: 构造三阶段流水线 (sink 回调负责画框/写视频/异步落库)
    VideoPipeline::Config pipe_cfg;
    pipe_cfg.target_fps = app_cfg.video.target_fps;
    pipe_cfg.frame_interval = app_cfg.video.frame_interval;
    pipe_cfg.queue_max_size = app_cfg.video.queue.max_size;
    pipe_cfg.drop_oldest = (app_cfg.video.queue.policy != "block");
    pipe_cfg.worker_threads = app_cfg.pipeline.worker_threads;

    VideoPipeline pipeline(
        *video_source, *detector, pipe_cfg, lifecycle,
        [&](std::uint64_t frame_seq, const cv::Mat& frame,
            const std::vector<DetectionResult>& detections) {
            // (0) [T25/T26] 多模态决策级融合: sink 最前置, 就地融合(不阻塞)。
            //     启用时在**拷贝**上改写(回调入参是 const, 且下游都应看到融合值);
            //     未启用时零拷贝, 直接引用原入参。
            std::vector<DetectionResult> fused_dets;
            const std::vector<DetectionResult>* dets_ptr = &detections;
            if (fusion_stage) {
                fused_dets = detections;
                fusion_stage->fuse(fused_dets);
                dets_ptr = &fused_dets;
            }

            // (0.5) [T40] 目标跟踪: 就地回写 track_id(未启用时零拷贝)。
            //   跟踪**融合之后**的最终集合 => 传感器补出的目标也能拿到 id。
            std::vector<DetectionResult> tracked_dets;
            if (tracker) {
                tracked_dets = *dets_ptr;
                tracker->update(tracked_dets, nowMs(), frame_seq);
                dets_ptr = &tracked_dets;
            }
            const std::vector<DetectionResult>& dets = *dets_ptr;

            // (1) 画框 / 写视频: 用本地(主筛+二级[+融合])结果, 实时输出, 不等复核
            if (video_writer.isOpened()) {
                cv::Mat annotated = frame.clone();
                for (const auto& det : dets) {
                    cv::rectangle(annotated, det.box, cv::Scalar(0, 255, 0), 2);
                    // [T40] 带上 track_id: 肉眼可验证"同一个人是否只有一个 id"
                    const std::string text =
                        (det.track_id >= 0 ? "#" + std::to_string(det.track_id) + " " : "") +
                        det.label + " " +
                        std::to_string(static_cast<int>(det.confidence * 100)) + "%";
                    cv::putText(annotated, text, cv::Point(det.box.x, det.box.y - 5),
                                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
                }
                video_writer.write(annotated);
            }
            if (dets.empty()) return;

            // (2) 检测入库: 保持现状(原始记录立即落库, 不被复核/融合阻塞)
            db_writer.writeDetections(dets);

            // (3) 告警:
            //     启用复核 -> 候选目标裁剪 ROI 后异步送审, 确认后由 on_outcome 写告警;
            //     未启用   -> 保持原本地规则(person>0.8 立即告警)。
            const bool review_on = (review_scheduler && review_scheduler->enabled());
            for (const auto& det : dets) {
                const bool candidate =
                    review_on ? isReviewCandidate(det, app_cfg.review)
                              : (det.label == "person" && det.confidence > 0.8f);
                if (!candidate) continue;

                // [T39] 去重打点:
                //   复核路径打在**送审处** —— 同一目标只送审一次, 自然不可能重复告警,
                //   而且省掉重复的 VLM 调用(复核回调拿不到框, 无法在那里去重);
                //   本地规则路径打在**写告警处**。
                //   [T40] 有 track_id 时闸门**以身份为准**(与框怎么移动无关)。
                if (review_on) {
                    ReviewRequest job;
                    job.frame_seq = frame_seq;
                    job.roi = roi_utils::crop(frame, det.box, app_cfg.review.roi_padding);
                    job.class_id = det.class_id;
                    job.label = det.label;
                    job.confidence = det.confidence;
                    job.prompt = app_cfg.review.prompt;

                    if (job.roi.empty()) {
                        // ROI 无效无法送审: 按兜底策略处理(需要告警时同样过闸门)
                        if (!app_cfg.review.alert_on_failure) continue;
                        if (!alert_gate.allow(det.label, det.track_id, det.box, nowMs())) continue;
                        db_writer.writeAlert(app_cfg.review.alert_type,
                                             "ROI 无效, 按兜底策略告警, frame=" +
                                                 std::to_string(frame_seq));
                        continue;
                    }
                    if (!alert_gate.allow(det.label, det.track_id, det.box, nowMs())) {
                        CVLOG_DEBUG << "[T39] 冷却窗内同目标重复, 跳过送审: " << det.label
                                    << " frame=" << frame_seq;
                        continue;
                    }
                    review_scheduler->submit(std::move(job));
                } else {
                    if (!alert_gate.allow(det.label, det.track_id, det.box, nowMs())) {
                        CVLOG_DEBUG << "[T39] 冷却窗内同目标重复, 跳过告警: " << det.label
                                    << " frame=" << frame_seq;
                        continue;
                    }
                    db_writer.writeAlert(
                        "安全帽缺失",
                        "检测到未佩戴安全帽的人员, 置信度: " +
                            std::to_string(det.confidence));
                }
            }
        });
            
    // 6. 启动 gRPC 服务 (独立线程, 与流水线并行; 旧版是视频跑完才启动)
    DetectionServiceImpl service(*detector);
    // T8: 按 GrpcConfig 配置消息大小/线程/keepalive 并启动服务
    std::string server_address;
    std::unique_ptr<grpc::Server> server =
        buildAndStartGrpcServer(app_cfg.grpc, service, server_address);
    if (!server) {
        CVLOG_ERROR << "gRPC 服务启动失败, 端口: " << app_cfg.grpc.port;
        db_writer.stop();
        return -1;
    }
    CVLOG_INFO << "gRPC 服务已启动, 监听: " << server_address;

    lifecycle.registerChild();
    std::thread grpc_thread([&server, &lifecycle] {
        server->Wait();
        lifecycle.unregisterChild();
    });

    // 7. 启动视频流水线 (视频源可用时)
    if (video_ok) {
        if (!pipeline.start()) {
            CVLOG_WARN << "视频流水线启动失败, 仅提供 gRPC 服务。";
        }
    }

    // 8. 主线程阻塞, 等待关闭信号 (Ctrl+C)
    CVLOG_INFO << "服务就绪, 按 Ctrl+C 退出。";
    lifecycle.waitUntilShutdown();
    
    // 9. 优雅关闭 (顺序: gRPC -> 流水线 -> 数据库)
    CVLOG_INFO << "正在关闭服务...";
    if (server) server->Shutdown();
    if (grpc_thread.joinable()) grpc_thread.join();

    pipeline.stop();
    const VideoPipeline::Stats st = pipeline.stats();
    CVLOG_INFO << "流水线统计: decoded=" << st.decoded << " dropped=" << st.dropped
               << " processed=" << st.processed << " emitted=" << st.emitted;

    // [T16] 级联统计(若处于级联模式): 主筛/触发/确认/否决/降级
    if (auto* cascade = dynamic_cast<CascadeEngine*>(detector.get())) {
        const CascadeEngine::Stats cs = cascade->stats();
        CVLOG_INFO << "级联统计: primary=" << cs.primary << " triggered=" << cs.triggered
                   << " confirmed=" << cs.confirmed << " rejected=" << cs.rejected
                   << " skipped=" << cs.skipped;
    }

    // [T21] 复核调度器: 停机并排空在途复核(结果回调会写告警),
    //       必须在 db_writer.flush()/stop() 之前完成, 否则告警可能丢失。
    if (review_scheduler) {
        review_scheduler->stop();
        const ReviewScheduler::Stats rs = review_scheduler->stats();
        CVLOG_INFO << "复核统计: submitted=" << rs.submitted << " dropped=" << rs.dropped
                   << " reviewed=" << rs.reviewed << " confirmed=" << rs.confirmed
                   << " rejected=" << rs.rejected << " timeout=" << rs.timeout
                   << " unavailable=" << rs.unavailable << " failed=" << rs.failed;
    }

    // [T26] 多模态融合: 停机并打印统计。必须晚于 pipeline.stop() —— fuse() 由 sink 调用。
    if (fusion_stage) {
        fusion_stage->stop();
        const MultiSensorPipeline::Stats fs = fusion_stage->stats();
        CVLOG_INFO << "融合统计: frames=" << fs.frames
                   << " samples=" << fs.samples_pushed << " dropped=" << fs.samples_dropped
                   << " aligned=" << fs.fusion.aligned << " targets=" << fs.fusion.targets
                   << " matched=" << fs.fusion.matched
                   << " unmatched_sensor=" << fs.fusion.unmatched_sensor
                   << " emitted_sensor_only=" << fs.fusion.emitted_sensor_only
                   << " poll_errors=" << fs.poll_errors;
    }

    // [T39] 告警去重统计: suppressed 越大说明去重越在干活(告警刷屏被压住)
    //   注意: 复核路径是“确认后才告警”, 故这里的 allowed 包含“已放行送审”的次数,
    //   不等于最终告警条数(最终条数看数据库)。
    CVLOG_INFO << "告警去重统计: allowed=" << alert_gate.allowed()
               << " suppressed=" << alert_gate.suppressed()
               << " tracked=" << alert_gate.tracked();

    // [T40] 跟踪统计: spawned/retired 看目标进出, longest_dwell 是行为分析的雏形
    if (tracker) {
        const auto ts = tracker->stats();
        CVLOG_INFO << "目标跟踪统计: frames=" << ts.frames
                   << " spawned=" << ts.spawned
                   << " retired=" << ts.retired
                   << " active=" << ts.active
                   << " matched=" << ts.matched
                   << " longest_dwell=" << tracker->longestDwellMs() << "ms";
    }

    db_writer.flush();
    db_writer.stop();

    video_writer.release();
    video_source->close();
    // [T31] 只有真的写过结果视频才宣告(降级运行时不误导)
    if (wrote_video) CVLOG_INFO << "结果视频已保存至 output.avi";
    CVLOG_INFO << "已安全退出。";
    signal_watcher.stop();

    return 0;
}