# CVInfer-Gate: 基于 C++ 与 OpenVINO 的高性能 AI 推理网关

![C++](https://img.shields.io/badge/C++-17-blue.svg)
![OpenVINO](https://img.shields.io/badge/OpenVINO-2026.4.0-green.svg)
![Docker](https://img.shields.io/badge/Docker-Enabled-blue.svg)
![gRPC](https://img.shields.io/badge/gRPC-Supported-orange.svg)

## 项目简介
CVInfer-Gate 是一个面向安防/工地场景（如安全帽检测）的高性能 AI 推理网关 针对传统 Python 后端在高并发视频流处理中存在的 GIL 锁限制和内存管理混乱问题 本项目底层完全采用 **C++17** 构建，结合 **OpenVINO** 实现高性能推理，并通过 **gRPC** 对外提供微服务接口 

项目采用了 **BFF（Backend for Frontend）** 架构，核心推理由 C++ 保障极致性能，前端交互由轻量级 Python Flask 网关进行 HTTP 协议转换 实现了性能与开发效率的平衡

## 核心特性
- **高性能多线程流水线**：基于生产者-消费者模型，手写线程安全队列，实现零拷贝与跳帧策略，防止视频流积压
- **AI 推理引擎**：使用 OpenVINO (C++) 加载 YOLO 模型，手写 NMS (非极大值抑制) 后处理；**纯 CPU（yolov8n/640、FP32、i7-12650H + WSL 8 vCPU）实测 30.2 ± 0.7 FPS**（源 29.96 FPS ⇒ 基本压实时线）；经 12 组配置验证的**量化上限约 33 FPS**，唯一剩余杠杆是 INT8
- **数据持久化**：集成 MySQL Connector/C++，实现检测记录与异常告警的异步落库
- **告警去重（[T39]）**：同标签 + 框重叠（IoU）+ 冷却窗内只告警一次，抑制「同一目标连续帧重复告警」；参数见 `alert.dedup`，默认开启、可一键关闭
- **目标跟踪（[T40]）**：关联式跟踪（IoU + 质心兼底、滑行/退休）给出跨帧稳定的 `track_id`（画面上直接标 `#12`）；告警去重**以身份为准**，并为停留/徘徊行为分析铺路
- **微服务与网络通信**：基于 gRPC + Protobuf 提供远程调用接口
- **容器化部署**：提供完整的 Dockerfile 与 docker-compose.yml，一键拉起 MySQL、C++ 网关与 Python Web 网关
- **Web 交互演示**：提供极简 Flask 网关，支持浏览器上传图片并实时返回带框结果
- **多模态决策级融合（可选）**：视频(主模态) + 雷达/红外(统一时间戳采样)，时间对齐 + 目标关联 + 加权置信度融合；默认关闭，关闭时与纯视觉链路行为完全一致
- **可自检的四层架构**：模型抽象 / 级联复核 / 异步大模型复核 / 多模态融合 均为接口分层，可被测试内替身替换；`phase_selftest` 一次跑完四个阶段（零外部依赖，秒级）
- **可接真 VLM 的异步复核（Phase C）**：复核服务端已实现（`vlm_review/`），支持任意 **OpenAI 兼容**多模态服务（vLLM/Ollama/DashScope/OpenAI）或本地 Qwen2.5-VL；另有确定性 mock 与前端联调客户端。换模型只改环境变量，**替换复核服务只改 `review.endpoint`，C++ 侧零改动**

## 系统架构
```text
[视频源/RTSP] ──(FFmpeg)──> [C++ 多线程队列] ──(OpenVINO)──> [YOLO 后处理/NMS]
                                                                  │
[Web 浏览器] <──(HTTP)── [Python Flask BFF] <──(gRPC)─────────────┤
                                                                  │
                                                       [MySQL 数据持久化]



```

## 技术栈
```text
模块	    技术选型
核心语言	C++17, Python 3.10
视频解码	FFmpeg (libav*)
图像处理	OpenCV
AI 推理     OpenVINO Runtime (C++)
网络通信	gRPC, Protobuf
网关提供    Python+轻量Flask网页
数据存储	MySQL 8.0 (Connector/C++)
工程构建	CMake
部署运维	Docker, Docker Compose
```


## 快速开始（Docker 部署）

**要求**：Ubuntu 22.04 或更高版本；Docker 与 Docker Compose。

1. 克隆项目
git clone https://github.com/jiangmingpeng/CVInfer-Gate.git
cd CVInfer-Gate

## 目录结构

```text
CVInfer-Gate/
├── config/           # YAML 配置文件
├── models/           # OpenVINO IR 模型与标签
├── proto/            # gRPC 接口定义
├── src/
│   ├── video/        # 视频源抽象与实现 (File/RTSP)
│   ├── inference/    # OpenVINO 引擎与 YOLO 后处理
│   ├── pipeline/     # 多线程流水线调度
│   ├── database/     # MySQL 写入封装
│   ├── service/      # gRPC 服务实现
│   ├── review/       # 大模型异步复核 (gRPC 客户端 + 异步调度)
│   ├── sensor/       # 多模态传感器统一抽象 (视频/雷达/红外)
│   ├── fusion/       # 决策级融合 (时间对齐 + 目标关联 + 置信度融合)
│   └── utils/        # 线程安全队列、配置解析
├── web_gateway/      # Python Flask BFF 网关
├── scripts/          # 建表脚本 schema.sql / 复核 mock / 传感器回放示例
├── vlm_review/       # [T37] 真 VLM 复核服务端(gRPC; OpenAI兼容/本地transformers/mock)
├── docker/           # Dockerfile 与 Compose 编排
├── tests/            # gRPC 客户端 + Phase A~D 阶段自检 (phase_selftest)
│   └── unit/         # [T38] gtest 单元测试 (纯逻辑: NMS/融合/配置/队列/ROI/告警去重)
├── docs/             # 架构与一致性总览 (OVERVIEW.md)
└── .github/workflows # [T38] CI: 编译 + ctest
```


2. 准备模型与视频
将 YOLO 的 OpenVINO 模型文件 (yolov8n.xml, yolov8n.bin) 放入 models/ 目录
准备测试视频 `test.mp4`：**本地直接跑放 `build/`**（程序按当前工作目录解析相对路径），**docker 跑放项目根目录**（compose 已挂载 `../test.mp4`）

3.一键启动
cd docker
docker compose build
docker compose up -d

4. 验证服务
查看后端日志：docker compose logs -f cv-infer-gate
浏览器访问 Web 网关：http://localhost:8080，上传图片即可看到检测结果
或使用 gRPC 客户端测试：./build/grpc_client


## 本地开发与阶段自检

工程把复杂链路分成了四层（模型抽象 / 级联复核 / 大模型异步复核 / 多模态融合），每一层都是接口调用，
因此每一层都可以用「测试内注入的替身」驱动，**不需要模型 / 视频 / MySQL / 复核服务端**：

```bash
cd build && cmake .. && cmake --build . -j

# 1) 零依赖自检：一次看全 Phase A~D 的行为与统计（退出码 0 = 全通过）
cmake --build . --target phase_selftest -j
./phase_selftest

# 2) [T38] 单元测试：纯逻辑回归（首次需要 sudo apt install libgtest-dev）
cmake --build . --target cv_unit_tests -j
ctest --output-on-failure            # 一次跑完 cv_unit_tests + phase_selftest
./cv_unit_tests --gtest_filter='YoloPostProcessor.*:SensorFusion*'   # 只跑某一块
```

单测覆盖的正是「改动受益面最大、又最容易悄悄改坏」的那部分：

| 单测文件 | 覆盖内容 |
|---|---|
| `test_yolo_post_processor.cpp` | cxcywh→xyxy 解码、置信度过滤、NMS 同类抑制/异类保留、`>` 阈值边界、边界裁剪、按原图缩放（**不需要模型文件**） |
| `test_sensor_fusion.cpp` | 容差窗口边界、雷达按标签关联、红外按 IoU 关联、贪心匹配「一个传感器目标只用一次」、权重 clamp、emit_sensor_only |
| `test_alert_gate.cpp` | 告警去重（[T39]）：滑动冷却窗（持续目标只告警一次 / 到期后重新告警）、标签隔离、框重叠判定、空框与 `cooldown=0` 退化、`enabled=false` 短路、有界内存丢最旧、**多线程并发只放行一次**、**[T40] 身份优先（同 id 框移远也去重 / 同框不同 id 不误杀 / 只有一边有 id 则退回几何）** |
| `test_target_tracker.cpp` | 目标跟踪（[T40]）：id 首次分配/跨帧稳定、快速移动的距离兼底、超出可达范围分裂新 id、标签隔离、遮挡滑行、老化退休后 **id 不复用**、`min_hits` 确认门限、回退帧防御、`enabled=false` 短路、轨迹数有界、dwell 累计、多目标不串号 |
| `test_config_parser.cpp` | 默认值契约（含 `alert.dedup`、`tracking`）、`source_type` 与 `rtsp://` 前缀交叉校验、`${VAR:-default}` 展开、灰区颠倒、传感器非法组合、去重/跟踪参数越界、新旧模型格式兼容 |
| `test_thread_safe_queue.cpp` | DropOldest 丢最旧、Block 不丢帧、close 排空后再返回 false、超时 pop、on_drop 计数 |
| `test_roi_utils.cpp` | 外扩取整、贴边/越界裁剪、越界判空、负 padding、crop 深拷贝（跨线程送审的安全前提） |

> CI（`.github/workflows/ci.yml`）在每次 push/PR 上跑同一套命令，`-DCV_TESTS_REQUIRE_GTEST=ON` 保证「测试没跑」不会被当成「测试通过」。

想跑**真实链路**看四个阶段（不动被锁的 `config/config.yaml`，用配置注入）：

```bash
# 复核服务（否则 Phase C 只能看到 unavailable=N），**二选一**：
#   A. 确定性 mock（回归/CI，零依赖）
python3 scripts/mock_review_server.py --port 50052
#   B. 真 VLM（T37；上游 VLM 先起在 :8000，如 vLLM/Ollama/云 API）
pip install -r vlm_review/requirements.txt
python3 -m vlm_review.server --backend openai \
    --base-url http://127.0.0.1:8000/v1 --model Qwen/Qwen2.5-VL-7B-Instruct
#   单独联调（不启动流水线；proto 首次会自动生成）：
./build/review_client 127.0.0.1:50052 frame.jpg "判断该人员是否未佩戴安全帽"

# 主程序：把 cascade / review / sensors+fusion 逐段打开（改 config/config.test.yaml 的 enabled）
cd build && ./CVInfer-Gate --config config/config.test.yaml
```

| 想看的阶段 | 看哪里 |
|---|---|
| A 抽象层 | 启动日志 `模型配置加载成功: ... (模型数=N)` / `使用检测器: xxx` |
| B 级联灰区 | 启动日志 `使用检测器: cascade (级联模式)` + 退出时 `级联统计: primary/triggered/confirmed/rejected/skipped` |
| C 异步复核 | 复核服务端逐条请求日志（`[vlm]`/`[mock]`）+ 客户端启动日志 `复核服务就绪/暂不可用` + 退出时 `复核统计: submitted/reviewed/confirmed/rejected/timeout/unavailable` |
| D 多模态融合 | 退出时 `融合统计: frames/samples/aligned/targets/matched/unmatched_sensor` |

> 提示：CMake 的 POST_BUILD 会用源码目录的 `config/` 覆盖 `build/config/`，所以不要改 `build/config/config.yaml`；
> 要改就改源码 `config/config.yaml`，或用 `--config` 指向另一个文件。

## 验证状态（本项目“结尾”时的账目）

**一句话：架构完备度高，真实资源验证覆盖率低。**

| 层 | 状态 | 说明 |
|---|---|---|
| 架构重构 T12–T36 | 🟢 可信 | 11 个架构级 BUG 全修（线程安全/启动时序/优雅关闭/背压/落库）+ **[T36] 数据库降级 / RTSP 断流重连**（见下）|
| Phase A~D 四层抽象 | 🟢 可信 | `phase_selftest` 50/50，零外部依赖、秒级 |
| 单元测试 / CI | 🟢 **[T38+T39+T40] 新增** | `ctest` = `cv_unit_tests`(gtest 94 例) + `phase_selftest`，共 95 项全绿；覆盖 NMS / 多模态融合 / 配置校验 / 线程安全队列 / ROI / 告警去重 / 目标跟踪；GitHub Actions 每次 push/PR 自动跑（纯文档改动跳过） |
| MySQL 落库 | 🟢 可信 | 表已建，程序正常写入（**不再产生 `db_fallback.csv`**）；**[T36] 库不可用不再退出**：降级写 CSV + 后台自动重连并回传 |
| Phase B 级联 | 🟡 能级联 / 精度未回归 | **本仓库已带 `role=classifier` 模型**（`models/helmet_cls.xml` + `helmet_labels.txt`，`model_config.yaml` 已注册 `helmet_classifier`，`config/config.test.yaml` 默认开启级联）；⚠️ 目前只有「能跑通级联」的冒烟，**未做过精度回归**（改 `accept_label`/`accept_conf` 无人能自动报警） |
| Phase C 异步复核 | 🟡 两端已就位 / 真 VLM 未实测 | **[T37] 服务端已实现**（`vlm_review/`：OpenAI 兼容 / 本地 transformers / mock 三后端，含 Health 探活 + 延迟观测；调用方鉴权见 [T41]）；`scripts/mock_review_server.py` 保留为规则 mock。⚠️ “真 VLM 结果好不好”取决于你本地上游模型，需自行实测 |
| Phase D 多模态融合 | 🟢 可信（stub 雷达）| 端到端实测 `matched=66`；带框传感器见 `config/config.test.yaml` 注释 |
| `web_gateway` (Flask BFF) | 🟡 未联调 | 需先 `cp web_gateway/env.example web_gateway/.env` |
| RTSP 接入 | 🟢 已补强 | 连通已验证；**[T36] 断流指数退避重连 + `close()` 可中断**（`interrupt_callback` 直接打断 `av_read_frame`），重连对上层透明 |
| 性能 | 🟢 已定档 | 30.2 ± 0.7 FPS（量化上限 ~33）；瓶颈是推理 FLOP，**不在流水线** |

**已知限制（剩余）**：
1. 结果视频无大小上限；RTSP 重连后若分辨率变化，sink 端 `VideoWriter` 不会自动重建（换流请重启进程）。
2. **告警去重现在是「身份优先」的**（[T39]+[T40]）：默认启用跟踪 ⇒ 同一 `track_id` 即使框移开也只告警一次（快速移动目标不再重复）。但**无外观(re-ID)特征**，遮挡/交叉后可能换 id（ID switch），那之后仍可能重新告警一次；`tracking.enabled=false` 则退回纯几何去重（即 [T39] 行为）。另外 `track_id` **未落库**（`detections` 表无该列，需 schema 迁移），gRPC 响应也没带它。

**[T41] 修复：复核链路鉴权「只有一半」**：
- 问题：C++ 侧 `GrpcLlmReviewer` 早就按 `review.auth_token` 发 `authorization: Bearer <token>`，但**服务端 `vlm_review/server.py` 从来不校验**（grep 到的 `Authorization` 只是它作为客户端去调上游 API 用的）⇒ token 是装饰品，**任何能连上 50052 的人都能白嫖你的 VLM**（算力/额度）。文档里"含鉴权"的说法也是错的（指的其实是上游 api_key），本轮一并改正。
- 做法：服务端加 `_AuthInterceptor`（gRPC 拦截器）+ `VLM_AUTH_TOKEN` / `--auth-token`，**所有方法统一校验**（含 Health —— 与 C++ 侧"每个 RPC 都带 token"完全对称；将来新增 RPC 自动被覆盖，不会漏）。`hmac.compare_digest` 定长比较防前缀时序泄漏；非 ASCII metadata 不会炸成 `UNKNOWN`；token 为空 = **完全不校验**（零破坏，与旧行为等价）。
- 客户端侧：`tests/test_review_client.cpp` 支持 `REVIEW_AUTH_TOKEN` 环境变量（**不用命令行参数**：token 不落进 shell 历史 / `ps`），并新增退出码 `5 = UNAUTHENTICATED` —— 否则服务端一开鉴权，仓库自带的诊断工具就哑了。
- 验证：`python3 -m vlm_review.test_auth_interceptor`（**无 grpcio 依赖**，用假 grpc 模块驱动真逻辑：放行/拒绝/前缀攻击/尾空格/小写 `bearer`/metadata 混排/非 ASCII/流式 handler 类型保持，全部通过）。
- 端到端自测（需 `pip install -r vlm_review/requirements.txt`，本机未装 grpcio 故未跑）：
  ```bash
  VLM_AUTH_TOKEN=s3cr3t python3 -m vlm_review.server --backend mock --port 50052 &
  ./build/review_client 127.0.0.1:50052                            # 期望退出码 5(UNAUTHENTICATED)
  REVIEW_AUTH_TOKEN=s3cr3t ./build/review_client 127.0.0.1:50052    # 期望退出码 0
  ```
- 仍未做：**主服务自己的 gRPC（对外 50051）仍无鉴权**；TLS（当前是明文共享密钥，内网够用、公网不行）；`scripts/mock_review_server.py` 不校验 token（开发 mock，忽略即可）。

**[T40] 新增：目标跟踪（track_id）**：
- 问题：全链没有 `track_id` —— 告警去重只能靠"框重叠"（快速移动目标照样重复），"停留/徘徊"这类**行为分析**更没有立足点。
- 做法：`src/tracking/TargetTracker.h`（纯逻辑、header-only）：关联式跟踪 —— 同标签候选里 IoU 贪心 + **质心距离兼底**（救快速移动目标），失配后**滑行** `max_age_ms`（容忍遮挡/漏检），超时退休；`track_id` 就地回写进 `DetectionResult`（**加法字段**，默认 -1 ⇒ 未启用跟踪的链路零影响）。**id 单调递增且永不复用**（复用会让去重漏报新目标）。
- 位置：sink 内、**融合之后**（跟踪的是最终参与告警的目标集合）。能安全地"有状态"靠两件事：sink 是**单线程**，且 [T29] 的重排缓冲保证 `frame_seq` **单调递增** ⇒ 顺序调用天然无并发（仍防御性忽略回退帧）。
- 联动：[T39] 的 `AlertGate` 升级为**身份优先** —— 两边都有 id 时比 id（与框怎么移动无关），没有 id 时退回几何重叠 ⇒ 对未启用跟踪的链路零行为变化。画面标注带 `#id`，肉眼可验证"同一个人只有一个 id"。
- 验证：13 条跟踪单测 + 3 条去重身份用例 + 1 条配置用例；退出日志新增 `目标跟踪统计: frames/spawned/retired/active/matched/longest_dwell`。
- 仍未做：`track_id` 未落库（schema 迁移 + `DBWriter` 两处）、gRPC 响应未带该字段、无轨迹预测/外观特征（ID switch 见上）、停留/徘徊的**告警规则**未做（dwell 已可读）。

**[T39] 新增：告警去重**：
- 问题：告警判定是**每帧**执行的，而「安全帽缺失」描述的是**目标状态** —— 同一个人站着不动会被连续帧反复告警（每秒 N 条）。OVERVIEW §5 原把它列为 🔴 缺失。
- 做法：新增 `src/utils/AlertGate.h`（纯逻辑、header-only、内部 mutex），在**告警链路**上去重：复核路径打在**送审处**（同一目标只送审一次 ⇒ 不可能重复告警，且省掉重复的 VLM 调用；复核回调拿不到框，无法在那里去重），本地规则路径打在**写告警处**。画框/写视频/检测入库**完全不受影响**。
- 语义：冷却窗按**最近一次命中**刷新 ⇒ 目标持续在画面里只告警一次；消失超过 `alert.dedup.cooldown_ms` 后再次出现才重新告警。`alert.dedup.enabled=false` 可一键回退到旧行为（默认开启是刻意的行为修正）。
- 验证：13 条单测（含并发）+ 2 条配置用例；退出日志新增 `告警去重统计: allowed=/suppressed=/tracked=`。

**[T38] 已修的一条功能盲区**：
- `SensorFusion::fuse()` 原为 `if (targets.empty() || vision.empty()) return st;` —— 某帧**视觉一个目标都没有**时整个融合阶段直接返回，传感器证据**既不计数也不产出**，把「雷达/红外测到了、视觉漏检了」这个**最该靠融合兜住**的场景静默作废。现改为**只**在「没有任何传感器目标」时提前返回：空视觉帧照常走未关联逻辑 —— 统计口径恢复真实，且 `emit_sensor_only=true` 时**带框**传感器目标可独立成为目标（默认 `false` ⇒ 只统计不产出，对存量配置**零行为影响**）。已由 `tests/unit/test_sensor_fusion.cpp` 的 `EmptyVisionStillAccountsSensorEvidence` / `EmptyVisionEmitsBoxCarryingSensorOnlyTarget` / `EmptyVisionStillRefusesBoxlessSensorTarget` 三条钉住。

**[T36] 已修的两条历史限制**：
1. 数据库不可用时程序**直接退出** → 现为**降级模式**：记录先落本地 CSV，后台按 `reconnect_interval_ms` 自动重建连接池，恢复后回传（启动只做 1 次快速连库尝试，不再白等 ~20s）。
2. RTSP 断流不重连 / `close()` 打不断 `av_read_frame` → 现为 `interrupt_callback` 打断 + 读循环内**指数退避重连**（0.5s→8s 封顶），且**未新增任何接口**。

> 完整推导、逐条实测数据与踩坑记录都在 **[PROJECT_NOTES.md](PROJECT_NOTES.md)**（§20.12–§20.15 = 性能与收尾对账）。

## 目前打算的拓展方向
- **INT8 量化**（唯一剩下的性能杠杆）：预计 1.5~2.5×（本机 AVX-VNNI）；阿里云 SPR/Xeon 带 AMX 收益更大 —— 但这是**拿精度换速度**，必须与 FP32 做同视频一致率对比
- **OpenVINO 异步推理（Async Infer）**：⚠️ 本项目的实测结论是**它不会提速**（推理侧已饱和，加并发只是多烧 CPU），见 PROJECT_NOTES §20.13/§20.14
- 支持 RTMP/WebRTC 实时视频流推流，实现网页端实时监控
- 引入 Prometheus + Grafana 监控推理延迟与系统资源

