#pragma once

#include <string>
#include <cstddef>
#include <iostream>
#include <vector>
#include <yaml-cpp/yaml.h>

// 系统运行配置 (扁平结构 -> 按业务域分组的嵌套结构)
// 原 AppConfig 为扁平字段 (video_source_type / db_host / grpc_port ...),
// 现拆分到 Video/Database/Grpc/Log/Pipeline 各子结构, 便于
// 扩展、传参与校验。所有字段均带默认值, 未配置时行为可预测。

// 日志配置
struct LogConfig {
    std::string level = "info"; // trace|debug|info|warn|error
    std::string file; // 空 = 仅输出到控制台
    // 文件轮转(仅在 file 非空且 max_size_mb > 0 时生效)
    int max_size_mb = 0; // 0 = 不轮转(一直追加); >0 = 单文件超过即轮转
    int keep_files = 3; // 保留的历史文件数(app.log.1 ... app.log.N); 0 = 不保留历史
};

// 帧队列防爆配置
struct QueueConfig {
    std::size_t max_size = 24; // 队列容量上限(有界)
    std::string policy = "drop_oldest"; // drop_oldest | block
};

// 视频源配置
struct VideoConfig {
    std::string source_type = "file"; // file | rtsp
    std::string source_path; // 视频文件 / RTSP 地址
    int target_fps = 0; // 抽帧目标帧率, 0 = 不限速
    int frame_interval = 1; // 每 N 帧取 1 帧
    // 结果视频上的可视化(纯观感旋钮, 不影响判定/落库/告警)
    // 由来: 线宽原来写死 2px —— 在 720x1280 上还行, 换到 544x592 这种小分辨率源
    // 就显得又粗又糊(MJPG 编码还会把 2px 糊成 ~3px), 密集框下很挡目标。
    int box_thickness = 1; // 框线宽(px); <=0 = 不画框
    double label_scale = 0.45; // 框上标签字号; <=0 = 不写字
    QueueConfig queue;
};

// 流水线配置
struct PipelineConfig {
    int worker_threads = 2; // 推理工作线程数 / 引擎池大小
};

// 数据库配置
struct DatabaseConfig {
    std::string host = "tcp://127.0.0.1:3306";
    std::string user = "root";
    std::string password;
    std::string dbname = "cv_infer";
    int pool_size = 4; // 连接池大小
    int batch_size = 20; // 攒够多少条批量写入
    int flush_interval_ms = 1000; // 或每隔多久批量冲刷(ms)
    int max_retries = 10; // 初次连接最大重试次数
    // 数据库降级 / 重试策略
    std::string fallback_path = "db_fallback.csv"; // 不可用时本地降级 CSV(空=禁用)
    int reconnect_interval_ms = 30000; // 不可用后重试间隔(ms)
    int reconnect_after_writes = 100; // 或累计冲刷次数后重试
};

// gRPC 配置
struct GrpcConfig {
    int port = 50051;
    int timeout_ms = 5000; // RPC 超时时间
    int max_message_size_mb = 16; // 单条消息上限
    int worker_threads = 0; // 0 = gRPC 默认
    int keepalive_time_ms = 20000; // keepalive 时间
    // 非空 => 主服务每个 RPC 都要求 authorization: Bearer <token>
    // (空 = 不鉴权, 与改动前完全一致)。建议写 auth_token: "${GRPC_AUTH_TOKEN:-}"
    // 让口令走环境变量, 不进 git。
    std::string auth_token;
};

// 级联配置
// 控制"主模型灰区 -> 二级分类器复核"的级联行为。所有字段带默认值;
// 未配置 cascade 段时 enabled=false, 退化为单模型路径。
// 该结构同时被 ConfigParser(解析) 与 CascadeEngine(运行) 使用。
struct CascadeConfig {
    bool enabled = false; // 是否启用级联
    std::string primary; // 主筛模型名(空 = 首个 detector)
    std::string secondary; // 二级分类器模型名(空 = 退化为单模型)
    // 触发条件(灰区)
    std::vector<std::string> trigger_labels; // 触发复核的类别(空 = 所有类别)
    float min_conf = 0.0f; // 灰区下界(含)
    float max_conf = 1.0f; // 灰区上界(不含)
    float roi_padding = 0.10f; // ROI 外扩比例(相对目标宽高)
    // 决策规则
    std::string accept_label; // 视为"通过"的二级标签(空 = 仅看置信度)
    float accept_conf = 0.50f; // 通过所需的最低二级置信度
    bool drop_rejected = true; // 二级否决时是否丢弃该目标
    bool boost_on_confirm = false; // 确认时是否用二级置信度提升主置信度
};

// 大模型异步复核配置
// 控制"告警候选 -> 异步大模型复核 -> 确认后告警"的行为。默认 enabled=false,
// 未启用时告警走原有本地规则(person>0.8), 行为不变。
struct ReviewConfig {
    bool enabled = false; // 是否启用大模型复核
    std::string endpoint = "llm:50052"; // 复核服务 gRPC 地址(host:port)
    int timeout_ms = 3000; // 单次复核超时(deadline)
    std::size_t queue_size = 128; // 有界队列容量(满则丢最旧)
    int worker_threads = 2; // 复核并发线程数
    // 触发条件(告警候选)
    std::vector<std::string> trigger_labels; // 触发复核的类别(空 = 所有类别)
    float min_conf = 0.5f; // 触发下界(含)
    float max_conf = 1.0f; // 触发上界(不含)
    float roi_padding = 0.10f; // ROI 外扩比例(相对目标宽高)
    // [占座/场景复核] 送 VLM 前把"单物体框"扩成"物体所在场景":
    //   只裁"一本书"(可能不到 120x120)大模型无法判断"占座/场景", 需要看到桌面/
    //   座位与周围环境。roi_context_scale: 以框中心为基准再放大多少倍(>=1; 1 = 不放大)
    //   roi_min_side:      放大后仍不足该边长(像素)时补到该边长(0 = 不补)
    // 两者都为默认值时行为与改造前**完全一致**。
    float roi_context_scale = 1.0f;
    int roi_min_side = 0;
    // [占座] 时序门: 只有"已在画面里停留够久"的目标才送审(ms; 0 = 关闭)。
    //   占座的定义是"长期占用" => 刚出现/刚建轨的物品先观望, 既省算力又减误报。
    //   依赖 tracking.enabled(拿不到 track_id 时不拦, 保持宽容)。
    int min_dwell_ms = 0;
    // 业务/策略
    std::string prompt; // 透传给复核服务的业务提示(可空)
    std::string alert_type = "疑似违规"; // 确认后写入的告警类型(默认中性, 业务自己覆盖)
    bool alert_on_failure = false; // 复核不可用(超时/失败)时是否仍告警

    // 接入真实 VLM 服务端所需的传输/鉴权/探活项
    int max_message_size_mb = 16; // gRPC 收发上限(ROI JPEG 较大时需放宽; gRPC 默认 4MB 会超限)
    int keepalive_time_ms = 20000; // HTTP/2 keepalive(长连接保活; 0 = 不设置)
    std::string auth_token; // 非空则每次调用带 metadata: authorization=Bearer <token>
    bool health_check = true; // init() 时探测一次 Health(仅用于日志; 失败不影响运行)
};

// 非视频传感器配置
// 一条 sensors: 记录描述一路**非视频**传感器(雷达/红外)。视频不走这里
// (视频由 video: 段描述, 用 VideoSensorSource 适配)。
// 与 CascadeConfig/ReviewConfig 一样放在 utils: 解析器与运行期(sensor:)
// 共用同一份定义, 避免 utils 反向依赖 sensor 模块。
struct SensorConfig {
    std::string kind = "radar"; // video | radar | infrared
    std::string name; // 唯一名(日志/统计); 空 = 用 kind
    std::string backend = "stub"; // file(回放) | stub(合成); 骨架实现
    std::string path; // backend=file 时的回放文件
    int rate_hz = 10; // backend=stub 的合成速率
    std::vector<std::string> labels; // 标签白名单(空 = 不过滤)
};

// 多模态融合配置
// 默认 enabled=false => 完全不启用融合, 行为与纯视觉一致。
struct FusionConfig {
    bool enabled = false; // 是否启用多模态融合
    std::string level = "decision"; // 融合层级; 目前仅支持 decision(决策级)
    int time_tolerance_ms = 50; // 时间对齐容差窗口(±, ms)
    float match_iou = 0.30f; // 空间关联最小 IoU(传感器无框时退化为标签关联)
    float sensor_weight = 0.35f; // 加权融合中传感器权重 [0,1]
    bool emit_sensor_only = false; // 未关联的传感器目标是否作为新目标输出(需其自带框)
    bool adopt_sensor_label = false; // 关联后是否采用传感器标签
    std::size_t buffer_capacity = 256; // 采样时间缓冲容量(满则丢最旧)
};

// 告警去重配置
// 控制"同一目标在连续帧里重复告警"的抑制。默认 enabled=true —— 这是**行为修正**:
// 告警判定按帧执行, 而"某目标疑似违规"描述的是目标状态 => 一个站着不动的人会被
// 连续帧反复告警(告警刷屏)。
// 语义: (标签 + 同一目标) 判为重复, 冷却窗内只放行一次;
// 冷却窗按"最近一次命中"刷新 => 目标持续在画面里只告警一次, 消失超过
// cooldown_ms 后再次出现才重新告警。
// 注: 两边都有 track_id 时**以身份为准**(同 id = 同目标, 与框怎么移动无关);
// 没有 id 时退回几何重叠 => 快速移动目标仍可能重复(开 tracking 即解决)。
struct AlertDedupConfig {
    bool enabled = true; // 是否启用去重(false = 完全等价于旧行为)
    float iou = 0.30f; // 判定"同一目标"的框重叠阈值 [0,1]; 0 = 退化为仅按标签
    int cooldown_ms = 5000; // 冷却窗(ms); 0 = 等于不去重
    int max_entries = 256; // 记忆条目上限(有界, 防长时间运行无界增长)
};

// 指标端点(Prometheus 抓取)
// enabled=false 时**不监听任何端口**(零行为变化)。端点无鉴权, 只应暴露在受信网络:
// * 容器内 bind 0.0.0.0 + compose 用 expose(不发布到宿主机), Prometheus 走容器网络抓取;
// * 裸机建议 bind 127.0.0.1, 或防火墙只放行抓取端。
struct MetricsConfig {
    bool enabled = false;
    std::string bind = "0.0.0.0";
    int port = 9100; // 避开 50051(gRPC) / 50052(复核) / 3306(MySQL)
};

// 告警推送(webhook)
// 缺口补全: 告警此前**只写 MySQL** —— 检测再准、去重再好, 没人会知道。
// enabled=false ⇒ 只落库(与改造前完全一致)。传输层只支持 http://(见 utils/HttpClient.h 的边界)。
struct AlertPushConfig {
    bool enabled = false;
    std::string url; // 如 http://127.0.0.1:8899/alert
    int timeout_ms = 3000; // 单次请求超时(连接+收发)
    int max_retries = 2; // 失败重试次数(不含首次)
    int retry_backoff_ms = 200; // 退避基数(第 n 次等待 base*2^(n-1), 上限 5s)
    std::size_t max_queue = 256; // 有界队列(满则丢最旧, 与帧队列同策略)
    int drain_timeout_ms = 5000; // 退出时最多再等多久把队列发完
    std::string header_name; // 可选自定义头(如 "x-alert-token")
    std::string header_value; // 其值(建议 ${ALERT_TOKEN:-} 走环境变量, 不进 git)
};

struct AlertConfig {
    AlertDedupConfig dedup;
    AlertPushConfig push; // 推送到 webhook(默认关闭 => 只落库, 行为不变)
};

// 目标跟踪配置
// 语义: 关联式跟踪(IoU 贪心 + 质心距离兜底), 给每个检测一个跨帧稳定的 track_id。
// * 有 id 后, 告警去重**以身份为准**(与框怎么移动无关);
// * dwell(连续被跟踪时长)是停留/徘徊等行为分析的立足点;
// * 无外观特征 => 遮挡/交叉后可能换 id(ID switch);
// * id 单调递增且**永不复用**(复用会让去重漏报新目标)。
struct TrackingConfig {
    bool enabled = true;
    float iou = 0.30f; // 关联的框重叠阈值 [0,1]
    float dist_factor = 1.0f; // 质心距离兜底: <= dist_factor * 框半周长; <=0 = 关闭
    int max_age_ms = 1000; // 失配后轨迹保留时长(容忍遮挡/漏检)
    int min_hits = 1; // 连续命中多少次后才输出 id(>1 = 抑制瞬时误检)
    int max_tracks = 256; // 轨迹上限(有界, 防长时间运行无界增长)
};

// 座位区域(配置里的坐标, 刻意不用 cv::Point —— utils 不引入 OpenCV 依赖,
// 由 main 在装配时转成 occupancy::SeatZone)
struct SeatPoint {
    int x = 0;
    int y = 0;
};

struct SeatZoneConfig {
    std::string name;
    // 顶点(像素坐标)。两种写法都支持:
    //   polygon: [[x0,y0],[x1,y1],[x2,y2],[x3,y3]]   任意凸多边形(推荐, 贴合透视)
    //   rect:    [x, y, w, h]                        矩形(内部展开为 4 个顶点)
    std::vector<SeatPoint> polygon;
};

// 占座判定配置  [新增]
// ---------------------------------------------------------------------------
// 定位: 卡在「检测」与「大模型复核」**之间**的规则层 —— 用静态座位 zone + 几何关系
// + 时序状态机, 把"画面里散落着几个 book/bag"变成
// "A-12 号桌 上物品在、人不在, 已持续 412 秒" 这个**可解释的结论**。
//
// 为什么不能省掉这一层(两个常见误解):
//   ① "再加一个检测器就能判占座" —— 不能。检测器没有"座位"这个概念, 再多模型
//      也只是多检出几类**物品**, 拿不到"物品属于哪个座位"的空间关系。
//   ② "让 VLM 看图判占座" —— 单帧图给不出"持续了多久", 而时间维度恰恰是
//      "占座"定义的核心(坐了 1 分钟 vs 占了 1 小时)。
//
// 本层判定后, 只有规则筛出的占座候选才送 VLM 复核 => VLM 调用量降 1~2 个数量级,
// 且送审时带上结构化证据(describe() 文案), VLM 从"看图猜场景"变成"对证据做裁判"。
//
// 默认 enabled=false => 不启用, 行为与改造前完全一致(逐物体送审照旧)。
// ⚠ 交互: enabled=true 时, 标签命中 item_labels 的目标**不再**走"逐物体送审/告警"
//    链路(改由占座事件产生告警), 否则会出现"书本告警"与"占座告警"重复上报。
// 判据 C1~C7 的完整说明见 src/occupancy/SeatOccupancyAnalyzer.h。
struct OccupancyConfig {
    bool enabled = false;
    std::vector<SeatZoneConfig> seats; // 座位区(静态: 机位固定则座位是常量)
    std::vector<std::string> item_labels; // 构成"物品"的类别; 空 = 用内置默认
    std::string person_label = "person";
    // C1 物品归属座位: 底边中点在区内 **或** 与座位区包含度 >= 该值
    float item_seat_overlap = 0.5f;
    // C1b 物品在人手上: 物品面积落在 person 框内的比例 >= 该值且中心也在人框内
    //     => 随身物品, 不算放在桌上(用包含度而非 IoU, 理由见 SeatOccupancyAnalyzer.h)
    float item_person_overlap = 0.5f;
    // C2 人在使用座位: 底边中点在区内 **或** 包含度 >= 该值。
    // 0.30 而非 0.15: 15% 太松 —— 邻座/路人的框稍一重叠就命中, 会把占座计时反复暂停 => 漏报。
    float person_seat_iou = 0.30f;
    // C2 人框最小高度(像素): 滤掉远景小框/误检; 0 = 不过滤
    int min_person_height_px = 0;
    // C3 判占座的持续时间(ms): "物品在 ∧ 人不在"累计够久才判占座。
    //    这是**业务口径** —— 建议先按 300000(5 分钟)上线, 再由业务方拍板。
    int t_occupied_ms = 300000;
    // C4 短暂离开容忍(ms): 人短暂出现(不足该时长)**暂停**计时而不清零;
    //    人稳定在场 >= 该时长才复位。用于滤掉"起身接水/有人路过被误检"。
    int t_grace_ms = 90000;
    // C5 稳定性投票: 最近 vote_n 帧中至少 vote_m 帧满足才算数(去抖动)。
    //    vote_n=1 时 vote_m 必须为 1 => 等价于逐帧判定。
    int vote_n = 10;
    int vote_m = 7;
};

struct AppConfig {
    LogConfig log;
    VideoConfig video;
    PipelineConfig pipeline;
    DatabaseConfig database;
    GrpcConfig grpc;
    CascadeConfig cascade;
    ReviewConfig review;
    std::vector<SensorConfig> sensors; // 非视频传感器列表
    FusionConfig fusion; // 多模态决策级融合
    AlertConfig alert; // 告警去重
    TrackingConfig tracking; // 目标跟踪(track_id)
    OccupancyConfig occupancy; // [新增] 占座判定(静态座位 zone + 规则状态机)
    MetricsConfig metrics; // 指标端点(Prometheus)

    // 结果视频输出路径(带框视频)。相对路径按**当前工作目录**解析(与旧行为一致),
    // 也可写绝对路径。默认 "output.avi" = 改造前行为, 零影响。
    // 动机: Docker 部署时把它指到挂载卷(如 /app/output/output.avi), 宿主机/侧车
    // (web_gateway) 才拿得到结果视频; 这是唯一原先写死、无法配置的产物路径。
    // 配置写法: app.output_path: "output.avi" (支持 ${VAR} 环境变量展开)。
    std::string output_path = "output.avi";
};

// 模型推理配置
// 由"单模型"扩展为"可命名 + 带角色"的模型描述, 以支持多模型注册;
// 新增字段均带默认值, 旧的单模型配置解析路径保持兼容。
struct ModelConfig {
    std::string name = "yolov8_detector"; // 模型名(多模型注册/查找用)
    std::string role = "detector"; // detector | classifier | reviewer
    std::string model_xml_path;
    std::string model_bin_path;
    std::string labels_path;
    int input_width = 640;
    int input_height = 640;
    float conf_threshold = 0.25f;
    float nms_threshold = 0.45f;
    int pool_size = 0; // 该模型引擎池大小; 0 = 由 ModelPoolManager 按 worker_threads 兜底
    int acquire_timeout_ms = 2000; // 借引擎超时(ms)

    // 推理性能旋钮(可选; 默认值 = 改造前行为, 不向 OpenVINO 设任何属性)
    // 背景: 原先硬编码 core.compile_model(model, "AUTO") + 默认性能模式(LATENCY)。
    // 当 pipeline.worker_threads = N (>1) 时, N 个引擎各自按 LATENCY 开满物理核
    // -> 线程超订互相抢核, 总吐吐反而暴跌(实测 RTSP 场景仅 3.6fps)。
    std::string device = "AUTO"; // AUTO | CPU | GPU | GPU.0 | NPU ...
    std::string perf_mode; // 空=不设置(LATENCY) | latency | throughput
    int num_threads = 0; // 0=不设置(默认=物理核数); >0=推理线程数
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
    // 主模型(首个); 保持旧接口兼容
    const ModelConfig& getModelConfig() const { return model_config_; }
    // 全部模型(单模型配置会被规整为长度 1 的列表)
    const std::vector<ModelConfig>& getModelConfigs() const { return model_configs_; }

private:
    // 配置合法性校验; 加载成功后自动调用
    bool validate() const;
    // 模型配置校验(角色/阈值/重名等)
    bool validateModelConfigs() const;

    AppConfig app_config_;
    ModelConfig model_config_;
    std::vector<ModelConfig> model_configs_;
};