#include <algorithm>
#include <atomic>
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
#include "service/MetricsServer.h"
#include "utils/RoiUtils.h"
#include "utils/AlertGate.h"
#include "utils/Metrics.h"
#include "tracking/TargetTracker.h"
#include "occupancy/SeatOccupancyAnalyzer.h"
#include "alert/AlertNotifier.h"
#include "inference.grpc.pb.h" // --health-check 探针要用的 Health RPC 存根

// 版本号由 CMake 注入(project(... VERSION x.y.z)); 脱离 CMake 单独编译时兜底 "dev"
#ifndef CV_GATE_VERSION
#define CV_GATE_VERSION "dev"
#endif

// CVInfer-Gate 主程序 (装配收口)
// 与旧版 main.cpp 的差异:
// 1) 引入 Logger, 统一分级日志, 取代散落的 std::cout/cerr
// 2) 引入 LifecycleCoordinator + SignalWatcher, 主线程与
// gRPC/流水线子线程通过 condition_variable 协作, Ctrl+C 优雅关闭
// 3) gRPC 服务在独立线程启动, 与视频流水线"并行"
// (旧版是视频跑完才启动 gRPC, 且无法优雅退出)
// 4) 引入 InferenceEnginePool, 推理引擎被流水线 worker 与 gRPC
// 共享, 消除"多线程共用单个 ov::InferRequest"的数据竞争
// 5) 视频处理交棒给 VideoPipeline (解码/抽帧/限速/多 worker/落库)
// 6) 保留原业务规则: 画框写视频、检测入库、person>0.8 触发告警
// 7) 推理链路由"单引擎池"升级为"多模型注册 + IDetector 抽象":
// ModelPoolManager 按 model_config 批量构建模型(每模型独立引擎池),
// 流水线/gRPC 只依赖 IDetector, 为后续的多模型级联铺路。
// 8) 当 config.yaml 中 cascade.enabled=true 时, 用 CascadeEngine
// (同样实现 IDetector)替换单模型: 主筛灰区目标 -> 二级分类器复核。
// 流水线/gRPC 代码无需改动(这正是 Phase A 抽象层的价值)。
// 9) 当 config.yaml 中 review.enabled=true 时, 告警候选目标被裁剪
// ROI 后异步送大模型复核(复用 gRPC), **复核确认后才写告警**; 画框/写视频/
// 检测入库仍用本地结果实时进行, 复核不阻塞任何流水线线程。
// 10) 当 config.yaml 中 fusion.enabled=true 时, 在 sink **最前置**叠加
// 多模态决策级融合: 雷达/红外采样经 poller 存进有界时间缓冲, 每帧按
// fusion.time_tolerance_ms 时间对齐 + 目标关联 + 加权置信度融合。
// 视频仍是主模态(画框/落库/告警的框都来自视觉), 融合只调置信度/补测距;
// 11) 告警去重(alert.dedup): 告警判定按帧执行, 而"某目标疑似违规"描述的是**目标
// 状态** => 同一静止目标会被连续帧反复告警。新增 AlertGate(标签 + 框重叠 +
// 冷却窗), 在**告警链路上**去重(送审处 / 写告警处), 画框与落库不受影响。
// 12) 运维收口 —— 把"能跑"补成"好运维":
// (a) 告警可推 webhook(alert.push): 有界队列 + 重试退避, 不阻塞流水线;
// (b) /metrics 指标端点(metrics.enabled) + 日志文件轮转(app.log_max_size_mb);
// (c) gRPC Health RPC + `--health-check` 探针, 供容器 healthcheck / systemd 判活。
// 13) 占座判定(occupancy): 在"检测"与"大模型复核"之间补上一层**规则层** ——
// 静态座位 zone + 几何关系(物品属于哪个座位? 人在不在用?) + 时序状态机
// ("物品在且人不在"持续了多久?), 把散落的检测框变成"某座位被长期占用"
// 这个**可解释**的结论。规则层完成"筛"之后, 只有候选才送 VLM 复核
// (调用量降 1~2 个数量级), 且送审时带上结构化证据(物品/时长/投票情况),
// 让 VLM 从"看图猜场景"变成"对证据做裁判"。
// 规则本身**不依赖模型**, 因此可脱离模型/视频做确定性单测。

namespace {

// 判定某检测是否为"送复核"的候选(与 review.trigger 条件一致)
bool isReviewCandidate(const DetectionResult& det, const ReviewConfig& rc) {
    if (det.confidence < rc.min_conf || det.confidence >= rc.max_conf) return false;
    if (rc.trigger_labels.empty()) return true;
    return std::find(rc.trigger_labels.begin(), rc.trigger_labels.end(), det.label) !=
           rc.trigger_labels.end();
}

// 单调时钟(ms): 冷却窗必须用单调钟 —— 系统时间被回拨/校正时,
// steady_clock 不会跳变(否则可能永久抑制或去重失效)。
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// epoch 毫秒: 只用于"对外"的时间戳(uptime 基准、告警 webhook 的 ts_ms)
std::int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// 配置路径可注入: `--config <path>` / `CVINFER_CONFIG`(优先级: 命令行 > 环境变量 > 默认)
// 动机: config/config.yaml 是运行时唯一入口, 但 CMake 的 POST_BUILD 会用它**覆盖**
// build/config/config.yaml, 导致"改了 build 下的配置, 一 build 就被还原"。
// 有了这个开关, 就可以用 config/config.test.yaml 等测试配置运行, 互不干扰。
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

// 占座告警的标签/类型标识。同时用作告警去重的 key —— 座位是静态的, 用座位框做
// 几何去重最精确("同一座位在冷却窗内只报一次"正是想要的语义)。
constexpr const char* kOccupancyLabel = "seat_occupancy";

// 把字符串列表拼成 "a, b, c"(仅用于启动日志; 空列表表示"用组件内置默认")
std::string joinStrings(const std::vector<std::string>& v) {
    if (v.empty()) return "(内置默认)";
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += v[i];
    }
    return s;
}

// 配置里的座位(纯坐标, 见 ConfigParser.h 不引入 OpenCV 的说明)
// -> 运行期的座位区域(cv::Point 多边形; rect 写法已在配置层展开为 4 个顶点)
std::vector<occupancy::SeatZone> buildSeatZones(const std::vector<SeatZoneConfig>& zones) {
    std::vector<occupancy::SeatZone> out;
    out.reserve(zones.size());
    for (const auto& z : zones) {
        occupancy::SeatZone sz;
        sz.name = z.name;
        sz.polygon.reserve(z.polygon.size());
        for (const auto& p : z.polygon) sz.polygon.emplace_back(p.x, p.y);
        out.push_back(std::move(sz));
    }
    return out;
}

void printUsage(const char* argv0) {
    std::cout << "用法: " << argv0 << " [选项]\n"
              << "  --config <path>        系统配置(默认 config/config.yaml, 或环境变量 CVINFER_CONFIG)\n"
              << "  --model-config <path>  模型配置(默认 config/model_config.yaml, 或 CVINFER_MODEL_CONFIG)\n"
              << "  --health-check[=addr]  只做一次健康探针后退出(不加载模型/不连库)\n"
              << "                         默认 127.0.0.1:<grpc.port>, 可用 CVINFER_HEALTH_ADDR 覆盖;\n"
              << "                         退出码 0=serving 1=失败 2=连不上 3=超时 4=未授权\n"
              << "  --help                 显示本帮助\n";
}

} // namespace

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
    const std::int64_t start_ms = epochMs(); // 进程启动时刻(uptime 基准)

    // --health-check: 一次性健康探针
    // 存在意义: 容器 healthcheck / systemd / 负载均衡需要一个"进程真的在服务"的判据。
    // 刻意**不加载模型、不连数据库**(探针必须秒级返回), 只问 gRPC 的 Health RPC;
    // 鉴权口径与 Detect 一致 => 开了鉴权就带上 GRPC_AUTH_TOKEN。
    // 退出码: 0=serving 1=失败 2=连不上 3=超时 4=未授权
    {
        bool health_check = false;
        std::string health_addr;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--health-check") {
                health_check = true;
            } else if (a.rfind("--health-check=", 0) == 0) {
                health_check = true;
                health_addr = a.substr(15); // "--health-check=" 长度 15
            }
        }
        if (health_check) {
            if (health_addr.empty()) {
                if (const char* e = std::getenv("CVINFER_HEALTH_ADDR")) { if (*e) health_addr = e; }
            }
            if (health_addr.empty()) health_addr = "127.0.0.1:" + std::to_string(app_cfg.grpc.port);

            auto channel = grpc::CreateChannel(health_addr, grpc::InsecureChannelCredentials());
            auto stub = inference::DetectionService::NewStub(channel);
            grpc::ClientContext ctx;
            const int timeout = app_cfg.grpc.timeout_ms > 0 ? app_cfg.grpc.timeout_ms : 5000;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout));
            if (!app_cfg.grpc.auth_token.empty()) {
                ctx.AddMetadata("authorization", "Bearer " + app_cfg.grpc.auth_token);
            }
            inference::HealthRequest req;
            inference::HealthResponse resp;
            const grpc::Status st = stub->Health(&ctx, req, &resp);
            if (st.ok() && resp.serving()) {
                std::cout << "[health-check] OK addr=" << health_addr
                          << " version=" << resp.version()
                          << " uptime_ms=" << resp.uptime_ms()
                          << " " << resp.detail() << std::endl;
                return 0;
            }
            int code = 1;
            std::string why = "失败";
            switch (st.error_code()) {
                case grpc::StatusCode::UNAVAILABLE:       code = 2; why = "连不上(服务未启动?)"; break;
                case grpc::StatusCode::DEADLINE_EXCEEDED: code = 3; why = "超时"; break;
                case grpc::StatusCode::UNAUTHENTICATED:   code = 4; why = "未授权(检查 GRPC_AUTH_TOKEN)"; break;
                default: break;
            }
            if (st.ok() && !resp.serving()) why = "进程在跑但未就绪(serving=false)";
            std::cout << "[health-check] 不健康: " << why << " addr=" << health_addr;
            if (!st.ok()) {
                std::cout << " code=" << static_cast<int>(st.error_code())
                          << " (" << st.error_message() << ")";
            }
            std::cout << std::endl;
            return code;
        }
    }

    // 初始化分级日志
    Logger::instance().init(app_cfg.log);
    CVLOG_INFO << "=== CVInfer-Gate 启动 ===";

    // 信号监听必须早于其它线程, 使其继承信号掩码
    LifecycleCoordinator lifecycle;
    SignalWatcher signal_watcher([&lifecycle](int sig) {
        CVLOG_WARN << "收到信号 " << sig << ", 开始优雅关闭...";
        lifecycle.requestShutdown();
    });
    if (!signal_watcher.start()) {
        CVLOG_ERROR << "信号监听启动失败";
        return -1;
    }

    // 数据库 (连接池 + 异步落库)
    // DBWriter::init() 已改为“连不上库也**不**失败”: 降级模式启动(记录先落
    // 本地 CSV), 后台按 database.reconnect_interval_ms 自动重建连接池, 恢复
    // 后回传; 启动只做 1 次快速连库尝试(不再白等 ~20s)。
    // 故这里**不再 return -1** —— 网关必须能“无库运行”。
    // 注: init() 现在恒返回 true, 本分支仅在极端异常时触发。
    DBWriter db_writer;
    if (!db_writer.init(app_cfg)) {
        CVLOG_ERROR << "数据库初始化异常! 将以【无落库】模式启动, 请检查 config.yaml。";
    }

    // 告警推送 (webhook)
    // 定位: 推送是"通知", 落库才是"账"。推送失败/队列满只计数 + 告警, 绝不影响落库与推理。
    alert::AlertNotifier alert_notifier;
    if (app_cfg.alert.push.enabled) {
        if (!alert_notifier.init(app_cfg.alert.push)) {
            CVLOG_WARN << "告警推送初始化失败(url=" << app_cfg.alert.push.url
                       << "), 降级为仅落库。";
        } else {
            CVLOG_INFO << "告警推送: 已启用 -> " << app_cfg.alert.push.url
                       << " (timeout=" << app_cfg.alert.push.timeout_ms << "ms, retries="
                       << app_cfg.alert.push.max_retries << ", queue="
                       << app_cfg.alert.push.max_queue << ")";
        }
    } else {
        CVLOG_INFO << "告警推送: 已禁用(仅落库; 需要时在 config 里开 alert.push.enabled)";
    }

    // 统一告警入口: "落库 + 推送 + 计数"三件事只在这一处发生
    // (此前 writeAlert 散在 sink 与复核回调里 => 新增通知渠道就得改多处, 漏一处就是静默丢通知)
    std::atomic<std::uint64_t> alerts_raised{0};
    std::atomic<std::uint64_t> det_total{0}; // 检测框总数(供 /metrics "拉"取)
    auto raise_alert = [&db_writer, &alert_notifier, &alerts_raised](
                           const std::string& type, const std::string& desc,
                           std::uint64_t frame_seq = 0, const std::string& label = "",
                           float confidence = 0.0f, int track_id = -1) {
        alerts_raised.fetch_add(1, std::memory_order_relaxed);
        db_writer.writeAlert(type, desc); // 账: 先落库(异步入队)
        alert::Alert a; // 通知: 尽力而为
        a.type = type;
        a.description = desc;
        a.frame_seq = frame_seq;
        a.label = label;
        a.confidence = confidence;
        a.track_id = track_id; // ts_ms 留给 AlertNotifier 盖戳(发送时刻)
        alert_notifier.push(a);
    };

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

    // 输出视频的容器帧率 = **抽帧后**的有效帧率, 否则回看会快放:
    // 源 30fps + frame_interval=3 -> sink 实际只拿到 10fps 的帧, 若容器仍写 30,
    // 视频就会 3 倍速播放(实测 RTSP 场景因丢了 65% 的帧, 写出了 8 倍速视频)。
    const int interval = app_cfg.video.frame_interval > 0 ? app_cfg.video.frame_interval : 1;
    double fps = source_fps / static_cast<double>(interval);
    if (app_cfg.video.target_fps > 0 && app_cfg.video.target_fps < fps) {
        fps = app_cfg.video.target_fps; // 限速比抽帧更严时, 以限速为准
    }
    if (fps <= 0.0) fps = 30.0;
    CVLOG_INFO << "视频帧率: 源=" << source_fps << "fps, frame_interval=" << interval
               << ", 输出容器=" << fps << "fps";

    // 3. 多模型注册 (Phase A 走单模型路径; 级联在后续阶段)
    // ModelPoolManager 按配置批量构建模型: 每个模型(如 YoloDetector)内部持有
    // 独立引擎池, 流水线 worker 与 gRPC 共享同一个 detector, 资源隔离/复用等价于旧版。
    ModelPoolManager model_manager;
    if (!model_manager.init(config_parser.getModelConfigs(), app_cfg.pipeline.worker_threads)) {
        CVLOG_ERROR << "模型初始化失败!";
        db_writer.stop();
        return -1;
    }
    // 选择检测器: 启用级联则用 CascadeEngine, 否则回退单模型。
    // 级联内部仍依赖 ModelPoolManager 中的模型(共享指针保证生命周期)。
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

    // 3.5 大模型异步复核 (可选; 复用 gRPC)
    // 仅作用于"告警"链路: 命中 review.trigger 的候选目标裁剪 ROI 后异步送审,
    // 复核确认后才写告警; 画框/写视频/检测入库完全不受影响。
    std::unique_ptr<ReviewScheduler> review_scheduler;
    if (app_cfg.review.enabled) {
        auto reviewer = std::make_shared<GrpcLlmReviewer>();
        if (!reviewer->init(app_cfg.review)) {
            CVLOG_WARN << "复核服务初始化失败(endpoint=" << app_cfg.review.endpoint
                       << "), 告警回退为本地规则。";
        } else {
            // 结果回调(在复核 worker 线程执行): 只做"写告警"(异步入队, 轻量)
            auto on_outcome = [&raise_alert, &app_cfg](const ReviewOutcome& out) {
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
                raise_alert(app_cfg.review.alert_type, desc, out.frame_seq, out.label,
                            out.confidence, -1);
            };

            review_scheduler = std::make_unique<ReviewScheduler>();
            if (!review_scheduler->init(app_cfg.review, reviewer, on_outcome)) {
                CVLOG_WARN << "复核调度器初始化失败, 告警回退为本地规则。";
                review_scheduler.reset();
            } else {
                // 把"送审的那张小图到底包含什么"写进日志 —— 占座/场景类复核
                // 出问题时, 这一行往往是第一个要看的(只裁物体 vs 裁场景)。
                CVLOG_INFO << "复核 ROI: padding=" << app_cfg.review.roi_padding
                           << " context_scale=" << app_cfg.review.roi_context_scale
                           << " min_side=" << app_cfg.review.roi_min_side
                           << " min_dwell=" << app_cfg.review.min_dwell_ms << "ms";
            }
        }
    }

    // 3.6 多模态决策级融合 (可选)
    // 不改动 VideoPipeline: 融合作为 sink **最前置阶段**就地执行 ——
    // poller 线程只做 read->入时间缓冲(有界丢最旧), fuse() 只做一次内存快照 +
    // 纯计算, 因此对流水线是"不阻塞"的。
    // 生命周期: video_sensor 必须先于 fusion_stage 声明(后者持有其裸指针),
    // 故析构顺序为 fusion_stage -> video_sensor, 不会悬空。
    std::unique_ptr<sensor::VideoSensorSource> video_sensor; // 视频时间基(非拥有底层)
    std::unique_ptr<MultiSensorPipeline> fusion_stage;
    if (app_cfg.fusion.enabled) {
        std::vector<std::shared_ptr<sensor::ISensorSource>> sensors;
        for (const auto& sc : app_cfg.sensors) {
            auto src = sensor::createSensorSource(sc); // 内部已完成 open()
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
            SensorConfig vcfg; // 视频源由 main 持有, 这里只借名(不重复 open)
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
    // RTSP 兼容: 某些网络流 open() 成功但拿不到分辨率(codec_ctx_->width==0),
    // 旧逻辑会把 0x0 交给 VideoWriter -> 打不开 -> 直接 return -1 退出,
    // 于是“流连上了却启不来服务”。现在改为**降级**: 尺寸未知时不写结果视频,
    // 推理/落库/告警/gRPC 全部照常。
    // 关键细节: 尺寸未知时**根本不要构造 VideoWriter** ——
    // 传 cv::Size(0,0) 会让 OpenCV 依次试 GStreamer/CV_IMAGES/FFMPEG 后端,
    // 每个后端都抛异常并打印 [ERROR:0] 噪声(实测: GStreamer 断言
    // “frameSize.width > 0” + CV_IMAGES “can't find starting number: output.avi”),
    // 还可能留下一个“已打开但写不出”的 0x0 writer。故改为延迟 open。
    const int video_width  = video_ok ? video_source->getWidth() : 0;
    const int video_height = video_ok ? video_source->getHeight() : 0;
    bool wrote_video = false; // 末尾据此决定是否打印“结果视频已保存”
    cv::VideoWriter video_writer; // 延迟 open(尺寸未知时保持关闭, 不触发后端探测)
    if (video_width > 0 && video_height > 0) {
        std::cout << "原视频分辨率: " << video_width << "x" << video_height << std::endl;
        video_writer.open(app_cfg.output_path,
                          cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                          fps, cv::Size(video_width, video_height));
        wrote_video = video_writer.isOpened();
        if (!wrote_video) {
            // 不再 return -1: 编码器不可用时降级为“不写结果视频”, 其余照常
            CVLOG_WARN << "无法初始化 VideoWriter(" << app_cfg.output_path
                       << "), 继续运行(仅不写结果视频)。";
        }
    } else if (video_ok) {
        CVLOG_WARN << "视频源未提供分辨率(source_type=" << app_cfg.video.source_type
                   << "), 本次不写结果视频(" << app_cfg.output_path << ")。";
    }
    // (!video_ok 时前面已 warn 过“视频源打开失败”, 此处不重复刷屏)

    // 4.5 告警去重闸门 (同标签 + 框重叠 + 冷却窗)
    // 告警判定是每帧执行的, 而"某目标疑似违规"描述的是目标状态 => 同一静止目标会被
    // 连续帧反复告警。闸门只作用于**告警链路**(送审处 + 写告警处), 画框/落库不受影响。
    // sink 回调会在多个 worker 线程并发执行, 故 AlertGate 内部自带 mutex
    // (已单测: 同目标并发调用只放行一次)。
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

    // 4.6 目标跟踪: 给每个目标一个跨帧稳定的 track_id。
    // 位置必须在 sink 内、**融合之后**: 跟踪的应是"最终参与告警的那批目标"。
    // 安全性: sink 是单线程, 且 的重排缓冲保证 frame_seq 单调递增 =>
    // 有状态的跟踪器在这里被顺序调用, 天然无并发。
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

    // 4.7 占座判定: 静态座位 zone + 几何关系 + 时序状态机
    //   位置: 与 tracker 一样放在 sink 内 —— 它**有状态**(累计时长/投票窗口),
    //   必须按 frame_seq 递增顺序调用(同一 sink 线程, 天然串行)。
    //   为什么不在"检测"层做: "属于哪个座位"(空间关系)与"持续了多久"(时序)
    //   都超出单帧检测器的表达能力, 而这两件事正是"占座"的定义;
    //   为什么不在 VLM 里做: 单帧小图既看不到座位全貌, 也答不了"持续多久"。
    std::unique_ptr<occupancy::SeatOccupancyAnalyzer> occupancy_analyzer;
    if (app_cfg.occupancy.enabled) {
        occupancy::Config oc_cfg;
        oc_cfg.enabled              = true;
        oc_cfg.item_labels          = app_cfg.occupancy.item_labels; // 空 => 组件内置默认
        oc_cfg.person_label         = app_cfg.occupancy.person_label;
        oc_cfg.item_seat_overlap    = app_cfg.occupancy.item_seat_overlap;
        oc_cfg.item_person_overlap  = app_cfg.occupancy.item_person_overlap;
        oc_cfg.person_seat_iou      = app_cfg.occupancy.person_seat_iou;
        oc_cfg.min_person_height_px = app_cfg.occupancy.min_person_height_px;
        oc_cfg.t_occupied_ms        = app_cfg.occupancy.t_occupied_ms;
        oc_cfg.t_grace_ms           = app_cfg.occupancy.t_grace_ms;
        oc_cfg.vote_n               = app_cfg.occupancy.vote_n;
        oc_cfg.vote_m               = app_cfg.occupancy.vote_m;

        occupancy_analyzer = std::make_unique<occupancy::SeatOccupancyAnalyzer>(
            oc_cfg, buildSeatZones(app_cfg.occupancy.seats));

        const auto& eff = occupancy_analyzer->config();
        CVLOG_INFO << "占座判定: 已启用 (座位数=" << occupancy_analyzer->seats().size()
                   << ", 物品类别=[" << joinStrings(eff.item_labels) << "]"
                   << ", t_occupied=" << eff.t_occupied_ms / 1000 << "s"
                   << ", t_grace=" << eff.t_grace_ms / 1000 << "s"
                   << ", vote=" << eff.vote_m << "/" << eff.vote_n << ")";
        for (const auto& z : occupancy_analyzer->seats()) {
            const cv::Rect r = z.boundingRect();
            CVLOG_INFO << "  座位 [" << z.name << "] 区域=" << r.width << "x" << r.height
                       << "@(" << r.x << "," << r.y << ") 顶点数=" << z.polygon.size();
            // 防呆: 座位区超出画面 ⇒ 判定基本失效。这是**换视频/换机位后最容易踩的坑**:
            // 坐标是按旧分辨率标的, 换了源就成了空转。
            // 为什么单靠下游那句"ROI 无效"告警拦不住: 那句只在座位区**完全**落在画面外时
            // 才触发; 部分超出时 ROI 仍非空(被裁到画面内), 于是静默地照常判定 ——
            // 实测 720x1280 -> 544x592 就是这么踩的(78% 面积在画面外, 却毫无提示)。
            if (video_width > 0 && video_height > 0 &&
                (r.x < 0 || r.y < 0 || r.x + r.width > video_width ||
                 r.y + r.height > video_height)) {
                const int vis_w = std::max(0, std::min(r.x + r.width, video_width) - std::max(r.x, 0));
                const int vis_h = std::max(0, std::min(r.y + r.height, video_height) - std::max(r.y, 0));
                const double vis = r.area() > 0
                    ? static_cast<double>(vis_w) * vis_h / static_cast<double>(r.area())
                    : 0.0;
                CVLOG_WARN << "  座位 [" << z.name << "] 区域超出画面 " << video_width << "x"
                           << video_height << ", 仅 " << static_cast<int>(vis * 100.0 + 0.5)
                           << "% 面积可见 —— 占座判定基本失效, 请按当前分辨率重标 occupancy.seats";
            }
        }
        // 交互提醒: 物品类标签已改由占座事件产生告警, 逐物体送审对它们自动失效。
        // 若这不符合预期(比如既想要"书"告警又想要"占座"告警), 应把该标签从
        // occupancy.item_labels 里拿掉, 或关闭 occupancy.enabled。
        CVLOG_INFO << "占座判定: 标签 [" << joinStrings(eff.item_labels)
                   << "] 的逐物体送审已抑制, 改由占座事件产生告警;"
                   << " 告警类型沿用 review.alert_type=" << app_cfg.review.alert_type;
    } else {
        CVLOG_INFO << "占座判定: 已禁用(occupancy.enabled=false)";
    }

    // 5. 构造三阶段流水线 (sink 回调负责画框/写视频/异步落库)
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
            // (0) 多模态决策级融合: sink 最前置, 就地融合(不阻塞)。
            // 启用时在**拷贝**上改写(回调入参是 const, 且下游都应看到融合值);
            // 未启用时零拷贝, 直接引用原入参。
            std::vector<DetectionResult> fused_dets;
            const std::vector<DetectionResult>* dets_ptr = &detections;
            if (fusion_stage) {
                fused_dets = detections;
                fusion_stage->fuse(fused_dets);
                dets_ptr = &fused_dets;
            }

            // (0.5) 目标跟踪: 就地回写 track_id(未启用时零拷贝)。
            // 跟踪**融合之后**的最终集合 => 传感器补出的目标也能拿到 id。
            std::vector<DetectionResult> tracked_dets;
            if (tracker) {
                tracked_dets = *dets_ptr;
                tracker->update(tracked_dets, nowMs(), frame_seq);
                dets_ptr = &tracked_dets;
            }
            const std::vector<DetectionResult>& dets = *dets_ptr;

            // (0.8) 占座判定: 每帧都要跑(累计时长靠它推进), **必须在"无检测早退"
            // 之前** —— "本帧什么都没检出"恰恰是状态机需要的输入(物品被收走)。
            // 本帧"刚刚判占座"的座位才返回事件(上升沿), 不会每帧刷。
            std::vector<occupancy::OccupancyEvent> occ_events;
            if (occupancy_analyzer) {
                occ_events = occupancy_analyzer->update(dets, nowMs());
            }

            // (1) 画框 / 写视频: 用本地(主筛+二级[+融合])结果, 实时输出, 不等复核
            if (video_writer.isOpened()) {
                cv::Mat annotated = frame.clone();
                // 线宽/字号走配置(video.box_thickness / video.label_scale):
                // 同一个值在不同分辨率的源上观感差很多, 写死必然有一边难受。
                const int box_thickness = app_cfg.video.box_thickness;
                const double label_scale = app_cfg.video.label_scale;
                for (const auto& det : dets) {
                    if (box_thickness > 0) {
                        cv::rectangle(annotated, det.box, cv::Scalar(0, 255, 0), box_thickness);
                    }
                    // 带上 track_id: 肉眼可验证"同一个人是否只有一个 id"
                    if (label_scale > 0.0) {
                        const std::string text =
                            (det.track_id >= 0 ? "#" + std::to_string(det.track_id) + " " : "") +
                            det.label + " " +
                            std::to_string(static_cast<int>(det.confidence * 100)) + "%";
                        cv::putText(annotated, text, cv::Point(det.box.x, det.box.y - 5),
                                    cv::FONT_HERSHEY_SIMPLEX, label_scale,
                                    cv::Scalar(0, 255, 0), 1);
                    }
                }
                video_writer.write(annotated);
            }
            if (dets.empty()) return;

            // (2) 检测入库: 保持现状(原始记录立即落库, 不被复核/融合阻塞)
            det_total.fetch_add(dets.size(), std::memory_order_relaxed);
            db_writer.writeDetections(dets);

            // (3) 告警:
            // 启用复核 -> 候选目标裁剪 ROI 后异步送审, 确认后由 on_outcome 写告警;
            // 未启用   -> 保持原本地规则(person>0.8 立即告警)。
            const bool review_on = (review_scheduler && review_scheduler->enabled());
            for (const auto& det : dets) {
                // 占座模式下的分流: 标签命中 occupancy.item_labels 的目标**不再**走
                // "逐物体送审/告警" —— "桌上有一本书"本身不是问题, "书在桌上而人
                // 已离开 5 分钟"才是。两条路径都发会让同一个场景报两次, 而且白烧
                // VLM 调用。看人不看物(物品只是判占座的**证据**)。
                if (occupancy_analyzer &&
                    occupancy::SeatOccupancyAnalyzer::isItemLabel(
                        det.label, occupancy_analyzer->config().item_labels)) {
                    continue;
                }
                const bool candidate =
                    review_on ? isReviewCandidate(det, app_cfg.review)
                              : (det.label == "person" && det.confidence > 0.8f);
                if (!candidate) continue;

                // 去重打点:
                // 复核路径打在**送审处** —— 同一目标只送审一次, 自然不可能重复告警,
                // 而且省掉重复的 VLM 调用(复核回调拿不到框, 无法在那里去重);
                // 本地规则路径打在**写告警处**。
                // 有 track_id 时闸门**以身份为准**(与框怎么移动无关)。
                if (review_on) {
                    // [占座] 时序门: 只送审"已在画面里停留够久"的目标。
                    // 占座 = 长期占用 => 刚出现/刚建轨的物品先观望(省算力 + 减误报)。
                    // 拿不到 track_id(未开跟踪 / 轨迹尚未确认)时不拦, 保持宽容。
                    if (app_cfg.review.min_dwell_ms > 0 && tracker) {
                        const auto* tr =
                            (det.track_id >= 0) ? tracker->find(det.track_id) : nullptr;
                        if (tr && tr->dwellMs() < app_cfg.review.min_dwell_ms) {
                            CVLOG_DEBUG << "[review] 停留不足 " << tr->dwellMs() << "ms < "
                                        << app_cfg.review.min_dwell_ms << "ms, 暂不送审: "
                                        << det.label << " frame=" << frame_seq;
                            continue;
                        }
                    }
                    ReviewRequest job;
                    job.frame_seq = frame_seq;
                    // 场景 ROI: 不再只裁"物体本身"(那样大模型看不到桌面/座位/周围的人),
                    // 而是以目标为中心扩大一块"物体所在场景"(见 ConfigParser 注释)。
                    job.roi = roi_utils::cropContext(frame, det.box,
                                                     app_cfg.review.roi_padding,
                                                     app_cfg.review.roi_context_scale,
                                                     app_cfg.review.roi_min_side);
                    job.class_id = det.class_id;
                    job.label = det.label;
                    job.confidence = det.confidence;
                    job.prompt = app_cfg.review.prompt;

                    if (job.roi.empty()) {
                        // ROI 无效无法送审: 按兜底策略处理(需要告警时同样过闸门)
                        if (!app_cfg.review.alert_on_failure) continue;
                        if (!alert_gate.allow(det.label, det.track_id, det.box, nowMs())) continue;
                        raise_alert(app_cfg.review.alert_type,
                                    "ROI 无效, 按兜底策略告警, frame=" +
                                        std::to_string(frame_seq),
                                    frame_seq, det.label, det.confidence, det.track_id);
                        continue;
                    }
                    if (!alert_gate.allow(det.label, det.track_id, det.box, nowMs())) {
                        CVLOG_DEBUG << "冷却窗内同目标重复, 跳过送审: " << det.label
                                    << " frame=" << frame_seq;
                        continue;
                    }
                    review_scheduler->submit(std::move(job));
                } else {
                    if (!alert_gate.allow(det.label, det.track_id, det.box, nowMs())) {
                        CVLOG_DEBUG << "冷却窗内同目标重复, 跳过告警: " << det.label
                                    << " frame=" << frame_seq;
                        continue;
                    }
                    // 业务不写死: 告警类型取配置(原先硬编码, 换业务会串味)
                    raise_alert(app_cfg.review.alert_type,
                                "检测到 " + det.label + ", 置信度: " +
                                    std::to_string(det.confidence),
                                frame_seq, det.label, det.confidence, det.track_id);
                }
            }

            // (4) 占座事件 -> 送审 / 告警
            // 规则层已经完成"筛"(哪个座位 / 持续多久 / 证据是什么), VLM 只做"判"。
            // 送审提示词里带上结构化证据 => 模型回答的是"这算不算占座", 而不是
            // "这张图里有什么"(后者正是 VLM 答不好的那些问题)。
            for (const auto& ev : occ_events) {
                // 座位是静态的 => 用座位框做几何去重最精确(与 track_id 无关):
                // "同一个座位在冷却窗内只报一次"正是这里想要的语义。
                if (!alert_gate.allow(kOccupancyLabel, ev.box, nowMs())) {
                    CVLOG_DEBUG << "[占座] 座位 " << ev.seat << " 在冷却窗内, 不重复告警";
                    continue;
                }
                const std::string desc = ev.evidence.describe();
                CVLOG_INFO << "[占座] " << desc;

                if (!review_on) {
                    // 未启用复核: 规则判定即结论, 直接告警(类型沿用 review.alert_type)
                    raise_alert(app_cfg.review.alert_type, desc, frame_seq, kOccupancyLabel,
                                ev.evidence.confidence(), -1);
                    continue;
                }

                ReviewRequest job;
                job.frame_seq = frame_seq;
                // 座位 zone 本身就是"场景"(人为画定的语义区域), 故不再做中心放大:
                // 想要更大视野就把 zone 画大一点 —— "所见即所判", 不引入隐藏的缩放。
                job.roi = roi_utils::crop(frame, ev.box, 0.0f);
                job.class_id = 0;
                job.label = kOccupancyLabel;
                job.confidence = ev.evidence.confidence();
                job.prompt = app_cfg.review.prompt;
                if (!job.prompt.empty()) job.prompt += "\n";
                job.prompt += "【规则引擎证据】" + desc;

                if (job.roi.empty()) {
                    // 座位区完全落在画面外(配置画错) => 按兜底策略处理
                    CVLOG_WARN << "[占座] 座位 " << ev.seat
                               << " 的 ROI 无效(座位区在画面外? 请核对 occupancy.seats), 证据: "
                               << desc;
                    if (app_cfg.review.alert_on_failure) {
                        raise_alert(app_cfg.review.alert_type, desc, frame_seq, kOccupancyLabel,
                                    ev.evidence.confidence(), -1);
                    }
                    continue;
                }
                review_scheduler->submit(std::move(job));
            }
        });
            
    // 6. 启动 gRPC 服务 (独立线程, 与流水线并行; 旧版是视频跑完才启动)
    // 鉴权: token 取自 grpc.auth_token(建议写 "${GRPC_AUTH_TOKEN:-}"; 空 = 不鉴权)
    DetectionServiceImpl service(*detector, app_cfg.grpc.auth_token, CV_GATE_VERSION, start_ms);
    // 按 GrpcConfig 配置消息大小/线程/keepalive 并启动服务
    std::string server_address;
    std::unique_ptr<grpc::Server> server =
        buildAndStartGrpcServer(app_cfg.grpc, service, server_address);
    if (!server) {
        CVLOG_ERROR << "gRPC 服务启动失败, 端口: " << app_cfg.grpc.port;
        db_writer.stop();
        return -1;
    }
    CVLOG_INFO << "gRPC 服务已启动, 监听: " << server_address;

    // 指标端点 (/metrics + /healthz)
    // 口径: 帧级数据"拉"(直接读各处已有的 Stats 原子量, 不在热路径上加锁);
    // 离散事件"推"(gRPC 计数在 service 内部自带)。
    // enabled=false 时不监听任何端口(零行为变化)。
    metrics::HttpServer metrics_server;
    if (app_cfg.metrics.enabled) {
        auto& reg = metrics::Registry::instance();
        const std::string ver_label = std::string("version=\"") + CV_GATE_VERSION + "\"";
        reg.declare("cvinfer_build_info", metrics::Type::Gauge, "构建信息(恒为 1)", ver_label);
        reg.setGauge("cvinfer_build_info", ver_label, 1.0);

        reg.addCollector("cvinfer_uptime_seconds", metrics::Type::Gauge, "进程已运行时长(秒)",
                         [start_ms] { return static_cast<double>(epochMs() - start_ms) / 1000.0; });
        reg.addCollector("cvinfer_frames_decoded_total", metrics::Type::Counter,
                         "已解码帧数(抽帧前)",
                         [&pipeline] { return static_cast<double>(pipeline.stats().decoded); });
        reg.addCollector("cvinfer_frames_dropped_total", metrics::Type::Counter,
                         "因背压丢弃的帧数(队列满)",
                         [&pipeline] { return static_cast<double>(pipeline.stats().dropped); });
        reg.addCollector("cvinfer_frames_processed_total", metrics::Type::Counter,
                         "已完成推理的帧数",
                         [&pipeline] { return static_cast<double>(pipeline.stats().processed); });
        reg.addCollector("cvinfer_frames_emitted_total", metrics::Type::Counter,
                         "已交给 sink 的帧数",
                         [&pipeline] { return static_cast<double>(pipeline.stats().emitted); });
        reg.addCollector("cvinfer_detections_total", metrics::Type::Counter,
                         "产生的检测框总数",
                         [&det_total] { return static_cast<double>(det_total.load()); });
        reg.addCollector("cvinfer_alerts_raised_total", metrics::Type::Counter,
                         "产生的告警条数(不管是否推送成功)",
                         [&alerts_raised] { return static_cast<double>(alerts_raised.load()); });
        reg.addCollector("cvinfer_alerts_suppressed_total", metrics::Type::Counter,
                         "被去重闸门抑制的重复告警数",
                         [&alert_gate] { return static_cast<double>(alert_gate.suppressed()); });
        reg.addCollector("cvinfer_db_healthy", metrics::Type::Gauge,
                         "数据库健康(1=正常写库, 0=降级到本地 CSV)",
                         [&db_writer] { return db_writer.dbHealthy() ? 1.0 : 0.0; });
        reg.addCollector("cvinfer_db_reconnects_total", metrics::Type::Counter,
                         "数据库不可用->恢复的累计次数",
                         [&db_writer] { return static_cast<double>(db_writer.dbReconnects()); });
        reg.addCollector("cvinfer_alert_push_sent_total", metrics::Type::Counter,
                         "webhook 推送成功数",
                         [&alert_notifier] { return static_cast<double>(alert_notifier.stats().sent); });
        reg.addCollector("cvinfer_alert_push_failed_total", metrics::Type::Counter,
                         "webhook 推送最终失败数(重试耗尽)",
                         [&alert_notifier] { return static_cast<double>(alert_notifier.stats().failed); });
        reg.addCollector("cvinfer_alert_push_dropped_total", metrics::Type::Counter,
                         "webhook 推送因队列满而丢弃的告警数",
                         [&alert_notifier] { return static_cast<double>(alert_notifier.stats().dropped); });
        if (review_scheduler) {
            reg.addCollector("cvinfer_review_submitted_total", metrics::Type::Counter,
                             "送大模型复核的任务数",
                             [&review_scheduler] { return static_cast<double>(review_scheduler->stats().submitted); });
            reg.addCollector("cvinfer_review_reviewed_total", metrics::Type::Counter,
                             "已完成复核的任务数",
                             [&review_scheduler] { return static_cast<double>(review_scheduler->stats().reviewed); });
            reg.addCollector("cvinfer_review_confirmed_total", metrics::Type::Counter,
                             "复核确认(告警)数",
                             [&review_scheduler] { return static_cast<double>(review_scheduler->stats().confirmed); });
            reg.addCollector("cvinfer_review_unavailable_total", metrics::Type::Counter,
                             "复核服务不可用次数(超时/连接失败)",
                             [&review_scheduler] { return static_cast<double>(review_scheduler->stats().unavailable); });
        }
        if (tracker) {
            reg.addCollector("cvinfer_tracks_active", metrics::Type::Gauge, "当前活跃轨迹数",
                             [&tracker] { return static_cast<double>(tracker->stats().active); });
        }
        if (occupancy_analyzer) {
            reg.addCollector("cvinfer_occupancy_events_total", metrics::Type::Counter,
                             "判定为占座的次数(座位级, 上升沿)",
                             [&occupancy_analyzer] {
                                 return static_cast<double>(occupancy_analyzer->stats().events);
                             });
        }

        metrics::HttpServer::Config mcfg;
        mcfg.enabled = true;
        mcfg.bind = app_cfg.metrics.bind;
        mcfg.port = app_cfg.metrics.port;
        if (!metrics_server.start(mcfg, [] { return metrics::Registry::instance().render(); })) {
            CVLOG_WARN << "指标端点启动失败(端口 " << app_cfg.metrics.port
                       << " 被占用?), 继续运行(仅无 /metrics)。";
        }
    } else {
        CVLOG_INFO << "指标端点: 已禁用(metrics.enabled=false)";
    }

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
    
    // 9. 优雅关闭 (顺序: gRPC -> 指标 -> 流水线 -> 告警推送 -> 数据库)
    CVLOG_INFO << "正在关闭服务...";
    if (server) server->Shutdown();
    if (grpc_thread.joinable()) grpc_thread.join();

    // 指标端点必须最先停: 之后的析构会让 collector 们持有的对象逐个消失, 不能再被抓取
    metrics_server.stop();

    pipeline.stop();
    const VideoPipeline::Stats st = pipeline.stats();
    CVLOG_INFO << "流水线统计: decoded=" << st.decoded << " dropped=" << st.dropped
               << " processed=" << st.processed << " emitted=" << st.emitted;

    // 级联统计(若处于级联模式): 主筛/触发/确认/否决/降级
    if (auto* cascade = dynamic_cast<CascadeEngine*>(detector.get())) {
        const CascadeEngine::Stats cs = cascade->stats();
        CVLOG_INFO << "级联统计: primary=" << cs.primary << " triggered=" << cs.triggered
                   << " confirmed=" << cs.confirmed << " rejected=" << cs.rejected
                   << " skipped=" << cs.skipped;
    }

    // 复核调度器: 停机并排空在途复核(结果回调会写告警),
    // 必须在 db_writer.flush()/stop() 之前完成, 否则告警可能丢失。
    if (review_scheduler) {
        review_scheduler->stop();
        const ReviewScheduler::Stats rs = review_scheduler->stats();
        CVLOG_INFO << "复核统计: submitted=" << rs.submitted << " dropped=" << rs.dropped
                   << " reviewed=" << rs.reviewed << " confirmed=" << rs.confirmed
                   << " rejected=" << rs.rejected << " timeout=" << rs.timeout
                   << " unavailable=" << rs.unavailable << " failed=" << rs.failed;
    }

    // 多模态融合: 停机并打印统计。必须晚于 pipeline.stop() —— fuse() 由 sink 调用。
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

    // 告警去重统计: suppressed 越大说明去重越在干活(告警刷屏被压住)
    // 注意: 复核路径是“确认后才告警”, 故这里的 allowed 包含“已放行送审”的次数,
    // 不等于最终告警条数(最终条数看数据库)。
    CVLOG_INFO << "告警去重统计: allowed=" << alert_gate.allowed()
               << " suppressed=" << alert_gate.suppressed()
               << " tracked=" << alert_gate.tracked();

    // 跟踪统计: spawned/retired 看目标进出, longest_dwell 是行为分析的雏形
    if (tracker) {
        const auto ts = tracker->stats();
        CVLOG_INFO << "目标跟踪统计: frames=" << ts.frames
                   << " spawned=" << ts.spawned
                   << " retired=" << ts.retired
                   << " active=" << ts.active
                   << " matched=" << ts.matched
                   << " longest_dwell=" << tracker->longestDwellMs() << "ms";
    }

    // 占座统计: 每个座位最后的状态 —— 运维最常问的就是"现在哪些座位被判占了"
    if (occupancy_analyzer) {
        const auto os = occupancy_analyzer->stats();
        CVLOG_INFO << "占座统计: frames=" << os.frames << " seats=" << os.seats
                   << " occupied_events=" << os.events;
        for (const auto& ev : occupancy_analyzer->snapshot()) {
            CVLOG_INFO << "  [占座] " << ev.describe();
        }
    }

    // 告警推送: 排空队列(有超时)后再关库;
    // 顺序很关键 —— 复核回调也会 raise_alert, 故本行晚于 review_scheduler->stop()。
    {
        const auto ps = alert_notifier.stats();
        alert_notifier.stop();
        CVLOG_INFO << "告警推送统计: pushed=" << ps.pushed << " sent=" << ps.sent
                   << " failed=" << ps.failed << " dropped=" << ps.dropped
                   << " retried=" << ps.retried
                   << (ps.failed > 0 ? (" last_error=" + ps.last_error) : std::string());
    }

    db_writer.flush();
    db_writer.stop();

    video_writer.release();
    video_source->close();
    // 只有真的写过结果视频才宣告(降级运行时不误导)
    if (wrote_video) CVLOG_INFO << "结果视频已保存至 " << app_cfg.output_path;
    CVLOG_INFO << "日志轮转次数: " << Logger::instance().rotations();
    CVLOG_INFO << "已安全退出。";
    signal_watcher.stop();

    return 0;
}