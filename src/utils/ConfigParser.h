#pragma once

#include <string>
#include <cstddef>
#include <iostream>
#include <vector>
#include <yaml-cpp/yaml.h>

// ============================================================
// 系统运行配置 (T1: 扁平结构 -> 按业务域分组的嵌套结构)
// ------------------------------------------------------------
// 原 AppConfig 为扁平字段 (video_source_type / db_host / grpc_port ...),
// 现拆分到 Video/Database/Grpc/Log/Pipeline 各子结构, 便于
// 扩展、传参与校验。所有字段均带默认值, 未配置时行为可预测。
// ============================================================

// 日志配置  [新增]
struct LogConfig {
    std::string level = "info";     // trace|debug|info|warn|error
    std::string file;               // 空 = 仅输出到控制台
};

// 帧队列防爆配置  [新增]
struct QueueConfig {
    std::size_t max_size = 24;      // 队列容量上限(有界)
    std::string policy = "drop_oldest";  // drop_oldest | block
};

// 视频源配置
struct VideoConfig {
    std::string source_type = "file";    // file | rtsp
    std::string source_path;             // 视频文件 / RTSP 地址
    int target_fps = 0;                  // [新增] 抽帧目标帧率, 0 = 不限速
    int frame_interval = 1;              // [新增] 每 N 帧取 1 帧
    QueueConfig queue;                   // [新增]
};

// 流水线配置  [新增]
struct PipelineConfig {
    int worker_threads = 2;              // 推理工作线程数 / 引擎池大小
};

// 数据库配置
struct DatabaseConfig {
    std::string host = "tcp://127.0.0.1:3306";
    std::string user = "root";
    std::string password;
    std::string dbname = "cv_infer";
    int pool_size = 4;                   // [新增] 连接池大小
    int batch_size = 20;                 // [新增/T11] 攒够多少条批量写入
    int flush_interval_ms = 1000;        // [新增/T11] 或每隔多久批量冲刷(ms)
    int max_retries = 10;                // [新增] 初次连接最大重试次数
    // [T11] 数据库降级 / 重试策略
    std::string fallback_path = "db_fallback.csv";  // 不可用时本地降级 CSV(空=禁用)
    int reconnect_interval_ms = 30000;               // 不可用后重试间隔(ms)
    int reconnect_after_writes = 100;                // 或累计冲刷次数后重试
};

// gRPC 配置
struct GrpcConfig {
    int port = 50051;
    int timeout_ms = 5000;               // [新增] RPC 超时时间
    int max_message_size_mb = 16;        // [新增] 单条消息上限
    int worker_threads = 0;              // [新增] 0 = gRPC 默认
    int keepalive_time_ms = 20000;       // [新增] keepalive 时间
};

// 级联配置  [新增/T16-T19]
// ------------------------------------------------------------
// 控制"主模型灰区 -> 二级分类器复核"的级联行为。所有字段带默认值;
// 未配置 cascade 段时 enabled=false, 退化为 T15 的单模型路径。
// 该结构同时被 ConfigParser(解析) 与 CascadeEngine(运行) 使用。
struct CascadeConfig {
    bool enabled = false;                    // 是否启用级联
    std::string primary;                     // 主筛模型名(空 = 首个 detector)
    std::string secondary;                   // 二级分类器模型名(空 = 退化为单模型)
    // ---- 触发条件(灰区) ----
    std::vector<std::string> trigger_labels; // 触发复核的类别(空 = 所有类别)
    float min_conf = 0.0f;                   // 灰区下界(含)
    float max_conf = 1.0f;                   // 灰区上界(不含)
    float roi_padding = 0.10f;               // ROI 外扩比例(相对目标宽高)
    // ---- 决策规则 ----
    std::string accept_label;                // 视为"通过"的二级标签(空 = 仅看置信度)
    float accept_conf = 0.50f;               // 通过所需的最低二级置信度
    bool drop_rejected = true;               // 二级否决时是否丢弃该目标
    bool boost_on_confirm = false;           // 确认时是否用二级置信度提升主置信度
};

// 大模型异步复核配置  [新增/T20-T22]
// ------------------------------------------------------------
// 控制"告警候选 -> 异步大模型复核 -> 确认后告警"的行为。默认 enabled=false,
// 未启用时告警走原有本地规则(person>0.8), 行为不变。
struct ReviewConfig {
    bool enabled = false;                    // 是否启用大模型复核
    std::string endpoint = "llm:50052";      // 复核服务 gRPC 地址(host:port)
    int timeout_ms = 3000;                   // 单次复核超时(deadline)
    std::size_t queue_size = 128;            // 有界队列容量(满则丢最旧)
    int worker_threads = 2;                  // 复核并发线程数
    // ---- 触发条件(告警候选) ----
    std::vector<std::string> trigger_labels; // 触发复核的类别(空 = 所有类别)
    float min_conf = 0.5f;                   // 触发下界(含)
    float max_conf = 1.0f;                   // 触发上界(不含)
    float roi_padding = 0.10f;               // ROI 外扩比例(相对目标宽高)
    // ---- 业务/策略 ----
    std::string prompt;                      // 透传给复核服务的业务提示(可空)
    std::string alert_type = "安全帽缺失";    // 确认后写入的告警类型
    bool alert_on_failure = false;           // 复核不可用(超时/失败)时是否仍告警
};

// 非视频传感器配置  [新增/T23-T24]
// ------------------------------------------------------------
// 一条 sensors: 记录描述一路**非视频**传感器(雷达/红外)。视频不走这里
// (视频由 video: 段描述, 用 VideoSensorSource 适配)。
// 与 CascadeConfig/ReviewConfig 一样放在 utils: 解析器与运行期(sensor:)
// 共用同一份定义, 避免 utils 反向依赖 sensor 模块。
struct SensorConfig {
    std::string kind = "radar";      // video | radar | infrared
    std::string name;                // 唯一名(日志/统计); 空 = 用 kind
    std::string backend = "stub";    // file(回放) | stub(合成); 骨架实现
    std::string path;                // backend=file 时的回放文件
    int rate_hz = 10;                // backend=stub 的合成速率
    std::vector<std::string> labels; // 标签白名单(空 = 不过滤)
};

// 多模态融合配置  [新增/T25-T26]
// ------------------------------------------------------------
// 默认 enabled=false => 完全不启用融合, 行为与 T22 一致(纯视觉)。
struct FusionConfig {
    bool enabled = false;                // 是否启用多模态融合
    std::string level = "decision";      // 融合层级; 目前仅支持 decision(决策级)
    int time_tolerance_ms = 50;          // 时间对齐容差窗口(±, ms)
    float match_iou = 0.30f;             // 空间关联最小 IoU(传感器无框时退化为标签关联)
    float sensor_weight = 0.35f;         // 加权融合中传感器权重 [0,1]
    bool emit_sensor_only = false;       // 未关联的传感器目标是否作为新目标输出(需其自带框)
    bool adopt_sensor_label = false;     // 关联后是否采用传感器标签
    std::size_t buffer_capacity = 256;   // 采样时间缓冲容量(满则丢最旧)
};

// 系统运行配置(聚合)
struct AppConfig {
    LogConfig log;                       // [新增]
    VideoConfig video;
    PipelineConfig pipeline;             // [新增]
    DatabaseConfig database;
    GrpcConfig grpc;
    CascadeConfig cascade;               // [新增/T16-T19]
    ReviewConfig review;                 // [新增/T20-T22]
    std::vector<SensorConfig> sensors;   // [新增/T23-T24] 非视频传感器列表
    FusionConfig fusion;                 // [新增/T25-T26] 多模态决策级融合
};

// 模型推理配置
// [T12/T14] 由"单模型"扩展为"可命名 + 带角色"的模型描述, 以支持多模型注册;
//          新增字段均带默认值, 旧的单模型配置解析路径保持兼容。
struct ModelConfig {
    std::string name = "yolov8_detector";  // [T12/T14] 模型名(多模型注册/查找用)
    std::string role = "detector";          // [T12/T14] detector | classifier | reviewer
    std::string model_xml_path;
    std::string model_bin_path;
    std::string labels_path;
    int input_width = 640;
    int input_height = 640;
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;
    int pool_size = 0;                      // [T15] 该模型引擎池大小; 0 = 由 ModelPoolManager 按 worker_threads 兜底
    int acquire_timeout_ms = 2000;          // [T15] 借引擎超时(ms)

    // ---- [T30] 推理性能旋钮(可选; 默认值 = 改造前行为, 不向 OpenVINO 设任何属性) ----
    // 背景: 原先硬编码 core.compile_model(model, "AUTO") + 默认性能模式(LATENCY)。
    //   当 pipeline.worker_threads = N (>1) 时, N 个引擎各自按 LATENCY 开满物理核
    //   -> 线程超订互相抢核, 总吐吐反而暴跌(实测 RTSP 场景仅 3.6fps)。
    std::string device = "AUTO";            // AUTO | CPU | GPU | GPU.0 | NPU ...
    std::string perf_mode;                  // 空=不设置(LATENCY) | latency | throughput
    int num_threads = 0;                    // 0=不设置(默认=物理核数); >0=推理线程数
};

class ConfigParser {
public:
    ConfigParser() = default;
    ~ConfigParser() = default;

    // 加载配置文件
    bool loadAppConfig(const std::string& filepath);
    bool loadModelConfig(const std::string& filepath);

    // 获取配置的常量引用
    const AppConfig& getAppConfig() const { return app_config_; }
    // [T14] 主模型(首个); 保持旧接口兼容
    const ModelConfig& getModelConfig() const { return model_config_; }
    // [T14] 全部模型(单模型配置会被规整为长度 1 的列表)
    const std::vector<ModelConfig>& getModelConfigs() const { return model_configs_; }

private:
    // [新增] 配置合法性校验; 加载成功后自动调用
    bool validate() const;
    // [T14] 模型配置校验(角色/阈值/重名等)
    bool validateModelConfigs() const;

    AppConfig app_config_;
    ModelConfig model_config_;
    std::vector<ModelConfig> model_configs_;   // [T14]
};