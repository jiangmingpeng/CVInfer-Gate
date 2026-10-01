# CVInfer-Gate 改造工程笔记（PROJECT_NOTES）

> **用途**：记录本轮架构重构的任务、根因、修复方案与接口约定。
> **给新对话**：先读本文，再 `git diff` / `git log` 即可快速对齐上下文，无需从头复述。
> 最近更新：T1–T31 完成（Phase A=T12–T15 地基；Phase B=T16–T19 级联主筛 + 二级复核；Phase C=T20–T22 大模型异步复核；Phase D=T23–T26 多模态接入与决策级融合；**T27 = 阶段自检工具 + 配置注入 + 复核服务 mock**，见第 14 节；**T28 = RTSP 真实接入；T29 = 输出视频正确性修正（容器帧率 + sink 保序）**，见第 16 节；**T30 = 推理性能旋钮配置化（device/performance_mode/num_threads）**，见第 18 节；**T31 = 视频源配置交叉校验 + VideoWriter 降噪**，见第 19 节；**T32 = 迁移本地 WSL（环境适配 + 配置审查 + “数据库启动强耦合”的发现）**；**T33 = WSL 下 `device=AUTO` 触发 NPU 插件段错误（改 `device: CPU`）**；**T34 = 修复优雅关闭完全失灵（`sigwait` → `sigaction` + self-pipe）**，见第 20 节；**T35 = 推理性能收官（缓存友好改造 + 分段计时 ⇒ FP32 天花板定量化 ~33fps）**，见 §20.14）。
> **新对话请先读第 17 节「架构总览」**（主动脉 + 四个挂载点 + 线程/关闭顺序 + 开关映射 + 验证状态），再按需下钻。

---

## 0. 一句话背景

CVInfer-Gate 是一个 C++17 视频推理网关：**FFmpeg/OpenCV 解码 → OpenVINO(YOLO) 推理 → 后处理 NMS → MySQL 落库**，并通过 **gRPC** 对外提供检测接口，前端由 Python Flask BFF 做 HTTP 转换。

原始版本“能跑”，但存在若干**架构级缺陷**（线程安全、启动时序、无法优雅退出、队列/DB 无背压）。本轮按任务清单 **T1–T22** 系统性重构，**只做加法 / 兼容性重命名，不破坏既有行为**。

---

## 1. ⚠️ 协作与环境约束（务必先看）

| 约束 | 说明 | 应对 |
|---|---|---|
| `config/config.yaml` 被锁 | **读、写均被安全策略拒绝**（Security concern） | 用模板 `config/config.new.yaml` 承载，用户手工 `copy` 覆盖 |
| 终端为独立远程环境 | AI 侧**拿不到命令输出、也看不到副作用文件**（实测：重定向写文件后读不到） | 编译/运行一律由用户在自己的终端执行；AI 侧靠“接口交叉核对”+ 自检程序（T27）保证正确 |
| CMake 用 `file(GLOB_RECURSE "src/*.cpp")` | **新增 `.cpp` 不会自动纳入构建** | 任何新增源文件后**必须重跑 cmake** |
| 编辑文件用工具 | 不要用 `sed/awk` 改文件 | 用 Edit/MultiEdit 工具 |
| 日志宏前缀 | 统一 `CVLOG_`，避免与 `<syslog.h>` 的 `LOG_INFO` 冲突 | — |

---

## 2. 目录结构（改造后）

```text
proto/
├── inference.proto                 # DetectionService (本仓库=服务端)
└── review.proto                    # T20 ReviewService (本仓库=客户端)

src/
├── main.cpp                        # T7 装配收口
├── video/                          # IVideoSource / FileVideoSource / RtspVideoSource
├── inference/
│   ├── IInferenceEngine.h          # 接口: init(ModelConfig) / infer(cv::Mat, vector<ov::Tensor>&)
│   ├── OpenVINOEngine.h/.cpp       # ⚠ 非线程安全（单 ov::InferRequest_）
│   ├── InferenceEnginePool.h/.cpp  # T4 引擎池 + EngineGuard(RAII)
│   ├── IModel.h                    # T12 模型抽象: IModel/IDetector/IClassifier + ModelRole/DetectStatus
│   ├── YoloDetector.h/.cpp         # T13 把单模型链路封装为 IDetector（内部持引擎池+后处理+标签）
│   ├── ModelFactory.h/.cpp         # T12 按 role 构建模型
│   ├── ModelPoolManager.h/.cpp     # T15 多模型注册表（每模型一池）+ primaryDetector()/buildCascade()
│   ├── BehaviorClassifier.h/.cpp   # T17 二级分类器（实现 IClassifier: ROI -> 类别）
│   ├── ClassificationPostProcessor.h/.cpp  # T17 分类后处理（自动 softmax + argmax）
│   ├── CascadeEngine.h/.cpp        # T16 级联（实现 IDetector: 灰区主筛 -> 二级复核）
│   ├── YoloPostProcessor.h/.cpp    # process(ov::Tensor, cv::Size, labels)
│   └── DetectionResult.h           # T16 增 reviewed/sub_* 级联元数据
├── pipeline/
│   ├── VideoPipeline.h/.cpp        # T5 三阶段流水线
│   └── MultiSensorPipeline.h/.cpp  # T26 多模态融合编排(poller 线程 + 有界时间缓冲)
├── sensor/                         # [T23–T24] 统一传感器抽象
│   ├── ISensorSource.h             # T23 抽象 + SensorKind/SensorTarget/SensorSample/nowMs()
│   ├── VideoSensorSource.h/.cpp    # T23 IVideoSource -> 统一时间戳(非拥有适配)
│   ├── ReplaySensorSource.h/.cpp   # T24 file(回放)/stub(合成)骨架 + 可打断限速
│   ├── RadarSensorSource.h         # T24 雷达(无像素框 -> 标签关联)
│   ├── InfraredSensorSource.h      # T24 红外(带框 -> IoU 关联)
│   └── SensorSourceFactory.h       # T24 按 kind 构建(与 ModelFactory 同构)
├── fusion/
│   └── SensorFusion.h/.cpp         # T25 时间对齐 + 目标关联 + 决策级置信度融合
├── database/
│   ├── ConnectionPool.h/.cpp       # T9 连接池 + ConnectionGuard(RAII)
│   └── DBWriter.h/.cpp             # T9 异步落库
├── service/
│   ├── DetectionServiceImpl.h/.cpp # T7 改用引擎池
│   └── GrpcServerSetup.h/.cpp      # T8 gRPC 服务构建
├── review/                         # [T20–T22] 大模型异步复核
│   ├── IReviewService.h            # T20 复核抽象 + ReviewRequest/Result/Status
│   ├── GrpcLlmReviewer.h/.cpp      # T20 复用 gRPC 的复核客户端(带 deadline)
│   └── ReviewScheduler.h/.cpp      # T21 异步调度(有界+超时+按 frame_seq 回收)
└── utils/
    ├── ConfigParser.h/.cpp         # T1 嵌套配置 + env 展开 + validate()
    ├── Logger.h/.cpp               # T2 分级日志
    ├── LifecycleCoordinator.h/.cpp # T6 生命周期 + SignalWatcher
    ├── RoiUtils.h                  # T16/T22 ROI 外扩裁剪(级联与复核共用)
    └── ThreadSafeQueue.h           # T3 有界队列(策略/统计/超时)
```

---

## 3. 核心架构问题 → 修复方案（本轮重点）

这是本次改造的**根因清单**，也是后续排 bug 时最该先排查的地方。

### BUG-1｜推理引擎非线程安全（最严重）
- **现象**：`OpenVINOEngine` 内部只持有一个 `ov::InferRequest_`，不是线程安全。
- **原版行为**：视频流水线线程与 gRPC 服务线程**共用同一个引擎实例** → 数据竞争 / 崩溃 / 结果错乱。
- **修复（T4）**：新增 `InferenceEnginePool`，按 `pipeline.worker_threads` 建 N 个独立引擎；`acquire(timeout)` 借、`release()` 还；`EngineGuard` 为 RAII 自动归还。流水线 worker 与 gRPC 请求都从池中借引擎。

### BUG-2｜gRPC 启动时序错误
- **现象**：原 `main.cpp` **先跑完整个视频循环，才启动 gRPC**；处理长视频期间服务完全不可用，且无退出机制。
- **修复（T7）**：gRPC 在**独立线程** `grpc_thread` 中启动，与流水线**并行**。

### BUG-3｜无法优雅关闭
- **现象**：`server->Wait()` 硬阻塞，Ctrl+C 直接杀进程，DB 未 flush、线程未 join。
- **修复（T6）**：`SignalWatcher`（专用 `sigwait` 线程，早于其它线程创建以继承信号掩码）→ 回调 `LifecycleCoordinator::requestShutdown()`；主线程 `waitUntilShutdown()` 阻塞；关闭顺序 **gRPC → 流水线 → DB**。

### BUG-4｜队列无界 / 无背压
- **现象**：原 `ThreadSafeQueue` 容量硬编码 10，无丢弃统计、无超时、无法感知“满”。
- **修复（T3）**：容量/策略来自 `video.queue.*`；支持 `DropOldest`/`Block` 策略、`Stats`、带超时 `pop`、`try_pop`；头文件去掉 OpenCV 依赖。

### BUG-5｜同步落库阻塞推理
- **现象**：原 `DBWriter` 单连接 + 在消费线程里同步写库 → 高并发瓶颈、连接失效无恢复。
- **修复（T9）**：`ConnectionPool`（`database.pool_size` 个连接）+ `DBWriter` 异步入队（有界队列，满则 DropOldest），后台 writer 线程消费；`flush()/stop()` 支持收尾。

### BUG-6｜无分级日志
- **修复（T2）**：`Logger` 单例 + `CVLOG_TRACE/DEBUG/INFO/WARN/ERROR` 流式宏，支持控制台+文件，`app.log_level/log_file` 生效。

### BUG-7｜配置扁平、无校验、无法注入密钥
- **修复（T1）**：`AppConfig` 由扁平字段重构为**分组嵌套**（log/video/pipeline/database/grpc）；加载后 `validate()`；支持 `${VAR}` / `${VAR:-default}` 环境变量展开（DB 密码用此避免明文）。

### BUG-8｜gRPC 消息大小 / 超时 / keepalive 未落地
- **现象**：T1 已解析 `grpc.max_message_size_mb/worker_threads/keepalive_time_ms/timeout_ms`，但**无人消费**；gRPC 默认收消息上限仅 4MB，大图必被拒。
- **修复（T8）**：`GrpcServerSetup` 把它们真正应用到 `ServerBuilder`；`timeout_ms` 用于服务端借引擎超时 + 客户端 deadline。

### BUG-9｜web_gateway 硬编码服务 IP
- **现象**：`web_gateway/app.py` 把 C++ gRPC 地址写死为 `106.15.88.152:50051`，端口 `8080` 也写死 → 换环境/换机必须改源码，易泄露真实 IP。
- **修复（T10）**：改由环境变量注入（`os.environ`），缺省值全部为本地/通用值；可选 `python-dotenv` 加载 `.env`。同时按 T8/R-7 在客户端设置 `grpc.max_receive_message_length` 与 deadline。

### BUG-10｜DBWriter 逐帧写库 / 失败即重试 / 数据易丢
- **现象**：每帧（每个检测批）单独写库，工地场景下写入频率极高；一旦写库失败，每个任务都重试并刷屏日志；数据库长时间不可用时数据直接丢失。
- **修复（T11）**：在 `DBWriter` 内部新增三项能力（对外接口 `writeDetections/writeAlert/flush/stop` 不变）：
  1. **状态缓存**：维护 `db_healthy_`。失败时仅告警一次并暂停写库，之后每 `reconnect_interval_ms`（默认 30s）或累计 `reconnect_after_writes`（默认 100）次冲刷后做一次探测重试，应对偶发闪断。
  2. **异步批量**：后台线程把任务攒进 `batch_`，达到 `batch_size`（默认 20）条或距上次冲刷超过 `flush_interval_ms`（默认 1000ms）时，用**多 `VALUES` 的 `INSERT`**（`(?,?,...),(?,?,...)`）在事务内一次写多行。仅依赖 `prepareStatement/setXxx/executeUpdate` 等基础 API，**不依赖 `addBatch()/executeBatch()`**（部分 Connector/C++ 版本不提供）。单条语句最多 500 行，防占位符/包体超限。
  3. **本地降级**：数据库不可用时把结构化记录追加写 `database.fallback_path`（默认 `db_fallback.csv`），保证数据不丢；恢复后自动回传并清理文件。

---

## 4. 任务总览（T1–T22 状态）

| 任务 | 内容 | 状态 | 主要产出 |
|---|---|---|---|
| T1 | 配置结构升级（嵌套 + 校验 + env 展开） | ✅ | `ConfigParser.h/.cpp`、`config/config.new.yaml` |
| T2 | 分级日志 | ✅ | `utils/Logger.h/.cpp` |
| T3 | 线程安全队列增强（有界/策略/统计/超时） | ✅ | `utils/ThreadSafeQueue.h` |
| T4 | 推理引擎池（解决非线程安全） | ✅ | `inference/InferenceEnginePool.h/.cpp` |
| T5 | 三阶段视频流水线 | ✅ | `pipeline/VideoPipeline.h/.cpp` |
| T6 | 生命周期协调器 + 信号监听 | ✅ | `utils/LifecycleCoordinator.h/.cpp` |
| T7 | `main.cpp` 装配收口 + 服务改用引擎池 | ✅ | `main.cpp`、`service/DetectionServiceImpl.h/.cpp` |
| T8 | gRPC 超时/消息大小/keepalive/线程数落地 | ✅ | `service/GrpcServerSetup.h/.cpp`、`tests/test_grpc_client.cpp` |
| T9 | DB 连接池 + 异步落库 | ✅ | `database/ConnectionPool.h/.cpp`、`database/DBWriter.h/.cpp` |
| T10 | web_gateway 去硬编码 IP，运行参数改环境变量 | ✅ | `web_gateway/app.py`、`web_gateway/env.example` |
| T11 | DBWriter 状态缓存 + 批量写入 + 本地降级 | ✅ | `database/DBWriter.h/.cpp`、`utils/ConfigParser.*`、`config/config.new.yaml` |
| T12 | 模型抽象层（IModel/IDetector/IClassifier + ModelRole + DetectStatus）与模型工厂 | ✅ | `inference/IModel.h`、`inference/ModelFactory.h/.cpp` |
| T13 | YoloDetector：把现有单模型链路封装为 IDetector（行为等价） | ✅ | `inference/YoloDetector.h/.cpp` |
| T14 | 配置多模型化（`models:` 列表 + 旧格式兼容 + 模型校验） | ✅ | `utils/ConfigParser.h/.cpp`、`config/model_config.new.yaml` |
| T15 | ModelPoolManager（每模型一池）+ 流水线/gRPC/`main` 接线到 `IDetector` | ✅ | `inference/ModelPoolManager.h/.cpp`、`pipeline/VideoPipeline.*`、`service/DetectionServiceImpl.*`、`main.cpp` |
| T16 | 级联引擎 `CascadeEngine`（实现 `IDetector`：灰区主筛 → ROI → 二级复核） | ✅ | `inference/CascadeEngine.h/.cpp`、`inference/DetectionResult.h` |
| T17 | `BehaviorClassifier`（实现 `IClassifier`）+ 分类后处理；`ModelFactory` classifier 分支落地 | ✅ | `inference/BehaviorClassifier.h/.cpp`、`inference/ClassificationPostProcessor.h/.cpp`、`inference/ModelFactory.*` |
| T18 | `ModelPoolManager::buildCascade` + `main` 按 `cascade.enabled` 注入级联 | ✅ | `inference/ModelPoolManager.h/.cpp`、`main.cpp` |
| T19 | 级联配置（`cascade:` 段）+ 校验 + 运行统计（主筛/触发/确认/否决/降级） | ✅ | `utils/ConfigParser.h/.cpp`、`config/cascade.example.yaml` |
| T20 | 复核抽象 `IReviewService` + `GrpcLlmReviewer`（复用 gRPC，带 deadline） | ✅ | `review/IReviewService.h`、`review/GrpcLlmReviewer.h/.cpp`、`proto/review.proto` |
| T21 | `ReviewScheduler`：异步 + 有界队列(DropOldest) + 超时 + 按 `frame_seq` 回收 | ✅ | `review/ReviewScheduler.h/.cpp` |
| T22 | 接入告警链路（复核确认后才 `writeAlert`）+ sink 透传 `frame_seq` + 统计 | ✅ | `main.cpp`、`pipeline/VideoPipeline.*`、`utils/RoiUtils.h` |
| T23 | 统一传感器抽象：`ISensorSource` + `SensorSample`（统一时间戳）+ `VideoSensorSource` 适配旧 `IVideoSource` | ✅ | `sensor/ISensorSource.h`、`sensor/VideoSensorSource.h/.cpp`、`inference/DetectionResult.h` |
| T24 | 雷达/红外传感器骨架（file 回放 / stub 合成）+ 工厂 + `sensors:` 配置解析与校验 | ✅ | `sensor/ReplaySensorSource.*`、`sensor/RadarSensorSource.h`、`sensor/InfraredSensorSource.h`、`sensor/SensorSourceFactory.h`、`config/sensors.example.yaml` |
| T25 | `SensorFusion`：时间对齐（容差窗口）+ 目标关联（IoU/标签）+ 决策级加权置信度融合 | ✅ | `fusion/SensorFusion.h/.cpp` |
| T26 | `MultiSensorPipeline`：poller 线程 + 有界时间缓冲（丢最旧）+ sink 最前置融合阶段 + 统计 | ✅ | `pipeline/MultiSensorPipeline.h/.cpp`、`main.cpp`、`utils/ConfigParser.h/.cpp` |
| T27 | 阶段自检工具（Phase A~D 一次跑完，零外部依赖）+ 配置路径注入 `--config` + 复核服务 Python mock；修正回放文件时间戳语义（R-11） | ✅ | `tests/phase_selftest.cpp`、`CMakeLists.txt`、`main.cpp`、`config/config.test.yaml`、`scripts/mock_review_server.py`、`scripts/sensor_replay_demo.txt` |
| T28 | RTSP 真实接入：独立运行配置 + 分辨率未知时降级（不再 `return -1`）+ `IVideoSource::getFps()` | ✅ 跑通 | `config/config.rtsp.yaml`、`main.cpp`、`video/IVideoSource.h`、`video/RtspVideoSource.h/.cpp` |
| T29 | 输出视频正确性：容器帧率 = 源fps/frame_interval + sink 按 `frame_seq` 保序（修 R-1） | ✅ 已修/待回归 | `main.cpp`、`pipeline/VideoPipeline.cpp` |
| T30 | 推理性能旋钮配置化：`device`/`performance_mode`/`num_threads`（默认值 = 改造前行为） | ✅ 已改/已验证默认路径 | `utils/ConfigParser.h/.cpp`、`inference/OpenVINOEngine.cpp`、`config/model_config.yaml` |
| T31 | 视频源配置交叉校验（file+rtsp:// 直接报错）+ VideoWriter 延迟 open（消除 `[ERROR:0]` 噪声） | ✅ 已改/待回归 | `utils/ConfigParser.cpp`、`main.cpp` |
| T32 | 迁移本地 WSL：配置审查（无硬编码✅）+ `mediamtx.yml` 开 Control API + `.gitignore` 补漏；发现“DB 不可用⇒程序直接退出” | ⚙️ 环境类/部分已改 | `mediamtx.yml`、`.gitignore`、待办见 §20.4 |
| T33 | WSL 下 `device=AUTO` → AUTO 插件枚举到 NPU 时在 `create_plugin_engine()` 段错误；改为显式 `device: CPU` | ✅ 已改/待回归 | `config/model_config.yaml`，详见 §20.7 |
| T34 | **优雅关闭完全失灵**（Ctrl+C / `kill -TERM` 后一条关闭日志都没有）：4 个库线程在 `main()` 前就已创建且掩码为空，SIGTERM 被内核投给它们 → 默认动作杀进程；`SignalWatcher` 改用 `sigaction` + self-pipe | ✅ 已改/待回归 | `src/utils/LifecycleCoordinator.{h,cpp}`，详见 §20.8 |
| T35 | 推理性能收官：后处理缓存友好化 + 预处理直写输入 tensor（worker 串行 **36 → 5.7 ms/帧**）+ `[T35]` 分段计时；**结论：FP32 天花板 ~33fps，瓶颈在推理自身（FLOP），流水线侧已无可优化** | ✅ 已完成/已验证 | `src/inference/YoloPostProcessor.cpp`、`src/inference/OpenVINOEngine.cpp`、`src/inference/YoloDetector.cpp`，详见 §20.14 |

> **Phase A（T12–T15）= 地基**：把“单引擎池”升级为“多模型注册 + IDetector 抽象”。
> **Phase B（T16–T19）= 级联主筛 + 二级复核**：主模型灰区目标 → ROI → 二级分类器复核，
> 级联同样实现 `IDetector`，故流水线/gRPC 零改动。详见第 13 节。
> **Phase C（T20–T22）= 大模型异步复核**：告警候选 ROI → 异步送复核服务 → 确认后才告警；
> 复核只作用于告警链路，画框/写视频/检测入库不受影响。详见第 13 节。
> **Phase D（T23–T26）= 多模态接入与决策级融合**：雷达/红外采样进有界时间缓冲，
> 每帧按容差窗口对齐 + 目标关联 + 加权置信度融合；融合作为 **sink 最前置阶段** 挂上，
> `VideoPipeline` 零改动。详见第 13 节。

---

## 5. 关键接口速查（新对话对齐用）

```cpp
// 配置（T1）
struct AppConfig { LogConfig log; VideoConfig video; PipelineConfig pipeline;
                   DatabaseConfig database; GrpcConfig grpc; };
// video: source_type,source_path,target_fps,frame_interval,queue{max_size,policy}
// pipeline: worker_threads
// database: host,user,password,dbname,pool_size,batch_size,flush_interval_ms,max_retries
// grpc: port,timeout_ms,max_message_size_mb,worker_threads,keepalive_time_ms
ConfigParser::loadAppConfig(path); getAppConfig(); getModelConfig();

// 日志（T2）
Logger::instance().init(app_cfg.log);
CVLOG_INFO << "x=" << 1;   // 宏，前缀 CVLOG_

// 引擎池（T4）
InferenceEnginePool::init(const ModelConfig&, int pool_size);
std::shared_ptr<IInferenceEngine> acquire(std::chrono::milliseconds);
EngineGuard guard(pool, timeout); if (guard.valid()) guard->infer(img, outs);

// 模型抽象（T12–T15）
enum class ModelRole { Detector, Classifier, Reviewer };
enum class DetectStatus { Ok, Busy, Failed };
class IModel { bool init(const ModelConfig&); const std::string& name(); ModelRole role(); };
class IDetector : public IModel { DetectStatus detect(const cv::Mat&, std::vector<DetectionResult>& out); };
class IClassifier : public IModel { DetectStatus classify(const cv::Mat& roi, Classification& out); };  // T17
ModelFactory::create(const ModelConfig&) -> std::shared_ptr<IModel>;   // detector->YoloDetector / classifier->BehaviorClassifier
ModelPoolManager::init(const std::vector<ModelConfig>&, int default_pool_size);
  // get(name)/getDetector(name)/getClassifier(name)/primaryDetector()/names()/size()/close()
  // [T18] buildCascade(const CascadeConfig&) -> std::shared_ptr<IDetector>
// ModelConfig 新增字段（带默认值）: name, role("detector"), pool_size(0=auto), acquire_timeout_ms(2000)

// 级联（T16–T19）
struct CascadeConfig {   // in utils/ConfigParser.h, AppConfig::cascade
  bool enabled; std::string primary, secondary;
  std::vector<std::string> trigger_labels; float min_conf, max_conf, roi_padding;
  std::string accept_label; float accept_conf; bool drop_rejected, boost_on_confirm; };
CascadeEngine(shared_ptr<IDetector> primary, shared_ptr<IClassifier> secondary /*可空*/, CascadeConfig);
  // detect(frame,out): 非灰区透传 -> 灰区 ROI -> classify -> 确认保留(写 sub_*)/否决丢弃
  // stats(): primary/triggered/confirmed/rejected/skipped
BehaviorClassifier : IClassifier;   classify(roi, out) -> Ok|Busy|Failed
ClassificationPostProcessor(labels).process(tensor) -> Classification  // 自动判定 softmax
// DetectionResult 新增: reviewed, sub_class_id, sub_confidence, sub_label

// 大模型复核（T20–T22）
struct ReviewConfig {   // in utils/ConfigParser.h, AppConfig::review
  bool enabled; std::string endpoint, alert_type, prompt; int timeout_ms, worker_threads;
  std::size_t queue_size; std::vector<std::string> trigger_labels;
  float min_conf, max_conf, roi_padding; bool alert_on_failure; };
enum class ReviewStatus { Ok, Unavailable, Timeout, Failed };
class IReviewService { const std::string& name(); ReviewStatus review(const ReviewRequest&, ReviewResult& out); };
GrpcLlmReviewer : IReviewService;   // init(ReviewConfig) 建 channel; review() 带 deadline=timeout_ms
ReviewScheduler::init(ReviewConfig, shared_ptr<IReviewService>, OutcomeCallback);
  submit(ReviewRequest) -> bool;    // 非阻塞(有界 DropOldest)
  stop(); stats();                  // submitted/dropped/reviewed/confirmed/rejected/timeout/unavailable/failed
// on_outcome(ReviewOutcome): frame_seq + status + confirmed + alert(已含兜底) + label/confidence/reason
// VideoPipeline::ResultCallback 增参: (uint64_t frame_seq, const cv::Mat&, const vector<DetectionResult>&)

// 多模态传感器（T23–T24）
// struct SensorConfig {   // in utils/ConfigParser.h, AppConfig::sensors
//   std::string kind, name, backend, path; int rate_hz; std::vector<std::string> labels; };
//   kind: video|radar|infrared   backend: file|stub   (校验层禁止 sensors 里写 kind=video)
enum class SensorKind { Video, Radar, Infrared };
struct SensorTarget { int class_id; std::string label; float confidence;
                      cv::Rect2f box; float distance_m; double azimuth_deg; };
struct SensorSample { SensorKind kind; int64_t timestamp_ms; cv::Mat frame; vector<SensorTarget> targets; };
class ISensorSource { name(); kind(); open(const SensorConfig&); close(); read(SensorSample&);
                      lastTimestampMs(); };
int64_t sensor::nowMs();   // 统一时间戳(steady_clock 毫秒)
sensor::createSensorSource(const SensorConfig&) -> shared_ptr<ISensorSource>;  // nullptr=失败(应跳过)
// 实现: VideoSensorSource(非拥有适配 IVideoSource) / RadarSensorSource / InfraredSensorSource
//   RadarSensorSource、InfraredSensorSource 均继承 ReplaySensorSource(file/stub 两种 backend)
//   ★ 自定义传感器必须保证 close() 能打断 read() 的等待(否则 stop() 的 join 会卡住)

// 决策级融合（T25–T26）
// struct FusionConfig {   // in utils/ConfigParser.h, AppConfig::fusion
//   bool enabled; std::string level("decision"); int time_tolerance_ms;
//   float match_iou, sensor_weight; bool emit_sensor_only, adopt_sensor_label;
//   std::size_t buffer_capacity; };
struct FusionStats {  // frames/samples_seen/aligned/targets/matched/unmatched_sensor/emitted_sensor_only
};
SensorFusion(FusionConfig);
  static align(samples, anchor_ms, tol_ms) -> vector<SensorSample>;   // 时间对齐(纯函数)
  fuse(vector<DetectionResult>& vision, samples, anchor_ms) -> FusionStats;  // 就地融合
MultiSensorPipeline::init(const FusionConfig&, vector<shared_ptr<ISensorSource>>, ISensorSource* video_meta);
MultiSensorPipeline::fuse(vector<DetectionResult>&) -> void;   // sink 调用(不阻塞)
MultiSensorPipeline::stop(); stats();   // Stats: frames/fusion{...}/samples_pushed/samples_dropped/poll_errors/buffer_size
// DetectionResult 新增: fused, vision_confidence(融合前视觉置信度, -1=未融合), sensor_confidence, distance_m

// 连接池（T9）
ConnectionPool::init(const DatabaseConfig&);
ConnectionGuard conn(pool, timeout); if (conn.valid()) conn.get();

// 流水线（T5；T15 起签名改为依赖 IDetector）
VideoPipeline(IVideoSource&, IDetector&, Config, LifecycleCoordinator&, ResultCallback cb);
pipeline.start(); pipeline.stop(); pipeline.stats();  // stats: decoded/dropped/processed/emitted

// gRPC 服务（T15 起依赖 IDetector）
DetectionServiceImpl(IDetector&);   // 引擎借用/推理/后处理收敛进模型内部

// 生命周期（T6）
LifecycleCoordinator: registerChild/unregisterChild/requestShutdown/isShutdownRequested/
                      waitUntilShutdown/waitForChildren
SignalWatcher(cb).start({SIGINT,SIGTERM}); .stop();

// 引擎接口（未改动）
IInferenceEngine::init(const ModelConfig&); infer(const cv::Mat&, std::vector<ov::Tensor>&);
YoloPostProcessor(conf, nms); process(const ov::Tensor&, const cv::Size&, const std::vector<std::string>&);
```

---

## 6. 运行流程（`main.cpp` 装配顺序）

```
加载 config / model 配置
  → Logger.init
  → SignalWatcher.start           (必须早于其它线程)
  → DBWriter.init                 (连接池 + 异步线程)
  → 选择并打开视频源 (file/rtsp)
  → ModelPoolManager.init(config.getModelConfigs(), worker_threads)   [T12–T15]
  → [T18] detector = cascade.enabled ? buildCascade(cascade) : primaryDetector()
  → [T20-T22] if review.enabled: GrpcLlmReviewer + ReviewScheduler(异步复核)
  → [T23-T26] if fusion.enabled: createSensorSource(sensors[*]) -> VideoSensorSource -> MultiSensorPipeline.init
  → 构造 VideoPipeline(*video_source, *detector, ...) (sink 回调: [融合]→画框→写 output.avi + 异步落库 + 告警)
  → 构造 DetectionServiceImpl(*detector)
  → buildAndStartGrpcServer(grpc_cfg, service, addr)   [T8]
  → grpc_thread = server->Wait()
  → pipeline.start()
  → lifecycle.waitUntilShutdown()   // Ctrl+C
  → 关闭: server->Shutdown → grpc join → pipeline.stop → review_scheduler.stop → fusion_stage.stop → db flush/stop
```

业务规则：检测入库；告警——`review.enabled=false` 时沿用原版 `label=="person" && confidence>0.8` 立即写“安全帽缺失”告警，
`review.enabled=true` 时改为命中 `review.trigger` 的候选异步送审、**复核确认后才写告警**（失败/超时默认不告警，可用 `alert_on_failure` 兜底）；
结果视频写 `output.avi`（MJPG，始终用本地结果，不等复核）。
启用融合时（`fusion.enabled=true`，且 `sensors:` 非空）额外多一条：sink 最前置先 `fuse()`（在拷贝上就地改写），
其后画框/写视频/落库/告警/复核看到的都是**融合后**的 `confidence`，原始视觉置信度保留在 `vision_confidence`；
融合**不改变框**、不新增类别（除非 `emit_sensor_only=true` 且传感器自带框）。

---

## 7. 构建与运行（**每次改完必做**）

```powershell
# 1) 配置（config.yaml 被锁，需手工维护）
#    - 若曾用 config.new.yaml 模板：按需重新 copy（该模板可能已被删除，见第 11 节）
#    - 启用级联：把 config/cascade.example.yaml 的 cascade: 段合并进 config/config.yaml
#    - 级联的二级分类器需在 config/model_config.yaml 的 models: 里以 role=classifier 注册
#    - 启用大模型复核：把 config/review.example.yaml 的 review: 段合并进 config/config.yaml
#      （复核服务端需实现 proto/review.proto 的 ReviewService）
#    - 启用多模态融合：把 config/sensors.example.yaml 的 sensors: + fusion: 段合并进 config/config.yaml
#      （sensors: 只列雷达/红外; 视频仍由 video: 段描述。无硬件时 backend=stub 可直接联调）

# 1.5) 初始化数据库表结构（首次部署或表缺失时执行）
#      docker 环境已由 docker/init_db.sql 在首次初始化数据卷时自动建表；
#      裸机/已有实例可手动导入（幂等，可重复执行）：
#      mysql -h <host> -u root -p < scripts\schema.sql

# 2) 重跑 CMake（新增 .cpp 依赖 GLOB，不复现会漏编译）
cd build
cmake ..
cmake --build . --config Release -j

# 2.5) [T27] 阶段自检：一次跑完 Phase A~D（不需要模型/传感器/复核服务/数据库, 秒级）
cmake --build . --target phase_selftest -j
./phase_selftest               # Windows: .\phase_selftest.exe   退出码 0 = 全部通过

# 3) 运行
./CVInfer-Gate                 # Windows: .\CVInfer-Gate.exe

# 3.5) [T27] 用“测试配置”运行（把 cascade / review / sensors+fusion 逐段打开）
./CVInfer-Gate --config config/config.test.yaml
#      也支持环境变量: CVINFER_CONFIG=config/config.test.yaml ./CVInfer-Gate
#      ./CVInfer-Gate --help 可看用法
#
# ⚠ 重要: CMake 的 POST_BUILD 会把**源码目录的 config/ 整个覆盖**到 build/config/,
#   所以不要改 build/config/config.yaml(一 build 就被还原); 要改就改源码 config/config.yaml,
#   或者用 --config 指向另一个文件(这正是 config/config.test.yaml 存在的原因)。

# 4) 客户端测试（T8：地址/超时可配）
./grpc_client 127.0.0.1:50051 5000
```

---

## 8. 已知遗留 / 待核查（后续 bug 修复候选）

> 这些是本轮**未闭环**或**需要验证**的点，新对话可直接从这些开始。

| 编号 | 描述 | 风险 | 建议方案 |
|---|---|---|---|
| R-1 | ~~`VideoPipeline` 多 worker 下 **结果完成顺序不保证与帧序一致**~~ | 输出视频“时间抖动” | **✅ T29 已修**：`sinkLoop()` 按 `frame->seq` 小缓冲重排（`kMaxReorder=8`，遇被丢弃的 seq 不死等）；单 worker/无丢帧时退化为直通 |
| R-2 | `YoloPostProcessor` 被多个 worker + gRPC **并发调用** | 若其内部有共享可变状态则竞争 | 已确认逻辑用局部变量（`calculateIoU/applyNMS`），**建议复核**无成员写入 |
| R-3 | `DBWriter::flush()` 用 `sleep(5ms)` **忙等** | 忙等不优雅 | T11 已改为同时等待队列+批缓冲；仍可进一步换成条件变量 |
| R-4 | `InferenceEnginePool::init` 失败回滚 | 部分引擎已建 | 复核回滚路径与 `active_workers_` 计数一致 |
| R-5 | `RtspVideoSource` 未验证（本次主要走 file） | RTSP 断流重连缺失 | 增加重连/超时策略 |
| R-6 | gRPC sync server 并发 > 引擎池容量 | 请求排队至 `timeout_ms` 后返回“引擎繁忙” | 可评估异步 server 或增大池/超时 |
| R-7 | `GrpcConfig.max_message_size_mb` 仅服务端设置 | 客户端仍需一致 | 客户端已设（T8）；Web/Python 网关需同步 |
| R-8 | `config/config.yaml` 需手工 copy | 忘记则新键不生效 | 考虑 CMake `configure_file` 或文档强提醒 |
| R-9 | **视频时间基未真实接入**：`VideoPipeline` 直接持有 `IVideoSource`，不会调用 `VideoSensorSource::read()`，故 `lastTimestampMs()==0`，融合锚点实际回退为 `nowMs()`（“融合时刻”） | 对齐误差 ≈ 推理耗时(数十 ms)，对 50ms 容差可接受；但若将来有逐帧捕获时间戳需求会失真 | 给 `VideoPipeline::Frame` 加 `timestamp_ms` 并透传到 sink，再调 `VideoSensorSource` 回填（改动点已预留在 注释中） |
| R-10 | 雷达/红外均为**骨架**：只有 file/stub backend，无真实 SDK/UDP 接入；关联策略为“贪心 + 标签相容”，雷达无框时退化为纯标签关联（不做方位角空间匹配） | 真实部署下关联准确率未知 | 接入真实传感器后，能用 `azimuth_deg` 做角度关联 / 改用匈牙利匹配 |
| R-11 | `ReplaySensorSource`（backend=file）原先把**源文件时间戳**原样写入 `SensorSample.timestamp_ms`，而融合锚点是 `nowMs()`（steady_clock，与文件里的“相对首行”小数值差几个数量级）→ 与 `ISensorSource.h` 的“时间戳一律用 nowMs()”约定相悖，`aligned` 恒为 0（融合**静默失效**） | 用 file 回放时融合完全不生效，且没有任何报错 | **T27 已修复**（见 `ReplaySensorSource.cpp` 的 `[T27 修正]` 注释）：对外时间戳改为 `nowMs()`，限速仍按源文件的相对间隔，源 ts 只进调试日志；`scripts/sensor_replay_demo.txt` 可回归 |
| R-12 | `MultiSensorPipeline` 的时间缓冲只靠**容量**淘汰（满则丢最旧），没有按 `time_tolerance_ms` 主动 prune；`fuse()` 的 `snapshot()` 是拷贝而非消费 | 老样本常驻内存直到被新样本挤掉；`FusionStats::samples_seen` 语义偏大（= 缓冲存量，而非“本轮相关采样数”），排查时容易误读 | 实测 `samples=492, dropped=236` 恰好等于容量 256 的溢出量（492-256），说明从未主动清理。建议：`fuse()` 后按时间窗 prune，或把 `samples_seen` 改成“窗口内可见采样数”。**评估结论（T27）：暂不处理** —— 不影响正确性（`align()` 按窗口过滤）、不泄漏（容量有上界）、默认容量下开销可忽略；仅当 `buffer_capacity` 上调到千级 / 传感器 ≥50Hz / `fuse()` 进入帧率瓶颈时才值得动 |

| R-13 | `RtspVideoSource::close()` **打不断阻塞中的 `av_read_frame`**；`output.avi` 为流式写入、**无限增长** | ① 摄像头断电/拔线后 decode 线程退出 → 流水线静默停止（程序不退出、gRPC 仍可用）；② 若它正阻塞在 `av_read_frame`，Ctrl+C 会**卡在 `pipeline.stop()`** 的 join（只能 `kill`）；③ 长跑占满磁盘 | ① `AVIOInterruptCB` + 原子 `stop_` 标志，使 `read()` 可被外部打断；② RTSP 断流自动重连（带退避）；③ 结果视频按大小/时长分片。**未修（用户指示：缺陷冻结，后续完善）** |

---

## 9. 与原版的差异一览（速览）

| 维度 | 原版 | 现版 |
|---|---|---|
| 配置 | 扁平字段 | 嵌套分组 + 校验 + env 展开 |
| 日志 | `cout/cerr` | Logger 分级 + CVLOG 宏 |
| 队列 | 固定容量、无统计 | 有界 + 策略 + 统计 + 超时 |
| 引擎 | 单实例共享（竞争）| 引擎池 + RAII |
| 流水线 | 单消费者线程 | 解码 / N worker / sink 三阶段 |
| 生命周期 | 无，硬阻塞 | SignalWatcher + 协调器，优雅关闭 |
| gRPC 启动 | 视频跑完才启动 | 独立线程并行 |
| gRPC 传输 | 默认 4MB、无超时 | 消息上限 + keepalive + deadline |
| 落库 | 同步单连接 | 连接池 + 异步入队 |
| 视频输出 | `output.mp4`(avc1/mp4v) | `output.avi`(MJPG，兼容性更好)；[T29] 容器帧率 = `源fps / frame_interval`，且多 worker 写帧**按 `seq` 保序** |

---

## 10. 本轮改动文件清单

**新增**：`utils/Logger.*`、`utils/LifecycleCoordinator.*`、`inference/InferenceEnginePool.*`、`pipeline/VideoPipeline.*`、`database/ConnectionPool.*`、`service/GrpcServerSetup.*`、`config/config.new.yaml`、`web_gateway/env.example`、`scripts/schema.sql`（T11 建表脚本，与 `docker/init_db.sql` 同步）

**重写**：`utils/ThreadSafeQueue.h`、`database/DBWriter.*`

**修改**：`utils/ConfigParser.*`、`main.cpp`、`service/DetectionServiceImpl.*`、`tests/test_grpc_client.cpp`、`web_gateway/app.py`、`web_gateway/requirements.txt`、`database/DBWriter.h/.cpp`（T11 增量）、`config/config.new.yaml`（T11 新增键）

**T12–T15 新增**：`inference/IModel.h`、`inference/YoloDetector.h/.cpp`、`inference/ModelFactory.h/.cpp`、`inference/ModelPoolManager.h/.cpp`、`config/model_config.new.yaml`

**T12–T15 修改**：`utils/ConfigParser.h/.cpp`（ModelConfig 增字段 + `models:` 解析 + `validateModelConfigs()`）、`pipeline/VideoPipeline.h/.cpp`（改依赖 `IDetector`）、`service/DetectionServiceImpl.h/.cpp`（改依赖 `IDetector`）、`main.cpp`（ModelPoolManager 接线）

**T16–T19 新增**：`inference/CascadeEngine.h/.cpp`、`inference/BehaviorClassifier.h/.cpp`、`inference/ClassificationPostProcessor.h/.cpp`、`config/cascade.example.yaml`

**T16–T19 修改**：`inference/IModel.h`（`IClassifier::classify` 增加状态返回值）、`inference/DetectionResult.h`（新增 `reviewed/sub_*` 元数据）、`inference/ModelFactory.h/.cpp`（classifier 分支）、`inference/ModelPoolManager.h/.cpp`（`buildCascade`）、`utils/ConfigParser.h/.cpp`（`CascadeConfig` + `cascade:` 解析/校验）、`main.cpp`（级联注入 + 统计打印）、`config/model_config.new.yaml`（分类器示例注释）

**T20–T22 新增**：`proto/review.proto`、`src/review/IReviewService.h`、`src/review/GrpcLlmReviewer.h/.cpp`、`src/review/ReviewScheduler.h/.cpp`、`src/utils/RoiUtils.h`、`config/review.example.yaml`

**T20–T22 修改**：`CMakeLists.txt`（生成 `review.pb.*` / `review.grpc.pb.*`）、`utils/ConfigParser.h/.cpp`（`ReviewConfig` + `review:` 解析/校验）、`pipeline/VideoPipeline.h/.cpp`（`ResultCallback` 增 `frame_seq`）、`inference/CascadeEngine.cpp`（改复用 `RoiUtils`）、`main.cpp`（复核接线 + 告警门控 + 统计）

**T23–T26 新增**：`sensor/ISensorSource.h`、`sensor/VideoSensorSource.h/.cpp`、`sensor/ReplaySensorSource.h/.cpp`、`sensor/RadarSensorSource.h`、`sensor/InfraredSensorSource.h`、`sensor/SensorSourceFactory.h`、`fusion/SensorFusion.h/.cpp`、`pipeline/MultiSensorPipeline.h/.cpp`、`config/sensors.example.yaml`

**T23–T26 修改**：`inference/DetectionResult.h`（新增 `fused/vision_confidence/sensor_confidence/distance_m`）、`utils/ConfigParser.h/.cpp`（`SensorConfig`+`FusionConfig` + `sensors:`/`fusion:` 解析与校验）、`main.cpp`（融合阶段接线 + 统计打印）

**T27 新增**：`tests/phase_selftest.cpp`、`config/config.test.yaml`、`scripts/mock_review_server.py`、`scripts/sensor_replay_demo.txt`

**T27 修改**：`CMakeLists.txt`（新增 `phase_selftest` target，只编译 5 个 .cpp，不链 OpenVINO/gRPC/MySQL）、`main.cpp`（`--config/--model-config/--help` + 环境变量注入）、`src/sensor/ReplaySensorSource.cpp`（R-11 时间戳修正）

> 注：`SensorConfig`/`FusionConfig` 刻意放在 `utils/ConfigParser.h`（与 `CascadeConfig`/`ReviewConfig` 同例），
> 使 `sensor/` 与解析器共用同一份定义，避免 `utils` 反向依赖 `sensor`。

---

## 11. 兼容性保证（别踩坑）

- 旧版三段式 `config.yaml`（`video/database/grpc`）**仍可解析**，缺失的新键走默认值；`validate()` 不会误杀旧配置。
- `getAppConfig()/getModelConfig()` 签名未变；`ModelConfig` 只做**加法**（新增 `name/role/pool_size/acquire_timeout_ms`，均带默认值）。
- 旧的单模型 `model_config.yaml`（`model:` + `thresholds:`）**仍可解析**，会被规整为长度 1 的模型列表；`getModelConfigs()` 返回该列表，`getModelConfig()` 返回首个。
- 旧的 `model_config.yaml` 无需改动即可继续跑；新格式模板见 `config/model_config.new.yaml`。
- 不配置 `cascade:` 段时 `enabled=false`，行为与 T15 单模型路径**完全一致**。
- `DetectionResult` 新增字段均带默认值，且代码全部按字段名访问（无聚合初始化），故旧链路零影响。
- 注意：`config/config.new.yaml` 当前**实际不存在**（见下）；应用层配置只加载 `config/config.yaml`，级联段请用 `config/cascade.example.yaml` 合并。
- `ThreadSafeQueue` 旧用法 `q(10); q.push(x); q.pop(y); q.stop();` 仍可用。
- `Detect` RPC 的 proto（`proto/inference.proto`）**未改动**；新增的 `proto/review.proto` 是**独立服务**（本仓库为客户端），不影响既有 `Detect`。
- 不配置 `review:` 段时 `enabled=false`，告警沿用原版本地规则，行为与 T19 一致；启用后**只改告警**链路。
- `VideoPipeline::ResultCallback` 由 `(frame, detections)` 变为 `(frame_seq, frame, detections)`（增参，属**破坏性签名变更**，但仓库内唯一调用点是 `main.cpp`，已同步）。
- 复核结论与 ROI 始终带 `frame_seq`，异步结果按帧回收；`ReviewScheduler::submit` 非阻塞（有界 DropOldest）。
- `config/review.example.yaml` 是**模板**，程序不会自动加载；需手工合并进 `config/config.yaml`。
- `models:` 中 `role: reviewer` 现直接报错（复核不走本地模型）；原有以 `reviewer` 注册的配置需迁移到 `review:` 段。
- `ModelRole::Reviewer` 枚举值保留但当前未使用（供未来本地 VLM 复核预留）。
- **不配置 `sensors:`/`fusion:`** 时 `fusion.enabled=false` → `MultiSensorPipeline` 根本不创建，sink 里 `fusion_stage==nullptr`，**零拷贝直通**，行为与 T22 完全一致。
- 启用融合后：`VideoPipeline` 签名/内部**未改**；融合只在 sink 最前置阶段作用，且对检测结果做**拷贝**后改写（回调入参是 `const`）。
- `DetectionResult` 新增字段仍为**加法且带默认值**，无聚合初始化，旧链路零影响。
- `sensors[].kind=video` 在校验层**直接报错**（视频必须由 `video:` 段描述），避免两套时间基并存。
- `fusion.level` 目前只允许 `decision`（校验层拒绝其它值），因为特征级融合需改推理链路。
- `config/sensors.example.yaml` 是**模板**，程序不会自动加载；需手工合并进 `config/config.yaml`。
- 雷达/红外的回放文件格式：`[timestamp_ms,]label,confidence[,distance_m[,x,y,w,h]]`（`#` 注释/空行忽略；多目标写成同一时间戳的多行）。

---

## 12. 下一步（可选）

1. 从第 8 节 **R-1 ~ R-13** 里挑风险项排期修复。**（R-1 已由 T29 修掉）** 当前用户指示：**缺陷冻结**，先推进功能/工程完善，故 R-5 / R-9 / R-10 / R-13 均暂缓；解冻后优先级建议：R-13（RTSP 长跑稳定性，与 R-5 一套）> R-9（视频时间基）> R-10（真实传感器接入）。
1.5. 任何改动到 Phase A~D 的代码后，先跑 `./phase_selftest`（秒级、零依赖、退出码即结论），再跑真实链路。
2. 多模态后续：真实雷达/红外 SDK 接入、按方位角的空间关联（R-10）、特征级融合。
3. 若引入 INT8/异步推理（README 的拓展方向），需同步调整 `OpenVINOEngine` 与引擎池。

---

## 13. 后续路线图：多模型级联 + 大模型复核 + 多模态（T16+）

> 用户已拍板的决策（新对话务必遵守）：
> ① 只对**灰区**（主模型置信度处于 `[min_conf, max_conf)` 的目标）做复核；
> ② 大模型复核**复用 gRPC**（不引入 HTTP/新依赖）；
> ③ 复核结果**只影响“告警/入库”**，不阻塞画框与实时视频输出；
> ④ 二级模型用**分类器（单 ROI，易落地）**；
> ⑤ 多模态先做**决策级（后期）融合**。

### Phase A（T12–T15，已完成）= 地基
- 新增 `IModel/IDetector/IClassifier` + `ModelRole` + `DetectStatus`。
- `YoloDetector` 把旧链路（引擎池+后处理+标签）封装为 `IDetector`，**行为等价**。
- `ModelConfig` 扩展 `name/role/pool_size/acquire_timeout_ms`；`ConfigParser` 支持 `models:` 列表且兼容旧单模型格式。
- `ModelPoolManager` 按配置构建模型（**每模型各自一套 `InferenceEnginePool`**），提供 `primaryDetector()`。
- `VideoPipeline`/`DetectionServiceImpl` 只依赖 `IDetector`，**级联引擎只要实现 `IDetector` 即可无缝接入，流水线无需再改**。

### Phase B（T16–T19，已完成）= 级联主筛 + 二级复核
- **T16** `inference/CascadeEngine.*`（**实现 `IDetector`**）：主模型 detect → 命中灰区（`conf∈[min_conf,max_conf)` 且 label∈trigger_labels）→ ROI 外扩裁剪 → 二级 `IClassifier::classify` → 决策：
  - 确认（`accept_label` 空则仅看 `accept_conf`）→ 保留并回写 `sub_class_id/sub_confidence/sub_label`；可选 `boost_on_confirm`；
  - 否决 → `drop_rejected=true` 丢弃，否则保留但带 `reviewed=true` 标记；
  - 复核不可用（Busy/Failed）→ **保留主结果（降级，绝不误杀）**。
- **T17** `inference/BehaviorClassifier.*`（`IClassifier`）+ `inference/ClassificationPostProcessor.*`（**自动判定** softmax：已是概率分布则直用，否则按 logits 处理，避免二次 softmax 造成置信度失真）；`ModelFactory` classifier 分支落地。
- **T18** `ModelPoolManager::buildCascade(cfg)` 用已注册模型组装级联（primary 缺省=首个 detector；secondary 空/未找到=退化为单模型）；`main` 按 `app_cfg.cascade.enabled` 选择 detector；关闭时打印级联统计。
- **T19** `CascadeConfig`（在 `utils/ConfigParser.h`，同时供解析与运行使用）+ `cascade:` 段解析 + `validate()` 校验 + 运行期 `Stats`（primary/triggered/confirmed/rejected/skipped，原子累加）。

> **对原计划的偏差（重要）**：
> ① `CascadeEngine` 落在 **`inference/`**（原写 `pipeline/`）—— 它实现 `IDetector`、组合模型，与流水线线程/队列无关，放 `inference/` 更贴切。
> ② `IClassifier::classify` 由 `Classification classify(roi)` 改为 **`DetectStatus classify(roi, out)`**（与 `IDetector::detect` 对称），以便级联区分 Busy/Failed 并降级。
> ③ 级联配置段解析进 **`AppConfig::cascade`**；因 `config/config.new.yaml` 实际不存在，模板改为 **`config/cascade.example.yaml`**（需手工合并进 `config/config.yaml`）。
> ④ `DetectionResult` 新增 `reviewed/sub_class_id/sub_confidence/sub_label`，供 Phase C 告警/入库使用。

### Phase B 遗留 / Phase C 衔接点
- CascadeEngine 目前**只做决策级“保留/丢弃 + 标注”**；是否据此写库/告警仍由 sink 现有逻辑（`label=="person" && conf>0.8`）决定 → **T22 才接大模型复核结果**。
- `boost_on_confirm` 若开启会把主置信度替换为二级置信度，可能影响现有 `conf>0.8` 告警阈值；默认关闭。
- 二级分类器需真实模型才能验证（本地无 `helmet_cls` 时 `cascade.enabled` 保持 false）。
- ✅ 已闭环：`models:` 中声明 `role: reviewer` 现由 `ConfigParser::validateModelConfigs()` 直接报错并提示改用 `review:` 段（T20–T22），不再走到 `ModelFactory`/`init` 失败。

### Phase C（T20–T22，已完成）= 大模型异步复核
- **T20** `review/IReviewService.h`（抽象 + `ReviewRequest/ReviewResult/ReviewStatus`）+ `review/GrpcLlmReviewer.*`：
  `proto/review.proto` 定义独立服务 `review.ReviewService`（本仓库为**客户端**）；ROI → JPEG → `Review`，带 `deadline=timeout_ms`；
  错误分类为 `Unavailable/Timeout/Failed`（`CreateChannel` 惰性连接，故服务未启动时 `init()` 仍成功）。
- **T21** `review/ReviewScheduler.*`：`submit()` 非阻塞入队（**有界 `DropOldest`**，决不阻塞 sink）；N 个 worker 出队 → `review()` → 回调；
  结果 `ReviewOutcome` 带 `frame_seq`，**按帧回收**；`stop()` 关闭队列并 join，排空在途复核。
- **T22** 接入告警链路（`main.cpp`）：
  - `VideoPipeline::ResultCallback` 增 `frame_seq` 参数（sink 透传 `Result.frame->seq`）；
  - 命中 `review.trigger` 的候选 → `roi_utils::crop` 取 ROI → `submit()`（**不阻塞**）；
  - 复核**确认才 `db_writer.writeAlert`**（在 worker 线程回调里异步入队）；否决不告警；不可用时按 `alert_on_failure` 兜底（默认 false）；
  - 画框/写视频/**检测入库**仍用本地结果，实时进行，**不受复核影响**。
- 新增 `utils/RoiUtils.h`（`expandAndClamp/crop`），`CascadeEngine` 与复核共用，消除重复实现。
- `ReviewConfig`（`AppConfig::review`）+ `review:` 段解析/校验 + `ReviewScheduler::Stats` 统计。

> **对原计划的偏差（重要）**：
> ① 复核服务的 proto 落在 **`proto/review.proto`（独立 package/service）**，不改 `inference.proto`，故 `Detect` 链路零影响；
>    CMake 生成 `review.pb.*`/`review.grpc.pb.*` 并纳入构建。
> ② 告警**门控范围**：仅 **`writeAlert`** 被复核门控；**检测入库 `writeDetections` 仍立即写入**（原始记录），
>    因为门控入库需按帧缓冲检测、与“不阻塞”冲突（如需，可后续用 `frame_seq` 缓冲实现）。
> ③ `ResultCallback` 增参属**破坏性签名变更**，但仓库内唯一调用点是 `main.cpp`，已同步。
> ④ 复核触发条件用 `review.trigger{labels,min_conf,max_conf}`（与 `cascade.trigger` 同形，默认 `[0.5,1.0)`），
>    与级联灰区（默认 `[0.4,0.9)`）**相互独立**：级联先本地收窄，复核再远端确认告警。

### Phase C 遗留 / Phase D 衔接点
- 复核服务端（`review.ReviewService`）不在本仓库，需另行部署；未部署时 `review.enabled=true` 会全部走 `Unavailable` 分支。
- 复核结果目前**只写告警**，未回写 `DetectionResult`（帧早已 sink/落库），如需回填需 `frame_seq` 缓冲。
- 多 worker 下 R-1 帧序问题在异步复核下更明显：复核按 `frame_seq` 关联，如需严格有序，sink 侧加小缓冲重排。
- `review` 与 `cascade` 可组合（级联本地筛 + 复核远端确认），但两者 ROI/阈值需分别调参。

### Phase D（T23–T26，已完成）= 多模态接入与决策级融合
- **T23** `sensor/ISensorSource.h`：`SensorKind` / `SensorTarget`（置信度+像素框+测距+方位角）/ `SensorSample`（统一 `timestamp_ms`）；
  `sensor::nowMs()` 用 **steady_clock**（不用 system_clock，避免校时导致“时间倒流”）；
  `sensor/VideoSensorSource.*` 把旧 `IVideoSource` 适配为“带统一时间戳的视频视图”——**非拥有**（打开/关闭仍由 `main` 负责，避免二次 `open()` 破坏底层状态）。
- **T24** `sensor/ReplaySensorSource.*`：两种 backend ——
  `file`（逐行回放 `[ts,]label,conf[,dist[,x,y,w,h]]`，**有显式时间戳时按相对首行限速**，也可写成同一时间戳多行表示多目标）与
  `stub`（按 `rate_hz` 合成确定性缓动目标；雷达无框 → 标签关联分支，红外带框 → IoU 关联分支，两条路径都能验到）。
  限速用 **10ms 分段睡眠**，保证 `close()` 能打断 `read()`，否则 `stop()` 的 `join` 会卡住。
  `RadarSensorSource`/`InfraredSensorSource` 均继承它；`sensor/SensorSourceFactory.h` 按 `kind` 构建并完成 `open()`（与 `ModelFactory` 同构）。
- **T25** `fusion/SensorFusion.*`（纯函数、无状态）：
  ① `align()` 保留 `|ts-anchor| <= time_tolerance_ms` 的采样；
  ② 关联：传感器带框（红外）用 `IoU >= match_iou` 且标签相容，传感器无框（雷达）退化为“标签相容”（任一侧标签空即相容）；
     **贪心**匹配，每个传感器目标最多被一个视觉框占用（避免一个雷达点被多个视觉框争用）；
  ③ 融合：`conf' = (1-w)*vision_conf + w*sensor_conf`（`w=sensor_weight`），原值存入 `vision_confidence`。
  未关联的传感器目标默认**只计数**；仅当 `emit_sensor_only=true` **且目标自带框**才追加（无框的雷达点强行输出会造出假框）。
- **T26** `pipeline/MultiSensorPipeline.*`：每路非视频传感器一个 **poller 线程** → 有界时间缓冲（满则 **丢最旧**，永不阻塞）；
  `fuse()` 由 **sink 线程**调用：一次内存快照 → 纯计算融合（锁只在快照的短临界区）；`stop()` 先 `close()` 再 `join`（幂等）。
  接入方式：**不改 `VideoPipeline`**，在 `main` 的 sink 回调**最前置**叠加 `fuse()` —— 与“级联实现 `IDetector`”“复核实现 `IReviewService`”同一种“挂在既有抽象上”的思路。
- 配置：`sensors:`（列表）+ `fusion:`（`FusionConfig`）解析与校验；模板 `config/sensors.example.yaml`；
  退出时打印“融合统计: frames/samples/dropped/aligned/targets/matched/unmatched_sensor/emitted_sensor_only/poll_errors”。

> **对原计划的偏差（重要）**：
> ① 原计划 `SensorConfig` 放在 `sensor/` 下，实际放在 **`utils/ConfigParser.h`**（与 `CascadeConfig`/`ReviewConfig` 同例），
>    使解析器与运行期共用一份定义，避免 `utils` 反向依赖 `sensor`。
> ② 视频**不写进 `sensors:`**（校验层直接报错）：视频仍由 `video:` 段描述，`VideoSensorSource` 只是“视频视图/时间基挂点”，
>    否则会出现两套视频时间基。
> ③ `MultiSensorPipeline` 落在 **`pipeline/`**（原写 `pipeline/MultiSensorPipeline` 或改 `VideoPipeline`）—— 选择“**不动 `VideoPipeline`**”
>    的方案，降低对已验证主链路的回归风险。
> ④ 对齐锚点当前回退为 `nowMs()`（原因见 R-9），代码已预留“优先用视频时间戳”的分支。

### Phase D 遗留 / 后续衔接点
- **R-9**：视频逐帧时间戳未透传，锚点=融合时刻；如需严格对齐，给 `VideoPipeline::Frame` 加 `timestamp_ms`（注解已写在 `VideoSensorSource.h`）。
- **R-10**：雷达/红外只有 file/stub backend；真实接入只需继承 `ReplaySensorSource` 覆写 `read()`（时间戳/限速/生命周期沿用）。
- 融合目前是**目标级（检测框）**关联；若要做“区域/轨迹级”融合（如雷达测速 + 视觉跟踪），需要引入跟踪 id（本项目暂无跟踪器）。
- 跨机多模态需统一时钟：`nowMs()` 是本机单调时钟，**跨进程/跨机不可比**；真实部署应约定统一时间源（如 NTP/PTP 或由主节点打戳）。
- `fusion` 与 `cascade`/`review` 的执行顺序（重要）：级联发生在 **`IDetector` 内部**，融合发生在 **sink 最前置**，
  故实际顺序为 **级联（detector）→ 融合（sink）→ 画框/落库/告警/复核**，即融合看到的是**级联后**的结果。
  若将来想让“融合后再级联/再复核”，需把融合前移到 detector 之前（当前不做）。

### 关键设计约束（勿忘）
- 级联只对**灰区 ROI** 触发，避免每帧全量二次推理拖垮帧率。
- 大模型复核**异步、有界、带超时**，绝不阻塞解码/推理/sink 线程。
- 帧序（R-1）在引入异步复核后更需注意：复核按 `frame_seq` 关联；如需严格有序，sink 侧加小缓冲重排。
- 新增 `.cpp` 后**必须重跑 cmake**（GLOB）。

### 配置演进（进 `config/config.new.yaml`；`config.yaml` 被锁）
```yaml
# models: [ {name, role, xml_path, bin_path, labels_path, input_width, input_height,
#            conf, nms, pool_size, acquire_timeout_ms}, ... ]   # T12–T15 已支持; role=classifier 于 T17 落地
cascade:                          # T16–T19 (已实现; 模板见 config/cascade.example.yaml)
  enabled: false
  primary: yolov8_detector
  secondary: helmet_classifier
  trigger: { labels: ["person"], min_conf: 0.4, max_conf: 0.9 }   # 只复核灰区
  roi_padding: 0.10
  accept_label: "with_helmet"     # 空 = 仅按置信度判定
  accept_conf: 0.50
  drop_rejected: true
  boost_on_confirm: false
review:                           # T20–T22 (已实现; 模板见 config/review.example.yaml)
  enabled: false
  endpoint: "llm:50052"
  timeout_ms: 3000
  queue_size: 128
  worker_threads: 2
  trigger: { labels: ["person"], min_conf: 0.50, max_conf: 1.00 }
  roi_padding: 0.10
  prompt: "判断该人员是否未佩戴安全帽"
  alert_type: "安全帽缺失"
  alert_on_failure: false
sensors:                          # T23–T24 (已实现; 模板见 config/sensors.example.yaml)
  - { kind: radar,    name: radar_front, backend: stub, rate_hz: 10, labels: ["person"] }
  - { kind: infrared, name: ir_1,        backend: file, path: config/ir_targets.txt }
  # 注意: 视频**不**写在这里(由 video: 段描述, 并自动作为融合时间基的挂点)
fusion:                           # T25–T26 (已实现; 模板见 config/sensors.example.yaml)
  enabled: false
  level: decision                 # 仅支持 decision
  time_tolerance_ms: 50
  match_iou: 0.30
  sensor_weight: 0.35
  emit_sensor_only: false
  adopt_sensor_label: false
  buffer_capacity: 256
```

---

## 14. [T27] 阶段自检工具 / 配置注入 / 复核服务 mock

### 14.1 为什么需要它

主程序要跑起来，需要**四类外部资源同时就位**：OpenVINO 模型、视频文件、MySQL、以及（Phase C 的）复核服务端。
任何一环缺失，跑一次都只能看到“某一段”行为，而且**默认配置并未打开 B/C/D**，所以“跑过了”不等于“验证过了”。

T27 用“**分层抽象 ⇒ 可以被替换**”这一既有设计，把每一层都换成测试内注入的假实现：

| 阶段 | 真实实现 | 自检用替身 | 验证的接缝 |
|---|---|---|---|
| A 抽象层 | `YoloDetector` + `ModelPoolManager` | `FakeDetector` | `IModel/IDetector` 契约 + `DetectionResult` 默认值 |
| B 级联 | `YoloDetector` + `BehaviorClassifier` | `FakeDetector` + `FakeClassifier` | 灰区判定、ROI 外扩、确认/否决/降级三分支 + 统计 |
| C 复核 | `GrpcLlmReviewer`（需服务端） | `FakeReviewer`（按 `frame_seq%4` 脚本化） | 异步调度、有界队列、超时/不可用分支、兜底告警策略 |
| D 融合 | 雷达/红外硬件 | `SensorFusion`（纯函数，喂合成样本）+ `ReplaySensorSource(backend=stub)` | 时间对齐、IoU/标签关联、加权融合、poller 线程 + 有界缓冲 |

### 14.2 怎么跑

```bash
cd build
cmake .. && cmake --build . --target phase_selftest -j
./phase_selftest          # 退出码 0 = 全部通过；输出 [ OK ]/[FAIL] 逐项断言
```

不需要模型、不需要数据库、不需要 `config.yaml`；`phase_selftest` **不链接** OpenVINO / gRPC / MySQL，
所以即使主程序链接失败（例如 gRPC 环境没配好），自检仍能单独跑通。

### 14.3 端到端（Phase A~D 真实链路）怎么跑

```bash
# 终端 1：复核服务 mock（可选，但强烈建议——否则 Phase C 只会看到 unavailable=N）
pip install grpcio grpcio-tools
mkdir -p build/pyproto
python3 -m grpc_tools.protoc -I proto --python_out=build/pyproto \
    --grpc_python_out=build/pyproto proto/review.proto
python3 scripts/mock_review_server.py --port 50052        # --mode confirm|reject|drop|error|auto

# 终端 2：主程序（注意用 --config 指向测试配置，不动 config/config.yaml）
cd build && ./CVInfer-Gate --config config/config.test.yaml
```

要点：
- 配置里 `review.enabled: true` 且 `endpoint` 指向 mock → 日志/CSV 里能看到**复核确认后才告警**；
- `sensors:` 用 `backend: stub` → **不需要任何硬件**就能看到 `融合统计: matched>0`；
- mock 的 `--mode` 与 `--delay-ms` 可把 C 阶段四条分支（确认/否决/超时/不可达）逐一跑出来；
- 库不可用时记录落 `build/db_fallback.csv`（`DBWriter` 的降级路径），**不影响推理与融合验证**。

### 14.4 告警语义（最容易弄反，已用自检钉死）

`review.alert_on_failure` **不是**“是否告警”的总开关，它只管**拿不到复核结论**（超时 / 不可用）时怎么办：

| 复核结果 | `alert_on_failure=false`(默认) | `alert_on_failure=true` |
|---|---|---|
| 调用成功 + **确认** | **告警** | **告警** |
| 调用成功 + **否决** | **不告警** | **不告警** |
| 超时 / 服务不可用 | 不告警 | **告警**（兜底） |
| 其它错误（Failed） | 不告警 | **告警**（兜底） |

“否决 → 不告警”是复核的**核心价值**（杀误报）；“拿不到结论 → 可选兜底”才是 `alert_on_failure` 的作用。
自检里 C1（10 个任务 = 3 确认 + 3 否决 + 2 超时 + 2 不可用）断言 **7** 条告警，
C2（`alert_on_failure=false`，4 个任务）断言 **1** 条告警 —— 两者交叉就把上表钉死了。
（首次写自检时 C1 被我误写成 10/10，正是被 C2 的正确结果反证出来的：
“断言写错”只有真跑起来才暴露 —— 这恰好说明 T27 的自检有存在的价值。）

### 14.5 实测观察（来自 `build/db_fallback.csv`，对调参很有用）

- 这段测试视频里 `person` 的置信度**大量落在 [0.4, 0.9)**，但**几乎不超过 0.8**（历史峰值 ~0.79）。
  ⇒ 原版告警规则 `label=="person" && confidence>0.8` 在此视频上**几乎永远不会触发**；
  而 `review.trigger = [0.50, 1.00)` 会命中大量帧 —— 这正是 Phase C 想验证的“本地阈值不可靠 ⇒ 远端复核”的现实动机。
- `person` 的框常常接近**满画面**（如 `0,383,719,1268`）。这会影响 Phase D 的 IoU 关联：
  `stub` 红外的框在画面上方（y≈120），与这种“满画面下半部”的 person 框**垂直不相交**（IoU=0）；
  故用红外验证时要么改用 `backend: file` 手工给重叠框（`scripts/sensor_replay_demo.txt`），
  要么先用**雷达**（无框 → 标签关联）确认 `matched>0`。
- `alert,` 记录在 CSV 中**尚不存在** —— 与上面第一条一致（本地规则从未命中）。

### 14.6 设计要点（为什么这样写）

- **不碰被锁的 `config/config.yaml`**：新增 `--config`（+ `CVINFER_CONFIG` 环境变量）注入配置路径，
  测试配置单独放 `config/config.test.yaml`；同时绕开 CMake POST_BUILD 覆盖 `build/config/` 的坑。
- **自检不依赖主程序链接**：新 target 只编译 5 个 `.cpp`（`CascadeEngine`/`ReviewScheduler`/`SensorFusion`/
  `MultiSensorPipeline`/`ReplaySensorSource` + `Logger`），链路里没有 OpenVINO / protobuf / MySQL。
- **断言取“确定性的量”**：融合里“一个传感器目标只被一个视觉框占用”的贪心顺序、
  复核里 `frame_seq%4` 的脚本化结果、stub 传感器的正弦缓动，都是**确定性**的，所以自检可以精确断言
  而不是“看起来差不多”。
- **R-11 修正的必要性**：`ReplaySensorSource(file)` 原先输出源文件时间戳，与 `nowMs()` 锚点不可比，
  融合会**静默失效**（`aligned=0`，无任何报错）。这类“时间基不一致”的 bug 只有把两端放在同一个
  断言里才抓得到 —— 自检 D-1 就是拿合成样本把 `align()` 的窗口行为钉死。
- **仍是加法**：`phase_selftest` 是新 target，`main.cpp` 只新增了参数解析分支，默认行为（不带参数）与 T26 完全一致。

### 14.7 端到端实测记录（2026-09-27，首次跑通 Phase C + D）

```bash
python3 scripts/mock_review_server.py --port 50052 --mode auto      # 终端 1
./CVInfer-Gate --config config/config.test.yaml                     # 终端 2
# database 未建表 -> 记录降级 build/db_fallback.csv
```

```text
流水线统计: decoded=1293 dropped=1162 processed=130 emitted=130
复核统计: submitted=63 dropped=0 reviewed=63 confirmed=24 rejected=29 timeout=0 unavailable=10 failed=0
融合统计: frames=130 samples=492 dropped=236 aligned=75 targets=75 matched=66 unmatched_sensor=9 emitted_sensor_only=0 poll_errors=0
```

**结论**：
- **Phase C 闭环**：`confirmed(24) + rejected(29) + unavailable(10) = reviewed(63)`，
  且 `grep -c '^alert' build/db_fallback.csv` == **24** —— 告警数 == confirmed 数，
  证明“**确认才告警；否决/不可用都不告警**”在真实链路成立（与自检 C1/C2 的断言一致）。
- `unavailable=10` 是**真实触发**的兜底分支（复核服务短暂不可达，例如 mock 重启窗口）；
  因 `alert_on_failure=false`，这 10 条**没有**产生告警。
- `timeout=0`：mock 零延迟，未触发超时分支；想验超时用 `--delay-ms 2000`（> `review.timeout_ms`）。
- `dropped=0`：63 个任务未撑满 `queue_size=64`；接真实 VLM（每次几百 ms）时这里会涨，属“实时优先”的预期行为。
- `decoded=1293 -> processed=130`：抽帧（`target_fps`）生效。
- 送审的置信度是**融合后**的值（`fuse()` 在 sink 最前置就地改写 `confidence`）；
  与 `fusion.enabled` 开关两次对比即可看出差异。
- ROI JPEG 偏大（CSV 里 `jpeg=116007B` 即 116KB/次）：因 person 框近乎满画面 + `roi_padding=0.10`；
  真实 VLM 场景应下调 `roi_padding` / 限制 ROI 尺寸，这是带宽与延迟的主要成本项。
- `samples=492, dropped=236` 与 R-12 对应：缓冲从未主动清理，只靠容量淘汰。

---

## 15. Phase A~D 整体验收清单（T12–T27）

> 按顺序跑；**P0 = 不能回归的底线**，P1 = 新增能力的独立开关，P2 = 可选项。
> 每条都给了“通过标准”，不满足就是回归，回来报现象即可。

### P0-1 构建（新增 `.cpp` 依赖 GLOB，务必先重跑 cmake）
```bash
cd build && cmake .. && cmake --build . -j
```
通过标准：`CVInfer-Gate`、`grpc_client`、`phase_selftest` 三个目标全部编过。

### P0-2 阶段自检（零依赖，秒级）
```bash
./phase_selftest
```
通过标准：末行 `结果: 50 项通过, 0 项失败`，退出码 0（`echo $?`）。

### P0-3 向后兼容基线（**最重要**的一条）
不带任何参数直接跑（等价于 cascade/review/sensors/fusion 全关）：
```bash
./CVInfer-Gate
```
通过标准：
- 单模型模式（有 `使用检测器: ...`，**无** `级联模式` 字样）；
- **无** `[ReviewScheduler] 初始化完成`、**无** `[MultiSensorPipeline] 初始化完成`；
- 退出时**不打印** `复核统计:` / `融合统计:`（组件根本没创建 ⇒ 零开销）；
- 检测记录照常落库（或降级 CSV）、`output.avi` 照常生成、本地告警规则（`person && conf>0.8`）生效。

### P0-4 gRPC 与流水线并行（T7/T8）
流水线跑着的同时，另开一个终端：
```bash
./grpc_client 127.0.0.1:50051 1000
```
通过标准：能拿到检测结果，且**不**让流水线停帧（引擎池隔离 + gRPC 独立线程）。

### P0-5 优雅退出（T6）
主程序终端 `Ctrl+C`。通过标准（顺序固定）：
`正在关闭服务...` → `流水线统计: ...` →（若启用）`复核统计:`/`融合统计:` → `结果视频已保存至 output.avi` → `已安全退出。`

### P1-1 只开复核
`config/config.test.yaml` 里 `review.enabled: true`，`sensors:` 清空 + `fusion.enabled: false`。
通过标准：有 `复核统计: submitted/reviewed/confirmed/rejected`，**无** `融合统计:`。

### P1-2 只开融合（关复核）
`review.enabled: false`，`fusion.enabled: true` + 两路 `backend: stub`。
通过标准：`融合统计: matched > 0`，`output.avi` 正常，日志无 `[ReviewScheduler]`。

### P1-3 组合（Phase C+D，已实测通过，见 14.7）
```bash
python3 scripts/mock_review_server.py --port 50052 --mode auto      # 终端 1
./CVInfer-Gate --config config/config.test.yaml                     # 终端 2
```
通过标准：`confirmed + rejected + unavailable == reviewed`，且 `grep -c '^alert' build/db_fallback.csv == confirmed`。

### P2-1 超时 / 兜底分支
mock 加 `--delay-ms 2000`（大于 `review.timeout_ms`）。
通过标准：`复核统计: timeout > 0`；`alert_on_failure=false` 时这些超时**不**产生 alert。

### P2-2 落库路径与降级回传（T11）
先不建表跑（落 CSV），再：
```bash
mysql -h 127.0.0.1 -u root -p < scripts/schema.sql
./CVInfer-Gate --config config/config.test.yaml
```
通过标准：检测/告警进 MySQL；`db_fallback.csv` 内容被**回传并清理**（T11 的“恢复后自动回传”）。

### P2-3 Web 网关（T10）
```bash
python3 web_gateway/app.py
```
通过标准：浏览器上传图片返回带框结果；地址全部来自环境变量（无硬编码 IP）。

---

## 16. [T28] RTSP 真实接入 与 [T29] 输出视频正确性修正

> 背景：T27 之后第一次接**真实信号源**（局域网 RTSP 摄像头）。
> 跑通后暴露出两个**互相独立**的问题：一个是环境/性能（丢帧），一个是代码 bug（回看视频 8 倍速）。本节把结论固化下来。

### 16.1 [T28] RTSP 接入

**改动**

| 改动 | 文件 | 说明 |
|---|---|---|
| 独立的 RTSP 运行配置 | `config/config.rtsp.yaml`（新增） | 用 `--config <绝对路径>` 加载，**不动**被锁的 `config/config.yaml`；只需改 `video.source_path` 一行 |
| 分辨率未知时**降级**而非退出 | `src/main.cpp` | `RtspVideoSource::open()` 可能成功但 `codec_ctx_->width==0`；旧逻辑把 `0x0` 交给 `VideoWriter` → 打不开 → **`return -1` 直接退出**（现象：“流连上了却启不来服务”）。现改为“只跳过写结果视频”，推理/落库/告警/gRPC 全部照常 |
| 源真实帧率 | `src/video/IVideoSource.h`、`RtspVideoSource.h/.cpp` | 新增 `getFps()`：RTSP 取 `avg_frame_rate`（退回 `r_frame_rate`）；`IVideoSource::getFps()` 带**默认实现 0**，故 File/Fake/测试替身**零改动** |

**RTSP 调参要点（踩坑记录）**

1. `source_type` **必须一起改成 `rtsp`**。只改 `source_path` 会走 `FileVideoSource`（`cv::VideoCapture`，默认 UDP、无超时）—— 也能连上，但容易丢包/卡死，且不打印分辨率。
2. **RTSP 是实时流，抽帧只能用 `frame_interval`，不能用 `target_fps` 限速**：
   `target_fps: 0 + frame_interval: N` = 全速读流、每 N 帧推理 1 帧（**不累积延迟**）；
   而 `target_fps: 10` 会让解码线程 `sleep_until`，没读走的帧堆在 TCP 缓冲里 → **延迟越积越大**（画面越来越“过去时”）。
3. `output.avi` 不会自动结束（RTSP 没有“读完”的概念），MJPG 约 1~3MB/s → 长跑盯磁盘，验证用 `timeout 60 ./CVInfer-Gate ...`。
4. `RtspVideoSource` 会把**含密码的完整 URL** 打进日志（`[RtspVideoSource] RTSP 流打开成功: ...`）→ 生产注意日志安全。
5. 摄像头若为 **H.265/HEVC** 而 FFmpeg 无对应解码器，`open()` 会失败 → 走“视频源打开失败”降级分支（gRPC 仍启动）。
6. 先用 `ffprobe -rtsp_transport tcp -i "<url>" -select_streams v -show_streams` 验地址/鉴权，比启动整个程序快得多。

### 16.2 [T29] 输出视频正确性（本次实测暴露的 bug）

**实测数据（RTSP，约 56 秒）**

```text
流水线统计: decoded=577 dropped=374 processed=203 emitted=203

decoded 577 / 56s = 10.3fps         <- 入队速率 = 源 30fps / frame_interval 3
processed 203 / 56s = 3.6fps        <- 推理吞吐（硬件上限）
dropped 374 = 577 - 203             <- 入队 > 吞吐 -> 队列(24)常满 -> drop_oldest 丢 65%
输出视频 = 203 帧 / 30fps = 6.8s    <- 而素材 56s => 8.2 倍速快放
```

**现象**：本地回看 `output.avi`，“画框效果特别差”。

**根因（三条叠加，其中 a/b 是代码 bug，c 是配置/性能）**

| # | 根因 | 说明 |
|---|---|---|
| a | **容器 fps 写死 30**（RTSP 分支取不到元数据，`main.cpp` 里 fallback `30.0`） | 203 帧 ÷ 30fps = **6.8s**，素材却是 56s → **8.2 倍速**，人一晃而过、框自然“对不上” —— **主因** |
| b | **多 worker 乱序写帧**（R-1） | 前后帧时间不单调 → 画面回跳/抖动/闪烁 |
| c | 抽帧 1/3 + 丢 65% | 相邻两帧实际相隔约 300ms，位移大，每帧的框都显得“偏” |

> 注：**丢帧本身不是 bug**，是“实时优先”的设计（宁可丢帧也不让延迟累积）。
> 早前 file 测试其实同样丢了 90%（`decoded=1293 dropped=1162`），只是当时没看回放画质。

**修复**

| 修复 | 文件 | 内容 |
|---|---|---|
| 容器帧率 = **抽帧后**有效帧率 | `src/main.cpp` | `fps = source_fps / frame_interval`（若 `target_fps` 更严则以它为准）；启动打印 `视频帧率: 源=30fps, frame_interval=3, 输出容器=10fps` |
| sink 按 `frame_seq` **保序** | `src/pipeline/VideoPipeline.cpp` | 小重排缓冲：期望 `next_seq`，乱序结果先暂存，能连续吐出就吐出；暂存数超过 `kMaxReorder=8`（说明有帧已被 `drop_oldest` 丢掉、对应 seq 永远等不到）则放弃等待、按 seq 从小到大吐出，保证不无限阻塞；单 worker/无丢帧时退化为“来一帧吐一帧”，**零额外延迟** |

**仍要注意**：fps 修正只在**不丢帧**时才准。实测吞吐 3.6fps ⇒ 应把 `frame_interval` 提到 `ceil(30/3.6) ≈ 10`
（入队 30/10 = 3fps ≤ 吞吐），此时输出容器 fps 自动变 3，视频时长 ≈ 实际时长，回看才自然。

**验证判据**

```bash
ffprobe -v error -show_entries stream=avg_frame_rate,nb_frames,duration -of default=nw=1 output.avi
# 修好后: avg_frame_rate ≈ 3/1, duration ≈ 实测时长(而不是 6.8s)
```

### 16.3 本轮新增缺陷：R-13

见第 8 节 R-13（`close()` 打不断阻塞中的 `av_read_frame` + `output.avi` 无限增长）。
**用户指示：缺陷冻结，后续完善时再处理。** R-13 与 R-5（断流不重连）属同一套改动。

### 16.4 状态变更

- **R-1**：`待办` → **✅ 已修**（T29 sink 保序）。
- **R-5**：仍 ❌ 未修，与 R-13 合并处理。
- **新增 R-13**（见第 8 节）。
- 新增文件：`config/config.rtsp.yaml`。
- 修改文件：`src/main.cpp`、`src/video/IVideoSource.h`、`src/video/RtspVideoSource.h/.cpp`、`src/pipeline/VideoPipeline.cpp`。

---

## 17. 架构总览（新对话 / 新人 30 秒对齐）

### 17.1 主动脉（T5 起**从未改动**）

```text
 解码线程                推理 worker × N                sink 线程
+--------------+  frame_queue  +--------------+  result_queue  +---------------------------+
| IVideoSource | ----有界/丢最旧->|  IDetector   | ------------->|  ResultCallback (回调)     |
| File / Rtsp  |               | (第 3 阶段)  |               |                           |
+--------------+               +--------------+               +---------------------------+
                                  ^    ^                          |  (1) 融合(最前置)
                                  单模型 YoloDetector            |  (2) 画框/写 output.avi
                                  级联 CascadeEngine             |  (3) 检测入库 -> DBWriter
                                  (同接口, 可互换)               |  (4) 告警 -> 复核 -> 入库

  （独立线程）gRPC: DetectionServiceImpl(IDetector&) -- 与流水线共用同一个 detector
```

### 17.2 四个挂载点（Phase B/C/D 全部“挂”在这里，**均不碰主动脉**）

| # | 挂点 | 抽象接口 | 能力 |
|---|---|---|---|
| (1) | `detector` 位置（换实现） | `IDetector` | 级联主筛 + 二级复核（Phase B） |
| (2) | sink **最前置** | 直接调 `MultiSensorPipeline::fuse()` | 多模态决策级融合（Phase D） |
| (3) | sink 的**告警分支** | `IReviewService` | 大模型异步复核（Phase C） |
| (4) | `VideoPipeline` **之外** | `ISensorSource` | 传感器 poller + 时间缓冲（Phase D） |

> 一句话记住：**所有新能力都是“挂在既有抽象上”，而不是“插进主链路里”** —— 这就是“改了 29 个任务但不乱”的原因。

### 17.3 线程与关闭顺序

| # | 线程 | 创建者 | 关闭顺序 |
|---|---|---|---|
| 1 | main（阻塞在 `waitUntilShutdown`） | 进程 | 最后 |
| 2 | SignalWatcher（`sigwait`） | `main`（**必须最早**，以继承信号掩码） | 最后 |
| 3 | gRPC `server->Wait()` | `main` | **第 1 个** |
| 4 | gRPC 内部线程池 | gRPC 库 | 随 server |
| 5 | decode | `VideoPipeline` | 第 2 个 |
| 6 | worker × N | `VideoPipeline` | 第 3 个 |
| 7 | sink | `VideoPipeline` | 第 4 个 |
| 8 | DBWriter 后台 | `DBWriter` | **最后**（先 flush） |
| 9 | 复核 worker × N | `ReviewScheduler` | 第 5 个 |
| 10 | 传感器 poller × N | `MultiSensorPipeline` | 第 6 个 |

```text
关闭顺序: server->Shutdown -> join(grpc) -> pipeline.stop(join decode->worker->sink)
          -> review.stop -> fusion.stop -> db.flush -> db.stop -> video.close
```

> ⚠ 卡住的唯一原因：`pipeline.stop()` 要 join decode 线程，而它可能阻塞在 `av_read_frame`，
> 且 `video.close()`（可打断它）排在后面 —— 即 R-13。

### 17.4 开关 -> 行为映射（**每个开关“关掉即回到原版”**）

| 段 / 开关 | 默认 | 打开后新增 | **关掉时** |
|---|---|---|---|
| `video.source_type` | `file` | `rtsp` -> `RtspVideoSource`（FFmpeg，强制 TCP + 5s stimeout） | `FileVideoSource` |
| `pipeline.worker_threads` | — | N 个推理线程 + 每模型 N 个引擎 | — |
| `cascade.enabled` | `false` | `CascadeEngine` 替换 detector | 单模型，**零开销** |
| `review.enabled` | `false` | `ReviewScheduler` + gRPC reviewer | 告警回退本地 `person && conf>0.8` |
| `sensors:[ ]` + `fusion.enabled` | `false` | `MultiSensorPipeline` + poller 线程 | sink **零拷贝直通** |
| `database.*` | — | 连接池 + 异步批量；失败降级 CSV 并回传 | — |
| `grpc.*` | — | 消息上限 / keepalive / deadline | — |

### 17.5 验证状态（三色）

**已闭环**
- `phase_selftest` **50/50**（零依赖、秒级）
- P0-3 向后兼容基线（单模型、无 B/C/D 组件、零开销）
- **Phase C 端到端**（mock）：`confirmed(24)+rejected(29)+unavailable(10)=reviewed(63)`，且 `alert 数 == confirmed 数`
- **Phase D 端到端**：`aligned=75/75, matched=66`
- **T28**：RTSP 能连、能推理、能落库降级、能优雅退出

**部分闭环**
- Phase B 级联：**仅自检背书**（本仓库无 `role=classifier` 模型）
- 复核服务端：仅 Python mock，**真 VLM 未接**
- MySQL：`cv_infer.detections` **表不存在** -> 一直在写 `build/db_fallback.csv`（P2-2 未做）
- `web_gateway` 未联调；P2-1 超时分支 / P2-2 降级回传未验
- **T29 待回归**（需按 `frame_interval: 10` 重跑）

**未修缺陷**
- R-5（RTSP 断流不重连）、R-9（视频时间基未透传）、R-10（真实传感器）、R-13（见第 8 节）、R-3 / R-6（小项）

---

## 18. [T30] 推理性能旋钮配置化（device / performance_mode / num_threads）

### 18.1 动机（承接 T28/T29 实测）
T28 实测 RTSP 吞吐仅 **3.6fps**（yolov8n/640，4 核 CPU 正常应 10~30fps）。线索就在
`OpenVINOEngine::init()`：原先**硬编码** `core_.compile_model(model, "AUTO")`，且
`pipeline.worker_threads = 2` ⇒ **2 个引擎同时、各自按 LATENCY 开满物理核** ⇒ 线程超订互抢，
总吞吐不升反降。**缺的不是算力，是可观测/可调的旋钮。**

### 18.2 改动（**默认值 = 改造前行为，一个属性都不设**）

| 文件 | 改动 |
|---|---|
| `utils/ConfigParser.h` | `ModelConfig` 增 3 字段：`device("AUTO")` / `perf_mode("")` / `num_threads(0)` |
| `utils/ConfigParser.cpp` | `models:` 与旧 `model:` 两条解析路径都支持这 3 个键（枚举值大小写容错）；`validateModelConfigs()` 增校验 |
| `inference/OpenVINOEngine.cpp` | 用 `ov::AnyMap` 传 `PERFORMANCE_HINT` / `INFERENCE_NUM_THREADS`，改为 `compile_model(model, device, props)`；启动打印**实际生效值** |
| `config/model_config.yaml` | 模板里加注释块（**默认注释掉** -> 行为不变） |

> 两个防坑点：① 值统一用 `std::string`/`int`，不用 C 字面量（`ov::Any` 对 `const char*` 的处理跨版本不一致，
> 可能抛 Bad cast）；② 键名用字符串 `"PERFORMANCE_HINT"` 而非 `ov::hint::*` 对象，避开版本命名空间差异。

### 18.3 怎么用（`config/model_config.yaml` 的 `models[]` 内）

```yaml
models:
  - name: yolov8_detector
    role: detector
    # ...
    device: CPU                    # AUTO(默认) | CPU | GPU | GPU.0 | NPU
    performance_mode: throughput   # 留空(默认 LATENCY) | latency | throughput
    num_threads: 2                 # 0/省略 = 物理核数
```

### 18.4 建议的调参实验（**按顺序，每步只改一处**）

| 步 | 配置 | 预期 |
|---|---|---|
| 0 | 现状（3 个键都不写） | baseline：记 `processed/秒`，启动日志应打 `device=AUTO, performance_mode=(default), num_threads=(default)` |
| 1 | `performance_mode: throughput` | **最可能见效**：OpenVINO 按“总线程≈物理核”分内部 stream，消除互抢 |
| 2 | `device: CPU` | 去掉 AUTO 的插件探测（若机器无 GPU/核显不可用） |
| 3 | `num_threads: <核数/2>` | 2 个 worker 平分核，防超订 |
| 4 | `worker_threads: 1` + `performance_mode: throughput` | 单推理吃满核，有时比并发 2 个**总吞吐更高** |
| 5 | `num_threads: 1` + `worker_threads: N` | 最省核/低延迟，吞吐优先 |

### 18.5 验证

```bash
cd build && cmake --build . -j
# ★ model_config.yaml 不在 --config 管辖范围内, 且 POST_BUILD 会把源码 config/ 覆盖到 build/config/,
#   所以两个路径都指向源码目录, 避免“改了源码却跑的是 build 里的旧副本”
timeout 60 ./CVInfer-Gate \
    --config /home/JMP/CVInfer-Gate/config/config.rtsp.yaml \
    --model-config /home/JMP/CVInfer-Gate/config/model_config.yaml
```

判据：
- 启动日志出现 `[OpenVINOEngine] device=..., performance_mode=..., num_threads=...`（确认参数真的落地）；
- 退出日志 `流水线统计: ... processed=N`，`N / 时长` 相比 baseline 提升即为生效；
- 同步看 `dropped`：吞吐上来后 `dropped` 会下降（因为入队 10fps 不再超过吞吐）。

### 18.6 后续（笔记 §12 第 3 条的剩余部分）
- **INT8 量化**：需重新导出 IR（`ovc` / `nncf`），不改代码；
- **异步推理（`start_async/wait`）**：单引擎内流水化，需改 `OpenVINOEngine::infer` + 引擎池借还语义（`EngineGuard` 需保证“借出期间 finish 完”），属**结构性**改动，待 T31+ 评估。
- 若最终确认瓶颈是**云主机核太少**，则以上均无效，应回到“降 `frame_interval` + 提高 `worker_threads`”的配置面解决。

---

## 19. [T31] 视频源配置交叉校验 + VideoWriter 降噪

> 触发：T30 编译通过后首次运行（`./CVInfer-Gate`，无参数），冒出两类“看着吓人但都不致命”的问题。
> 顺带记录了 T30 已验证：启动日志出现 `[OpenVINOEngine] device=AUTO, performance_mode=(default), num_threads=(default)`
> （打了两次 = `pool=2`），说明**默认路径零回归**。

### 19.1 现象与根因

| 现象 | 根因 | 处置 |
|---|---|---|
| `[FileVideoSource] 无法打开视频文件: rtsp://...` + `[rtsp @ ...] method DESCRIBE failed: 404` | `video.source_type: file` 但 `source_path` 是 RTSP 地址 → 走了 `FileVideoSource`（底层同样是 FFmpeg，故日志里出现 `[rtsp @ ...]`）。**404 说明 8554 确实有 RTSP 服务在应答，只是没有 `/live/stream` 这个流**（路径名不对，不是连不上） | 配置改 `source_type: rtsp` + 核对流路径；并新增**启动即报错**的交叉校验（见 19.2） |
| 两条 `[ERROR:0] global ./modules/videoio/... GStreamer / CV_IMAGES` | T28 的降级路径仍在 `cv::Size(0,0)` 上**构造** `VideoWriter` → OpenCV 挨个试后端：GStreamer 断言 `frameSize.width>0`、CV_IMAGES 把 `output.avi` 当图片序列找 `%d` 编号 | **尺寸未知时根本不构造 writer**（延迟 `open()`）→ 噪声消失，也避免留下一个“打开却写不出”的空 writer |

### 19.2 改动

| 文件 | 改动 |
|---|---|
| `utils/ConfigParser.cpp` | `validate()` 增交叉校验：`source_type=file` 却给 `rtsp://` 前缀（或相反）→ **启动失败并给出可执行提示** |
| `main.cpp` | `video_writer` 改为延迟 `open()`，尺寸未知直接跳过；新增 `wrote_video` 标志，末尾**只有真写过**才打印“结果视频已保存至 output.avi” |
| `video/RtspVideoSource.cpp` | `open()` 失败时保留 `avformat_open_input` 的错误码并用 `av_strerror` 转可读文案，附 mediamtx 常见原因自查清单（原来只打“无法打开 RTSP 流: <url>”，且 `!= 0` 把错误码丢了） |

> 效果：本次这种手误以后会在**启动时一行报错拦住**，而不是走错视频源、再冒出一堆 OpenCV 噪声。

### 19.3 对既有验收的影响

- 正常 file 路径（有分辨率、writer 打开成功）：日志与之前**完全一致**（含 `结果视频已保存至 output.avi`），P0-3 / P0-5 不受影响。
- 降级路径：不再打印 `[ERROR:0]`，也不再生空 `output.avi`。
- ⚠️ 任何 `source_type: file` + `rtsp://` 路径的配置，**现在会启动失败** —— 这正是本次想要的行为。

### 19.4 附：RTSP 404 的真实语义（mediamtx）

**`method DESCRIBE failed: 404 Not Found` 不等于“地址写错”。** mediamtx 的规则是：
**一个 path 只有在“当前存在发布者(推流端)”时才可被拉取**；没有发布者时，哪怕路径名拼对了，DESCRIBE 也返回 **404**。
（对比：端口/服务根本不在，FFmpeg 打的是 `Connection refused`，不是 404。
所以“本机 `127.0.0.1:8554` 拿到 404”恰恰证明 **mediamtx 本体在跑**。）

排查顺序（本次实战顺序）：

1. `ss -tlnp | grep 8554` → 期望 `0.0.0.0:8554` 或 `*:8554`。
   若是 `127.0.0.1:8554`（mediamtx 的 `rtspAddress` 绑了回环），**公网推流永远进不来**，只有本机能连 —— 与本次现象完全吻合，是首要嫌疑。
2. `ss -tnp | grep 8554` → 有没有**已建立**的连接（= 推流端在线）。
3. 看 **mediamtx 自己的日志**：推流成功时会出现该 path 的 `is publishing to path ...`；没有则推流压根没上来。
4. **隔离测试（最省事）**：在本机用文件先推一路到同一路径名；能拉通就说明 mediamtx / 路径 / 端口都对，问题 100% 在公网推流端：
   ```bash
   # 终端 A
   ffmpeg -re -stream_loop -1 -i <某mp4> -c copy -f rtsp rtsp://127.0.0.1:8554/live/stream
   # 终端 B
   ffprobe -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live/stream   # 应返回流信息而非 404
   ```
5. 公网推流还要查：安全组/防火墙放行 8554/TCP、NAT 映射、以及**推流端用的 path 是否与拉流端一致**。

### 19.5 本次实录（结论：程序无 bug）

- 推流端地址：`rtsp://106.15.88.152:8554/live`（**path = `live`**），且当时**尚未开推流**。
- 而当时跑的 `config/config.yaml` 里写的是 `rtsp://127.0.0.1:8554/live/stream`（path = `live/stream`）→ 即使推了流也永远 404。
- `config/config.rtsp.yaml` 里本来写的就是 `.../8554/live` ✅，所以**直接用它跑即可**；
  `config/config.yaml`（锁定文件）里那两行（`source_type: file` + `/live/stream`）需手动改。
- 因此本次现象 = “没推流 + 路径不一致”，**程序行为正确**（源打不开 -> 降级不写结果视频 -> gRPC 照常启动）。
- 头号经验：**先 `ffprobe` 确认能拉，再启程序**；404 先查“有没有人在推”，别先怀疑代码。

---

## 20. [T32] 迁移到本地 WSL（环境适配）

> 因阿里云 2H2G 严重限制吞吐（实测 2.65fps、连解码只 7.2fps），迁到本地 WSL 继续开发。
> 运行环境由用户自行装好，本节只记**配置审查结论 + 改动 + 需要注意的坑**。

### 20.1 路径对照（本文档其余章节的命令示例以此为准）

| 项 | 阿里云 | 本地 WSL |
|---|---|---|
| 项目根 | `/home/JMP/CVInfer-Gate` | **`/home/jmp/CVInfer-Gate`**（注意小写 jmp） |
| 构建目录 | `.../build` | 同上。**换机后必须删掉重建**：旧 `CMakeCache.txt` 里是旧机的绝对路径 |
| 文件系统 | ext4 | ext4 ✅（**切勿放 `/mnt/c/...`**：drvfs/9p 的 I/O 慢数倍，会污染性能结论） |

> 本文档其它章节里的 `/home/JMP/...` 均为阿里云路径，在 WSL 上请自行替换（或统一写 `~/CVInfer-Gate`）。

### 20.2 配置审查结论（为何“网络配置几乎不用改”）

- `CMakeLists.txt` **无任何硬编码路径**（全靠 `find_package` / `pkg_check_modules` / `find_path`）→ 只需 `source /opt/intel/openvino/setupvars.sh` 后 cmake。
- `web_gateway/app.py` 早已不写死 IP（T10：参数全走环境变量，默认 `localhost:50051`）。
- `mediamtx.yml`：`rtspAddress: :8554` 绑全接口；`paths` 仅 `all_others` ⇒ 任意 path 可推；`authInternalUsers` 对 `any` 开放 publish/read ⇒ 无鉴权障碍。

⇒ **只要 mediamtx / MySQL / 本程序全在 WSL 内，所有 `127.0.0.1` 一个都不用改。**

需人工注意的项：

| 文件 | 键 | 处置 |
|---|---|---|
| `config/config.yaml`（锁定） | `video.source_type` / `source_path` | 手改 `rtsp` + `.../8554/live`（原为 `file` + `/live/stream`） |
| `config/config.rtsp.yaml` / `config.test.yaml` | `database.password` | 与本地 MySQL 对齐（compose 里是 `<已移出仓库，见 docker/.env>`，已一致 ✅） |
| `config/config.test.yaml` | `video.source_path: test.mp4` | 需 `build/` 下存在该文件 |
| `docker/docker-compose.yml` | `3306:3306` | 本机若装 MySQL 会端口冲突（当前未装 ⇒ 无冲突）；另注意 `../test.mp4` 不存在时 Docker 会**创建同名目录** |

### 20.3 本次改动（用户已批准）

| 文件 | 改动 |
|---|---|
| `mediamtx.yml` | `api: no` → **`api: yes`**：以后 `curl -s http://127.0.0.1:9997/v3/paths/list` 就能一眼看出“哪条 path 有人在推、几个读者”，不必翻日志（仅 127.0.0.1/::1 有 api 权限） |
| `web_gateway/.env` | 需由 **用户执行** `cp env.example .env`（`.env` 受安全策略保护，工具无法创建）。注意 `app.py` 用 `load_dotenv()`，**必须在 `web_gateway/` 目录下启动** |
| `.gitignore` | 补：`mediamtx.log` / `mediamtx`(二进制) / `recordings/` / `db_fallback.csv` / `web_gateway/.env` |

### 20.4 ⚠️ 重要发现：数据库不可用 ⇒ **程序直接退出**（不是降级）

**现象**：MySQL 不起时，程序会刷屏重试约 **20 秒**（`max_retries`=10 × sleep 2s）后打 `数据库初始化失败!` 并 `return -1`，**gRPC 都不会起**。

**代码依据**：
- `ConnectionPool::init()` 是**启动时同步连接**，失败按 `max_retries` 次重试（每次 sleep 2s），全失败 `return false`；
- → `DBWriter::init()` false → `main.cpp` `return -1`；
- 而 DBWriter 那套降级机制（`spillToFallback` + `replayFallback`）只在 **“曾经连上、后来断了”**（`markUnhealthy`）时才生效——**从没连上过根本走不到**。

**影响**：本地开发/性能测试（不想起 DB）会被硬卡住；也与“DB 是可选依赖”的设计意图相矛盾。

**解法（建议）**：用 docker 起 MySQL，`docker/init_db.sql` 会自动建库建表（`detections` / `alerts`），顺带把“表不存在”一并解决：

```bash
cd ~/CVInfer-Gate/docker && docker compose up -d mysql-db
docker compose exec mysql-db mysql -uroot -p<已移出仓库，见 docker/.env> -e "SHOW TABLES FROM cv_infer;"
```

**待完善（未做）**：让 `DBWriter::init()` 容忍连接池失败——失败时不返回 false，而是置 `db_healthy_=false` 进入降级，
并在 `shouldProbe()` 到点时**惰性重建连接池**，使“DB 后起”也能自动接上。这才与现有重试/回传机制自洽。
（注意 `ConnectionPool::close()` 会把 `closed_` 置 true 且 `acquire()` 永远返回 nullptr，所以不能只把 `return false` 改成 `return true`。）

### 20.5 迁移后回归顺序

1. `nproc; free -h; df -T .` —— 确认 WSL 分到多少核 / 是否 ext4
2. `cd ~/CVInfer-Gate && rm -rf build && mkdir build && cd build && cmake .. && cmake --build . -j`（**换机必须重新 cmake**）
3. `./phase_selftest` —— 零依赖、秒级，验证环境与 Phase A~D
4. `cd ../docker && docker compose up -d mysql-db` —— 建库建表
5. 推流 → `ffprobe` 验 → 跑主程序（§19.5 顺序）
6. **重测基线**：阿里云那组 2.65fps / `decoded` 7.2fps 结论作废，必须在本地重测后再决定是否上 `throughput` / INT8

### 20.6 其它待清理项

- `.gitignore` 文件内容疑似被 markdown 围栏（`` ```text `` / `` ``` ``）包裹，建议清掉那两行纯装饰。
- 根目录调试残留：`_chk.txt`、`_chk.txtgit`、`check.txt` 可删。
- `docs/` 是空目录。
- `phase_selftest` 不链接 OpenVINO/gRPC/MySQL，只编 5 个 `.cpp` —— 换环境后它是最快的“环境体检”手段。

### 20.8 [T34] ⚠️ 优雅关闭完全失灵：`sigwait` 方案被“库线程”破功（已修：改 `sigaction` + self-pipe）

**现象**：`device: CPU` 修好后，程序能正常起来、推理/落库/写视频全正常（`output.avi` 7.5 MiB），
但 **Ctrl+C / `kill -TERM` 之后一条关闭日志都没有**（没有 `收到信号`/`正在关闭服务`/`流水线统计`）。

**排查过程（关键是“先分清是它自己死了还是信号没被处理”）**：

1. `timeout 20 ./CVInfer-Gate ...; echo $?` → **退出码 124**。124 = timeout 到点 ⇒ 进程**活过了 20 秒**
   ⇒ 直接排除“它自己崩了”（那会是 139）。⇒ **信号根本没被我们的 `sigwait` 消费**；
2. 逐线程查信号掩码（`/proc/<pid>/task/*/status` 的 `SigBlk`，期望含 `4002` = SIGINT(bit1)+SIGTERM(bit14)）：

```
27150  CVInfer-Gate        0000000000004002   ← main(已屏蔽)
27152  CVInfer-Gate        0000000000000000   ← ★掩码为空
27153  CVInfer-Gate        0000000000000000   ← ★
27154  CVInfer-Gate        0000000000000000   ← ★
27155  CVInfer-Gate        0000000000000000   ← ★
27156  CVInfer-Gate        0000000000004002   ← 我们的 sigwait 线程
27172  default-executo...  0000000000004002
27183  grpcpp_sync_ser...  0000000000004002
```

3. **四个空掩码线程的 TID(27152-27155) 比我们的 sigwait 线程(27156) 还小** ⇒ 它们在
   `SignalWatcher::start()` **之前**就被创建；而那段代码(读两个 YAML + Logger 初始化)**一处都不建线程**
   ⇒ 是**链接进来的库在静态初始化阶段(`main()` 之前)自己开的线程**（TBB/OpenCV/gRPC/protobuf 之一）。
   ⇒ 内核把 SIGTERM 投给这些线程 → **默认动作 → 进程当场死**，`sigwait` 永远等不到。

**根因**：`sigwait` 方案有一个**无法自保的前提** —— “进程里不存在任何未继承屏蔽掩码的线程”。
而线程是别人（第三方库）创建的，我们管不了。⚠️ 这是**潜伏 bug**，不是 WSL 特有：阿里云那次
`timeout 90` 能优雅退出（掩码恰好没被破），纯属运气（那台 OpenVINO 版本不同，库线程行为也不同）。

**修复**：`SignalWatcher` 改为 "`sigaction` + self-pipe"：

```
sigaction(SIGINT/SIGTERM) -> handleSignal() 只做 write(self_pipe[1], sig)   // 全 async-signal-safe
                           -> 监听线程阻塞 read(self_pipe[0]) -> 回调(普通线程里打日志/requestShutdown)
```

- **为什么这样就好了**：`sigaction` 注册的是**进程级**行为 —— 无论内核把信号投给哪个线程都会进
  `handleSignal`，**不再赌任何库的线程掩码**；
- 不再需要 `pthread_sigmask`（删掉）；handler 不再受限（重活全在监听线程里做）；
- 细节坑：self-pipe 的**写端必须 `O_NONBLOCK`**（管道满时 `write` 阻塞会把信号处理卡死），
  读端 `FD_CLOEXEC`；`sa_flags` 不设 `SA_RESTART`（让 `read` 返回 `EINTR` 后可重试）；
  `stop()` 顺手改为“写一字节唤醒监听线程 + 还原 `SIG_DFL`”，比原来的 `pthread_kill` 干净。
- 回调接口(`start/stop/回调`)不变，`main.cpp` 与业务代码**零改动**。

**排查手法沉淀**：判“进程为什么没打印后续日志”时，**先看退出码**（`timeout 20 cmd; echo $?`）
把“自己崩了(139/134)”和“活着但没响应(124)”一刀切开，比乱猜快得多。

### 20.9 [T34 回归] 本地 WSL 基线（调优前）—— ~19.6 fps，比阿里云快 ~7x

T34 修好后终于拿到了 `流水线统计`（之前拿不到，就是被这个 bug 卡住的）：

```
[WARN ] 收到信号 2, 开始优雅关闭...
[INFO ] 流水线统计: decoded=1332 dropped=1223 processed=109 emitted=109
[INFO ] 复核统计: submitted=61 dropped=0 reviewed=61 ... unavailable=61 ... failed=0
[INFO ] 融合统计: frames=109 samples=61 dropped=0 aligned=53 targets=53 matched=46 unmatched_sensor=7
[INFO ] 结果视频已保存至 output.avi
[INFO ] 已安全退出。               ← 退出码 0
```

| 指标 | 数值 | 说明 |
|---|---|---|
| 观测窗口 | 5.56 s | `服务就绪` 21.096 -> Ctrl+C 26.655 |
| 解码 | 1332 帧 / **~240 fps** | 文件源全速（test.mp4 共 1332 帧 ≈ 44.5s，5.5s 读完） |
| 推理 | 109 帧 / **~19.6 fps** | ← 真正的吞吐瓶颈 |
| 丢帧 | 1223（91.8%） | `queue.policy=drop_oldest` 生效（文件源故意全速灌） |
| 对比阿里云 | ~2.65 fps -> ~19.6 fps（**~7x**） | 多核红利确实吃到了 ✅ |

**结论与下一步**：
- 快了 ~7 倍，**19.6 fps 仍然 < 源 29.96 fps ⇒ 本地依旧丢 ~1/3 帧**，换成 30fps 摄像头照样跟不上；
- 聚合每帧 51ms，折算每个 worker 单帧推理 **~102ms** —— 对 yolov8n FP32 偏慢，
  正是 T30 预警的“`pool_size=2` + 默认 LATENCY ⇒ 两个引擎各自开满核互抢”；
- 下一步（一次只改一个变量）：`performance_mode: throughput`，然后再看 `num_threads`；
- 旁证：`unavailable=61` 是因为本机没起 mock LLM 服务(127.0.0.1:50052)，复核走兜底，**符合预期**
  （阿里云那次有 mock 服务，所以两者不可直接比）；`融合统计` 有真实对齐/关联数据
  (aligned=53, matched=46) ⇒ T23-T26 在新机器上复现 ✅。

**小噪音**：连按两次 Ctrl+C 会打两遗 `收到信号 2`（`requestShutdown()` 本身幂等，无害）。
若嫌乱，可在回调里先判 `lifecycle.isShutdownRequested()` 再打日志；
或者做成“第二次信号 = 强制退出”（nginx/docker 惯例，但会跳过落库 flush，未做）。

### 20.10 ⚠️ 算力被 WSL 扼住了：`processors=4` ⇒ 只用掉宿主机 16 个逻辑核中的 4 个

```bash
$ nproc
4
$ cat /proc/cpuinfo | grep "model name" | head -1
model name      : 12th Gen Intel(R) Core(TM) i7-12650H
$ cat /mnt/c/Users/jmp/.wslconfig
[wsl2]
memory=8GB
processors=4
swap=4GB
```

**关键结论**：i7-12650H 是 **6 P-core + 4 E-core = 10 核 / 16 逻辑线程**，
而 `.wslconfig` 写着 **`processors=4`** ⇒ **WSL 只拿到 4 个逻辑核，白扔 12 个**。

- ⚠️ 更正本节初稿写的“4 逻辑 CPU = 2 物理核 + SMT”：那只是 **WSL 给的合成拓扑**，
  **不是宿主机真相**；⇒ **19.6 fps 不是这台机器的天花板，只是 1/4 台机器的成绩**；
- 之前“两个引擎互抢核”的预判，**在 4 vCPU 的口径下成立**（2×2=4 线程正好占满 4 个逻辑 CPU），
  但一旦放开 vCPU，`pool_size`/`worker_threads`/`num_threads` 这些 T30 旋钮就**重新变得有意义**
  （可以把 `worker_threads`/`pool_size` 提到 4）；
- Alder Lake 有 **AVX-VNNI**（无 AVX-512）⇒ **INT8 量化有真实收益**，是下一步的主要杠杆。

**行动**：改 `.wslconfig`（`processors=12` 左右，给 Windows 留几个）→ Windows 侧 `wsl --shutdown`
→ 重开 WSL → （先拉起 MySQL/MediaMTX 容器，见 §20.4 的“DB 启动强耦合”）→ 重测基线。
**预期推理吞吐翻 2~3 倍，越过 30fps 实时线。**

### 20.11 [T30/T32] 放开 WSL CPU 配额（4->8 vCPU）-> **首次越过实时线：31.6 fps**

`.wslconfig` 把 `processors` 从 4 提到 **8**（没给满 16，怕宿主机卡）：

| 配置 | vCPU | 窗口 | processed | **吞吐** | 解码 |
|---|---|---|---|---|---|
| 调优前 | 4 | 5.56 s | 109 | **19.6 fps** | ~240 fps |
| 放开后 | 8 | 2.85 s | 90 | **31.6 fps** | ~215 fps |

- ⚠️ **结论修正**：首发那次（窗口 2.85s）算得 31.6 fps，**但那是短窗口 + 含预热的偏乐观值**。
  随后 3 次 `timeout -s TERM 5` 测得 processed = **143 / 141 / 140**（±1%，极稳），
  按窗口 4.7~5.3s 折算 ⇒ **约 28~30.5 fps** ⇒ **正确的说法是“基本压在 30fps 线上”，不是舒适达标**
  （待用确切时间戳复核，见下）；
- 提升 **1.61x**（非线性 2x：内存带宽 + HT 效率使然）；
- 单帧推理 **102ms -> 63ms**；按 WSL 报的拓扑（4 核 x 2 线程）推测 OV 默认 `num_threads`=4，
  2 引擎 x 4 线程 = 8 ⇒ **正好填满 vCPU，配置自适配，无需手调**；
- **丢帧问题实质上解决了**：文件源仍丢 85%（解码 215fps 全速灌，属预期）；
  但换 30fps 摄像头时 `processed(31.6) > 源(30)` ⇒ `dropped` 应接近 0。

#### ⚠️ 测量方法纠正：**不能用 `timeout 30` 做窗口**

本项目的源是**有限长文件**（test.mp4 = 1332 帧，解码 215fps ⇒ 约 6.2s 读完）。文件读完后
管线进入**排空 + 空闲**状态，`processed` 还会被排空阶段推高，而墙钟继续走 ⇒ `processed/30`
会被严重低估（估算只有 ~9 fps，错得离谱）。**正确做法**：窗口必须**落在“解码仍在进行”期间**（≤ 约 6s），
用日志时间戳算窗口：

```
fps = processed / (收到信号的时间戳 - 服务就绪的时间戳)
```

推荐固定用 `timeout -s TERM 5`，再跑 2~3 次取稳定值。（若需长窗口，先用
`ffmpeg -stream_loop 9 -i test.mp4 -c copy test_long.mp4` 造一个长源。）

> ⚠️ **注意：`timeout -s TERM 5` 的 5s 是从“进程启动”算起的**，所以真实工作窗口 = 5s - 启动耗时
> （实测启动约 0.3s）⇒ 约 4.7s。**必须从日志里取两个时间戳才能算准**，不能直接拿 5 当分母。

**另一个思路：直接看 `decoded`**。文件源全速解码，解码速率固定（~215-240 fps），
所以 `decoded` 本身就是“窗口有多长”的代理量（`decoded / 解码速率 = 窗口`），
可以拿来交叉校验时间戳算出来的窗口对不对。

### 20.12 本地基线定档：**30.2 ± 0.7 fps（压线）** + 两条测量铁律

基准脚本已入库：**`scripts/bench.sh`**（固定 `timeout -s TERM 5` 窗口，用日志时间戳算真实窗口）。
3 次干净数据：

| 次 | 窗口 | decoded | 解码速率 | processed | **吞吐** |
|---|---|---|---|---|---|
| 1 | 4.653 s | 1165 | 250.3 fps | 144 | **30.9 fps** |
| 2 | 4.674 s | 1186 | 253.7 fps | 143 | **30.5 fps** |
| 3 | 4.669 s | 1143 | 244.8 fps | 136 | **29.1 fps** |

**基线 = 30.2 ± 0.7 fps（±2.4%）**，对比源 **29.9551 fps** ⇒ **刚刚压线：实时余量 ≈ 0**。

> **铁律 1：`解码速率` 是机器的“体温计”。** 解码是单线程纯 CPU 活，速率应当恒定；它一掉
down，就是整机被降频了。实测抓到过一次：解码 **193 fps** 那一次，吞吐同步掉到 **24.1 fps**
>（正常是 250 fps / 30.9 fps）⇒ **那次是环境问题（功耗/温度/后台负载），不是程序问题**，
> 该次数据直接丢掉重跑，不要当基线。
>
> **铁律 2：窗口必须从日志时间戳算**（`收到信号` − `服务就绪`，实测约 4.6s）。
> 直接拿 `timeout` 的 5s 当分母会低估 ~8%（因为 5s 是从进程启动算的，含 ~0.3s 初始化）。

**根因更正**：本机 CPU 是 **i7-12650H（Alder Lake，并未硬件 NPU）**，
所以 T33 的触发条件不是“有 NPU 硬件”，而是 **WSL 里没有加速设备 + wheel 里带了 NPU 插件**，
AUTO 一让插件去枚举就崩。（先前写的“本机是带 NPU 的 Core Ultra”是错的，已改。）

**下一步先看一个数（30 秒，不改配置）——并发度 `%CPU`：**

- **≈800%（8 核跑满）** ⇒ 纯算力瓶颈 ⇒ **只有 INT8 能带来真实收益**（Alder Lake 有 AVX-VNNI，预计 1.5~2x）
- **≈400~500%（一半核空闲）** ⇒ 存在**串行瓶颈**（sink/锁/引擎借用）⇒ 先修串行，别急着上 INT8
- 参考：改成 4 路并发（`pool_size:4` + `num_threads:2` + `worker_threads:4`）**预期只有 ±10%**，
  因为总算力不变，只是换一种分配方式——**先看这个数，再决定值不值得做**。

### 20.13 并发度（%CPU）才是真 KPI：514% -> 天花板约 51 fps

显式写 `num_threads: 4`（2 引擎）后：**32.8 fps（+9%；三次 33.1/33.0/32.2 整段高于旧的 30.9/30.5/29.1）**，
但 **%CPU 一动不动（514% / 514% / 512%）** —— 同样的 CPU 干了更多活 ⇒ 瓶颈**不是总算力**，
而是**每帧的关键路径**。

```
514% / 32.8fps = 157 核·毫秒/帧   <- 处理一帧真正消耗的 CPU
2 worker / 32.8fps = 61ms        <- 但一帧的关键路径只有 61ms
```
⇒ 分配给一帧的 4 个线程，实际只顶得上 ~2.6 个（其余在 barrier / 串行 pre-post 上空等）。

**由此得出一条硬指标**：`8 核 / 157 核·毫秒 ≈ 51 fps` —— FP32 在本机上的**理论上限**。

> ⚠️ **[T35 更正] 这个推导方向是错的**：157 核·毫秒/帧里含 ~36 的 **worker 串行**（现已降到 5.7），
> 而把串行削掉后 fps **不动** ⇒ 真正的约束是“**推理吞吐本身 ≈ 32~33 次/秒**”，不是“总核数 ÷ 每帧 CPU”。
> 详见 **§20.14.4**（含教训：用“总量÷单量”推上限前，必须先证明单量里没有可消除的串行部分）。
所以后续实验的判据改成 **“把 %CPU 从 514% 推到 700~800%”**，而不是直接盯 fps。

**另一条**：解码那 1 个核是在为“被丢掉的 87% 帧”干活（文件源全速解码，处理 1 帧附带解码 7.6 帧）。
换真实 30fps 摄像头直接省下这 1 核 ⇒ **文件源基准测试整体偏悲观**。

**最佳已知配置（回退点）**：`pool_size: 2` + `num_threads: 4` = **32.8 fps**。
**下一实验**：`pool_size: 4` + `num_threads: 2` + `pipeline.worker_threads: 4`（4 路并发 x 2 线程 = 8）。
**踩坑预防**：`worker_threads` 必须 <= `pool_size`（借不到引擎的 worker 会白等）；
且三个旋钮**必须成组改**，改一半会更慢（会误判成“实验失败”）。`scripts/bench.sh` 已回显生效参数。

---

### 20.14 [T35] 推理性能收官：缓存友好改造 + 分段计时 ⇒ **FP32 天花板定量化（~33 fps）**

**一句话结论**：worker 侧串行开销从 ~36ms/帧 砍到 **5.70ms/帧（↓6.3×）**，而吞吐**纹丝不动**
⇒ **瓶颈是推理自身 ~32~33 帧/秒的吞吐墙（FLOP 受限），不是流水线**。
本机 FP32 调优**到此为止**；唯一剩下的杠杆是减少模型计算量（INT8 / 降分辨率 / 抽帧）。

#### 20.14.1 改了什么（两处，行为等价）

| 文件 | 改动 | 等价性依据 |
|---|---|---|
| `inference/YoloPostProcessor.cpp` | 后处理改**缓存友好**：旧写法 `for(i) for(c) data[(4+c)*8400+i]` = 672,000 次**跨步 33.6KB** 的访问（每次都是新 cache line，预取基本失效）；新写法 `for(c) for(i)` 先按 80 行逐列求逐框 max（每行 33.6KB 驻 L1/L2，顺序访问 + 可自动向量化），再只对**过阈的少数框**解析坐标 | 逐列取 max 与原“每个框找最大类别”等价；`>` 比较 ⇒ 平局保留较小 class id、max 初值同为 `0.0f`，行为一致 |
| `inference/OpenVINOEngine.cpp` | 预处理**直写输入 tensor**：旧路径 `cv::dnn::blobFromImage()` 每帧**新建 4.9MB blob** 再整体拷进用户 tensor；新路径 `cv::resize`（`thread_local` 复用缓冲）+ 按 NCHW 直接写 `infer_request_.get_input_tensor()`。保留 **f32/NCHW/CV_8UC3/尺寸一致** 前置检查，不满足（如 u8 输入的量化和模型）自动回落 `blobFromImage` | 与 `blobFromImage(swapRB=true, crop=false)` 同为 **INTER_LINEAR 普通缩放 + 1/255 + BGR→RGB + HWC→CHW**；仅浮点乘的结合顺序不同（≤1 ULP）|

**新增分段计时**（可直接 `grep T35`，每 100 帧打一行均值）：
`OpenVINOEngine`（预处理/推理）+ `YoloDetector`（后处理）。这是本轮唯一的“新增可观测性”，后续调优都靠它定位。

#### 20.14.2 实测（`pool_size=2` + `num_threads=4` + `worker_threads=2`，即 §20.13 的最佳已知配置）

```text
[生效] device=CPU, performance_mode=latency, num_threads=4 | 引擎数量: 2 | pool=2
[T35] 引擎分段/帧: 预处理=2.44ms  推理=77.94ms  (累计 100 帧)
[T35] 后处理耗时/帧: 3.26 ms  (累计 100 帧)
```

| 指标 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| 预处理 | ~8~15ms（估算，见下注）| **2.44ms** | ↓3~6× |
| 后处理 | ~10~20ms（估算，见下注）| **3.26ms** | ↓3~6× |
| **worker 串行合计** | **~36 核·毫秒/帧** | **5.70ms/帧** | **↓6.3×** |
| 吞吐（同配置）| 32.8 fps（T30/T32，§20.13）| 31.9 / 31.1 / 26.1 fps | **无变化**（±10% 噪声内）|
| %CPU | 514% | 511% | **无变化** |

> **注（本节唯一的估算成分）**：“优化前”没有直接读数（计时器是 T35 才加的）。量级由两条独立线索交叉印证：
> ① **代码级**：`blobFromImage` 每帧新建并拷贝 4.9MB（约数 ms~15ms）、后处理 672k 次跨步访问（约 10~20ms）；
> ② **%CPU 归因**：§20.13 测得整机 514%（≈5.1 核），而推理最多吃 4 核 ⇒ 推理之外确有 ~1 核的离线开销（`32.8fps` 下 ≈30ms/帧）。
> ⇒ 取 **~25~36ms/帧**；“优化后”的 5.70ms 是**直接实测**。
> 若报告里不愿留估算，可 `git stash` 旧实现重测一次（同配置）。
> `推理=77.94ms` 是**前 100 帧**均值（含引擎预热），稳态更低（按 fps 反推约 57ms）；结论不受影响。

#### 20.14.3 结论：FP32 天花板 ≈ 33 fps，且**与线程形态无关**

三条独立证据：
1. **12 组配置扫过**（`num_threads` 1/2/4/8 × 引擎/worker 1/2/4/8 × `latency`/`throughput`）：**只要给到 ≥4 线程且 4 路 worker，所有配置都撞在 29~33 fps**；线程/并发给得过少才会明显掉下去（单线程/单 stream 最慢）—— 即“**多了没用，少了才亏**”；
2. **把 worker 串行砍掉 30ms/帧后，fps 与 %CPU 都不动** ⇒ 关键路径不在 worker 侧（**本条是 T35 的核心增量**）；
3. **FLOP 定量**：`8.7 GFLOP/帧 × 32/s = 278 GFLOP/s`；WSL 报 4 核 × 2 线程 ⇒ `4 物理核 × ~4.3GHz × 32 FLOP/周期 ≈ 550 GFLOP/s`
   ⇒ **实效率 ~51%**。对一个含大量 depthwise / 小算子 / SiLU 的 CNN，这属**正常偏好**水平
   ⇒ 说明 OV 的 CPU 后端没被浪费，**是模型本身要花这些 FLOP**。

⇒ **原始 30.2 fps 已是天花板的 92%**；“流水线 / 线程 / 引擎 / worker 数”这条线**没有可挖的余量了**。

#### 20.14.4 ⚠️ 对 §20.13 的一处更正（自我纠错）

§20.13 写过「`8 核 / 157 核·毫秒 ≈ 51 fps` —— FP32 在本机上的**理论上限**」。**这个推导方向是错的**：

- 那 157 核·毫秒/帧里含 ~36 的 **worker 串行**（现已降到 5.7）；把它削掉后 fps **不动**，
  说明分母（每帧 CPU）不是约束 —— 推理占用的核数会**随可用余量自动膨胀**，把省下来的核吃掉；
- 正确说法：约束是“**推理吞吐本身 ≈ 32~33 次/秒**”，而不是“总核数 ÷ 每帧 CPU”；
- **教训**：用“总量 ÷ 单量”推上限，必须先证明“单量”里没有**可并行/可消除的串行部分**，否则会把串行开销误算成“可用算力”。

#### 20.14.5 机制：省下的核去哪了 —— TBB 自旋

- 优化前后 **%CPU 都是 511~514%，一核没降**；而 pre/post 实测只占 `5.7ms/帧 × 31.9fps ≈ 0.18 核`（原 ~1.08 核）
  ⇒ 释放出的 **~0.9 核没有变成帧，而是被推理侧（TBB）吃掉**；
- 与 §20.13 的另一观测自洽：**线程各忙 46%、栈上大量时间在 TBB wait/spin** ⇒ 推理池饱和后，多余的 CPU 用于**自旋等待**而非有效计算；
- ⇒ 这也是“加 worker / 加引擎 / 加线程全都无效”的微观解释：**它们只是把同样的 CPU 摊得更碎，并把省下的部分烧在同步上**。

#### 20.14.6 下一步：INT8（唯一剩下的杠杆）

| 项 | 内容 |
|---|---|
| 路径 | **A（先做，最省事）** 用官方/Ultralytics 导出的 yolov8n **INT8** IR，直接换 `model_xml_path` 验证收益上限；**B** 自做 PTQ（NNCF，`nncf.quantize`，用测试视频抽 100~300 帧做校准集）|
| 预期 | 本机 Alder Lake 有 **AVX-VNNI（无 AVX-512）** ⇒ **1.5~2.5×**（约 **50~80 fps**）；若上阿里云 SPR/Xeon 带 **AMX** ⇒ 常见 3~5× |
| 代码 | **不用改**（换模型路径 / 加一条 `models:` 条目即可）；也可用现成级联/双模型机制把 FP32 与 INT8 配在一起跑同一段视频做 A/B |
| 验收 | 速度：`scripts/bench.sh`（铁律 1/2 见 §20.12）；精度：**与 FP32 同视频的检测结果一致率 + 抽查画面**（无标注集时的代理指标），有标注再补 mAP。⚠️ **INT8 是拿精度换速度，`models:` 里必须同时保留 FP32 条目作退路** |
| 时机 | **T35 的串行优化在 INT8 之后更值钱**：推理变快后，5.7ms 的串行占比会从 ~9% 升到 ~17%，不做就会把 INT8 的收益吃掉一截 |

**如果继续调优（INT8），使用者能感知什么？（报告话术）**

| 场景 | 使用者可见？ | 说明 |
|---|---|---|
| 单路 30fps 摄像头 | ❌ 几乎无感 | 30.2 fps 已 1:1 跟上源 ⇒ 提速只是余量，**画面不会更流畅** |
| **多路复用** | ✅✅ **最直观** | “同一台机器从 1 路变 2 路” —— 现场数摄像头，谁都懂 |
| **端到端时延** | ✅ **演示最有效** | 帧不再积压，告警从“几百 ms~秒级”压到“几十 ms”；**建议在画面上直接打时延数字**（人走过去框跟手不跟手，一眼可见）|
| 不抽帧（`frame_interval: 1`）| ✅ 可见 | 快速移动目标不漏帧、轨迹连续（现在为保实时可能要抽帧）|
| 精度 | ⚠️ **可能是负向** | INT8 掉点会表现为“远处小目标偶尔漏检” —— **必须用一致率/mAP 守住，否则比“没变快”更糟** |

⇒ 所以 INT8 的目标**不该定成“更快”**，而应定成：**“精度不掉的前提下，一路变两路 / 时延压到 100ms 内”**。

#### 20.14.7 踩坑：**配置里重复键会让调优静默失效**

`model_config.yaml` 里出现过两处 `num_threads`（旧 `8` + 新加 `4`）：**yaml-cpp 取第一个** ⇒ 实际生效 8，
整整一轮实验的结论不可用（直到 `[生效]` 行打出 `num_threads=8` 才发现）。
⇒ **规矩：① 每轮实验先确认 `[生效]` 行；② 同一段里禁止重复键（历史值改用注释保留）。**

#### 20.14.8 文件清单

- **修改**：`src/inference/YoloPostProcessor.cpp`（缓存友好后处理 + `reserve`）、
  `src/inference/OpenVINOEngine.cpp`（直写输入 tensor + 预处理/推理计时）、
  `src/inference/YoloDetector.cpp`（后处理计时）、`config/model_config.yaml`（删重复 `num_threads`）。
- **不改**：任何接口、配置结构、流水线、引擎池 —— 全部是**内部实现改动**。
- **回退点**：`git checkout src/inference/`（配置保持 §20.13 的最佳已知配置：`pool=2` + `num_threads=4` + `worker_threads=2`）。

---

### 20.15 [T35 收尾] 文档与现实对账（本项目“结尾”时的账目）

写于调优收官、准备收尾时。**以下都是核对过的事实**；若与 §17.5 / §20.6 / §12 的旧文字冲突，**以本节为准**
（旧段落保留不删，因为它们是当时的记录；但不要再按它们判断现状）。

#### 20.15.1 三处状态修正

| 项 | 旧记录（已过时）| 实际（已核对）|
|---|---|---|
| **MySQL 落库** | §17.5 “部分闭环”里有一条说 `detections` **表不存在**、一直写 `build/db_fallback.csv`（P2-2 未做）| ✅ **已闭环**：表已建，程序正常落库、**不再产生 `db_fallback.csv`**（用户本机确认）|
| **`.gitignore`** | §20.6 说文件内容被 markdown 围栏（```text / ```）包裹 | **只有顶部那一行是真围栏（已删）**；末尾看到的那个是**读取工具的显示围栏**、文件里并没有 ⇒ 旧判断**对了一半**。现已重读验证干净（首行 `# 编译产物`，末行 `.env` 那条）|
| **`config/config.test.yaml`** | §10 / §14 / §15 与 README 都教人用它 | ⚠️ **文件已不存在**（源码与 `build/` 都没有，照着走会踩空）⇒ 已按笔记**重建**。**注意：重建版≠原文件逐字副本**（传感器只用 `radar/stub`，不依赖外部文件；`cascade` 保持 false）；若与 §14.7 的旧统计数字有出入，**以重建版实测为准** |

#### 20.15.2 项目现状（一句话）

**架构完备度高，真实资源验证覆盖率低。**

- 🟢 **可信**：T12–T35（11 个架构级 BUG 全修 + Phase A~D 四层抽象 + 自检 50/50 + 优雅关闭 + 性能定档）
- 🟡 **做完了但没被真实资源验证**：Phase B 级联（**本仓库无 classifier 模型**）/ 复核（只有 Python mock，真 VLM 未接）/ `web_gateway`（未联调）/ T29（待回归，**且只能在 RTSP 实时源下验**）
- 🔴 **已知但冻结**：§20.4 库不可用直接退出；R-13（RTSP `close()` 打不断 + `output.avi` 无上限）；R-5（断流不重连）；R-9（时间基未透传）

#### 20.15.3 唯一剩下的性能杠杆：**INT8**（不是调优）

T35 的三条证据（§20.14.3）已把 FP32 钉在 **~33 fps**；再调线程/引擎/worker 的收益是 **0**。
要再快只能 **减少模型计算量**（INT8，预计 1.5~2.5×，阿里云 SPR/Xeon 带 AMX 更高）。
⚠️ 但 **T35 的串行优化要到 INT8 之后才真正回本**：推理变快后，5.7ms 的串行占比会从 ~9% 升到 ~17%——
所以这两件事是配套的，不是二选一。

#### 20.15.4 若要继续（按性价比排序）

1. **§20.4 DB 解耦**（半小时量级，解开“演示必须先起数据库”的死结）—— 本次**明确不做**（用户判断当前无大问题）；
2. **把 🟡 跑一遍**（建表已做；剩余：T29 需先推 RTSP 流；`web_gateway` 需 `.env`；复核/融合用重建的 `config.test.yaml` + mock）；
3. **R-13 / R-5** —— 只在真要上 RTSP 摄像头时才需要；
4. **INT8** —— 需模型资源（官方 INT8 IR 或自做 PTQ）。

#### 20.15.5 收尾时仍悬空的小事（不阻塞，但别忘了）

- 根目录调试残留：`_chk.txt` / `_chk.txtgit` / `check.txt`（建议删，非必须）；
- `docs/` 是空目录（留着占位或删掉都行）；
- `[T35]` 的 printf 仍开着（每 100 帧一行）—— 留着有用（下次调 INT8 直接看分段耗时），生产环境算噪声；
- 本仓库的最新状态**建议打一个 git tag/commit**（当前是“已知最优且已量化”的快照）。

### 20.7 [T33] ⚠️ WSL 下 `device=AUTO` → NPU 插件段错误（已修：`device: CPU`）

**现象**：启动后第一台引擎加载成功、第二台还没打印就 `Segmentation fault`。

**排查过程（每步都在缩小范围，前两个假设均被证伪）**：

1. `OpenVINOEngine::init()` 里所有 OV 异常都被 `catch (const std::exception&)` 兜住并打 “初始化失败”——
   日志里**没有**这行 ⇒ 不是抛异常，是真·非法内存访问；
2. `InferenceEnginePool::init()` 用 `make_shared` + `vector<shared_ptr>` 持有引擎，无拷贝/移动/裸指针 ⇒ 池子无罪；
3. 环境自证：`OpenVINO_DIR` / `ldd` / `LD_LIBRARY_PATH` 三者**同源**（都是 `/opt/intel/openvino_2026.4.0`），
   机器上只有一套 OpenVINO ⇒ **排除“库/插件版本错配”**（这是最初的头号假设，被证伪）；
4. `models/yolov8n.xml` 头部：IR **v11** + `f32` + `1x3x640x640` ⇒ 模型正常，排除模型损坏；
5. `gdb -batch -ex run -ex bt --args ./CVInfer-Gate` 给出决定性栈（自底向上
