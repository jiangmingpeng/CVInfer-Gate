# CVInfer-Gate: 基于 C++ 与 OpenVINO 的高性能 AI 推理网关

![C++](https://img.shields.io/badge/C++-17-blue.svg)
![OpenVINO](https://img.shields.io/badge/OpenVINO-2025.4.0-green.svg)
![Docker](https://img.shields.io/badge/Docker-Enabled-blue.svg)
![gRPC](https://img.shields.io/badge/gRPC-Supported-orange.svg)

## 项目简介

**它是什么**：一个**视频流推理网关** —— 视频进去，检测 / 复核 / 告警 / 落库出来。
底层 **C++17 + OpenVINO** 扛性能，**gRPC** 对外提供接口，**Flask** 只负责网页那一层的协议转换（BFF 架构）。

**为什么用 C++**：高并发视频流放在 Python 后端里，很容易卡在 GIL 和内存管理上。
本项目把推理主链路整体放在 C++ 里 —— 实测纯 CPU（yolov8n / 640 / FP32）**30.2 ± 0.7 FPS**，基本压住 29.96 FPS 的实时线。

**它和“跑个 YOLO demo”的区别**：背压、降级、复核、去重、落库、指标、探针、容器化都是现成的 ——
少写的，正是“从能跑”到“能上”之间那几百行胶水代码。

> **一句话**：**clone 就能跑**（模型权重已入库）、**每项能力都能关**（关了就是纯视觉链路，对存量配置零影响）、**每个数字都有实测出处**。

## 它怎么工作（以“图书馆占座”为例）

需求很朴素：**“书在桌上、人走了很久”就该提醒管理员**。这件事拆成三步：

1. **规则层（便宜，每帧都能跑）** —— 先在画面上标好“座位”区域，再回答三个问题：
   物品在座位上吗？人在座位上吗？这个状态持续多久了？只有**连续够久**的候选才进入下一步。它的活是**筛**。
2. **VLM 复核（贵，但会“看图讲道理”）** —— 把座位那张图 + 结构化证据（`检出物品 [laptop]，已持续 412 秒无人`）交给大模型，
   请它回答“这算不算占座”。它的活是**判**。
3. **告警** —— 确认后才写库 + 推 webhook；同一个座位只报一次。

> **为什么要拆两步？** 因为**检测器没有“座位”这个概念**（再加一个模型也拿不到“物品属于哪个座位”），
> 而 **VLM 只看单帧、答不出“持续了多久”** —— 可“时间”恰恰是占座定义的核心。
> 规则层先把候选从「每帧每个目标」压到「每个座位一个事件」，**VLM 调用量直接降 1~2 个数量级**，还顺带把证据喂给了模型。

## 核心特性

**推理与流水线**
- **高性能多线程流水线**：生产者-消费者模型 + 手写线程安全队列（有界 + 丢最旧）实现背压 —— 算力不够时丢旧帧而非无限积压，延迟不随时间膨胀。
- **AI 推理引擎**：OpenVINO (C++) 加载 YOLO，手写 NMS 后处理。纯 CPU（yolov8n / 640 / FP32）实测 **30.2 ± 0.7 FPS**，已基本压住源 29.96 FPS 的实时线；经 12 组配置验证，FP32 天花板约 33 FPS，唯一剩余的杠杆是 INT8。
- **数据持久化**：MySQL Connector/C++ 异步批量落库；数据库不可用时自动降级写 CSV 并后台回连回传。

**业务能力（可插拔，默认关闭）**
- **级联复核（Phase B）**：检测器只把灰区（conf 0.25–0.5）目标交二级分类器二次确认。
- **异步 VLM 复核（Phase C）**：灰区目标送外部大模型「确认才告警」，支持任意 OpenAI 兼容服务或本地 Qwen2.5-VL；**替换复核服务只改配置，C++ 侧零改动**。
- **多模态融合（Phase D）**：视频 + 雷达/红外，统一时间戳后做时间对齐 → 目标关联 → 加权置信度融合。
- **目标跟踪**：IoU + 质心兜底关联，给出跨帧稳定的 `track_id`（画面直接标 `#12`）。
- **占座判定（规则层）**：静态座位 zone + 几何关系 + 时序状态机，把散落的 `book/bag/laptop` 框变成**「某座位：物品在、人不在，已持续 N 秒」**的可解释结论；只有规则筛出的候选才送 VLM 复核（调用量降 1~2 个数量级，且带结构化证据）。
- **告警去重**：同标签 + 框重叠（IoU）+ 冷却窗内只告警一次；启用跟踪后**按身份去重**。

**工程化**
- **可观测性与告警外发**：告警除落库外可 **webhook 推送**（有界队列 / 重试退避 / 不阻塞流水线）；`/metrics` 暴露 20 组指标供 Prometheus 抓取；`--health-check` 一条命令探活（容器 healthcheck 用）；日志按大小轮转。**均默认关闭**，对存量配置零行为变化。
- **微服务与容器化**：gRPC + Protobuf 对外接口；完整 Dockerfile + docker-compose，一键拉起 MySQL + C++ 网关 + Python Web 网关。

**演示与自检**
- **Web 交互演示**：`web_gateway/`（Flask BFF）一键启动（`bash web_gateway/run.sh`）。① **单图实时检测**：拖拽/选择上传 → 无刷新返回带框结果 + 结构化列表 + 在线徽标（点列表行高亮框、点图放大、一键下载）；② **流水线结果回看**：跑完 C++ 主程序后，在线播放带框视频（`output.avi` 自动按需转码）+ 占座/告警事件时间线。
- **可自检的四层架构**：四个 Phase 均为接口分层，可用测试替身驱动；`phase_selftest` 一次跑完 Phase A~D，**零外部依赖、秒级**。

## 系统架构
```text
[视频源 File / RTSP]
      │ FFmpeg 解码 · 抽帧 · 限速
      ▼
[有界帧队列 drop_oldest] ── [worker × N]  OpenVINO 推理 + NMS
                                      │
                                      ▼
[sink 单线程]  融合 → 跟踪 → 占座 → 告警去重
      ├──MySQL（检测/告警落库；连不上降级 CSV）
      ├──output.avi（带框结果视频）
      ├──gRPC :50051 ←── Flask BFF :8080 ←── Web 浏览器
      └──webhook 告警外发 / /metrics 指标（Prometheus）
```

## 技术栈

| 模块 | 技术选型 |
|---|---|
| 核心语言 | C++17 · Python 3.10 |
| 视频解码 | FFmpeg (libav*) |
| 图像处理 | OpenCV |
| AI 推理 | OpenVINO Runtime (C++) |
| 网络通信 | gRPC · Protobuf |
| Web 网关 | Python + 轻量 Flask |
| 数据存储 | MySQL 8.0 (Connector/C++) |
| 工程构建 | CMake |
| 部署运维 | Docker · Docker Compose |


## 快速开始（Docker 部署）

**要求**：Ubuntu 22.04 或更高版本；Docker 与 Docker Compose。

**1. 克隆项目**

```bash
git clone https://github.com/jiangmingpeng/CVInfer-Gate.git
cd CVInfer-Gate
```

<details>
<summary><b>clone 后的目录结构（点开看）</b></summary>

```text
CVInfer-Gate/
├── config/           # YAML 配置文件
├── models/           # OpenVINO IR 模型与标签(**权重已入库, clone 即可用**)
├── proto/            # gRPC 接口定义
├── src/
│   ├── video/        # 视频源抽象与实现 (File/RTSP)
│   ├── inference/    # OpenVINO 引擎与 YOLO 后处理
│   ├── pipeline/     # 多线程流水线调度
│   ├── database/     # MySQL 写入封装
│   ├── review/       # 大模型异步复核 (gRPC 客户端 + 异步调度)
│   ├── sensor/       # 多模态传感器统一抽象 (视频/雷达/红外)
│   ├── fusion/       # 决策级融合 (时间对齐 + 目标关联 + 置信度融合)
│   ├── tracking/     # 目标跟踪 (IoU+质心兜底关联, 给目标分配 track_id)
│   ├── occupancy/    # 占座判定 (静态座位 zone + 几何关系 + 时序状态机; 纯逻辑无模型依赖)
│   ├── alert/        # 告警外发 (webhook: 有界队列 + 重试退避, 传输层可注入)
│   ├── service/      # gRPC 服务 + /metrics 指标端点 (MetricsServer)
│   └── utils/        # 线程安全队列、配置解析、告警去重(AlertGate)、日志(轮转)、
│                     # 指标注册表(Metrics) 与极简 HTTP 客户端(HttpClient)
├── web_gateway/      # Python Flask BFF 网关（app.py + run.sh 一键启动 + templates/ + static/ 前端页面）
├── scripts/          # 建表脚本 schema.sql / 复核 mock / 传感器回放示例 / 告警接收端(alert_receiver.py)
├── docker/           # Dockerfile 与 Compose 编排 (+ prometheus.example.yml 抓取配置)
├── vlm_review/       # 真 VLM 复核服务端(gRPC; OpenAI兼容/本地transformers/mock)
├── tests/            # gRPC 客户端 + Phase A~D 阶段自检 (phase_selftest)
│   └── unit/         # gtest 单元测试 (纯逻辑: NMS/融合/配置/队列/ROI/告警去重/目标跟踪/占座)
├── docs/             # 文件级阅读地图 (READING_MAP.md) + 架构总览 (OVERVIEW.md)
├── mediamtx          # 内置 RTSP 服务器二进制(有意入库: clone 即可跑 RTSP 演示)
├── mediamtx.yml      # mediamtx 配置
└── .github/workflows # CI: 编译 + ctest + [P2-5] Python 自测
```

</details>


**2. 填写 docker/.env（口令）**

```bash
cp docker/.env.example docker/.env
# 编辑 docker/.env：至少把 MYSQL_ROOT_PASSWORD 与 DB_PASSWORD 填成**同一个**口令,
# 否则 mysql:8.0 会因空口令初始化失败、网关连不上库而降级写 CSV。
```

**3. 放一段测试视频**（模型已随仓库入库，无需准备）

**模型**：`models/` 下的 IR（`yolov8n.*` 检测 + `library_det.*` 图书馆场景检测 + `helmet_cls.*` 分类器样例）与标签**已入库**，
clone 即可用（[决策 a] 目标 = “clone 就能跑”）。

**配置**：Docker **不需要** `config/config.yaml` —— 它用仓库里已入库的 **`config/config.docker.yaml`**（Docker 专用），
容器内由 `CVINFER_CONFIG` 指定；`config/` 是挂载进去的 ⇒ 改完 `docker compose restart cv-infer-gate` 即生效。
口令 / 视频源等全部走环境变量（见上一步 `docker/.env`），`ConfigParser::expandEnv` 会展开 yaml 里的 `${VAR}`。

**测试视频**：`test.mp4` 需自备（体积大，不入库）——Docker 放到**仓库根目录**
（compose 把 `../test.mp4` 挂到容器 `/app/test.mp4`；**缺失时 compose 会明确报错**，不会静默建目录）。
想用 RTSP 源：改 `config/config.docker.yaml` 的 `video.source_type` / `source_path`（容器内指向 `host.docker.internal`）。

**4. 一键启动**

```bash
cd docker
docker compose up -d --build
```

首次会拉 OpenVINO / 编译 C++（约 5~10 分钟）。会依次拉起三个服务：
`mysql-db`（建表）→ `cv-infer-gate`（C++ 推理网关 :50051）→ `web-gateway`（Flask BFF :8080）。

**5. 验证**

```bash
cd docker
docker compose ps                     # cv-infer-gate 应显示 (healthy)
docker compose logs -f cv-infer-gate  # 看推理日志(找 "原视频分辨率" / FPS 统计)
```

浏览器打开 **http://localhost:8080**：上半部分拖拽/选择图片即可实时看到带框结果与检测列表；
下半部分「③ 流水线结果」可直接在线播放带框结果视频，并列出占座/告警事件。
也可用 gRPC 客户端：`./build/grpc_client`。

> **结果产物**：落在命名卷 `cvinfer-output`（`output.avi` + `logs/cvinfer.log` + 连不上库时的 `db_fallback.csv`），
> `cv-infer-gate` 与 `web-gateway` 共享该卷 ⇒ 网页才能读到结果视频与事件时间线。
> 宿主机查看：`docker run --rm -v docker_cvinfer-output:/v alpine ls -l /v`。

### 非 Docker（本地直接跑）

本地不读 `config.docker.yaml`，仍用 `config/config.yaml`（从模板生成）+ 环境变量注入口令：

```bash
cp config/config.example.yaml config/config.yaml
export DB_PASSWORD=<你的 MySQL 口令>     # 与本地 MySQL 一致
export GRPC_AUTH_TOKEN=<主服务 token>    # 可选；服务端/网关/grpc_client 必须一致
export VLM_TOKEN=<复核 token>            # 可选；设了则必须等于服务端 VLM_AUTH_TOKEN
export ALERT_PUSH_URL=<webhook 地址>      # 可选；如 http://127.0.0.1:8899/alert
export ALERT_TOKEN=<webhook token>        # 可选；与接收端校验的 x-alert-token 一致
```
（`ConfigParser::expandEnv` 会展开 yaml 里的 `${VAR}`，口令不必写回文件。）
`test.mp4` 放 `build/`（程序按当前工作目录解析相对路径）。


## 本地开发与阶段自检

工程把复杂链路分成了四层（模型抽象 / 级联复核 / 大模型异步复核 / 多模态融合），每一层都是接口调用，
因此每一层都可以用「测试内注入的替身」驱动，**不需要模型 / 视频 / MySQL / 复核服务端**：

```bash
cd build && cmake .. && cmake --build . -j

# 1) 零依赖自检：一次看全 Phase A~D 的行为与统计（退出码 0 = 全通过）
cmake --build . --target phase_selftest -j
./phase_selftest

# 2) 单元测试：纯逻辑回归（首次需要 sudo apt install libgtest-dev）
cmake --build . --target cv_unit_tests -j
ctest --output-on-failure            # 一次跑完 cv_unit_tests + phase_selftest
./cv_unit_tests --gtest_filter='YoloPostProcessor.*:SensorFusion*'   # 只跑某一块
```

<details>
<summary><b>单元测试覆盖清单（点开看）</b></summary>

单测覆盖的正是「改动受益面最大、又最容易悄悄改坏」的那部分：

| 单测文件 | 覆盖内容 |
|---|---|
| `test_yolo_post_processor.cpp` | cxcywh→xyxy 解码、置信度过滤、NMS 同类抑制/异类保留、`>` 阈值边界、边界裁剪、按原图缩放（**不需要模型文件**） |
| `test_sensor_fusion.cpp` | 容差窗口边界、雷达按标签关联、红外按 IoU 关联、贪心匹配「一个传感器目标只用一次」、权重 clamp、emit_sensor_only |
| `test_alert_gate.cpp` | 告警去重：滑动冷却窗（持续目标只告警一次 / 到期后重新告警）、标签隔离、框重叠判定、空框与 `cooldown=0` 退化、`enabled=false` 短路、有界内存丢最旧、**多线程并发只放行一次**、**身份优先（同 id 框移远也去重 / 同框不同 id 不误杀 / 只有一边有 id 则退回几何）** |
| `test_target_tracker.cpp` | 目标跟踪：id 首次分配/跨帧稳定、快速移动的距离兼底、超出可达范围分裂新 id、标签隔离、遮挡滑行、老化退休后 **id 不复用**、`min_hits` 确认门限、回退帧防御、`enabled=false` 短路、轨迹数有界、dwell 累计、多目标不串号 |
| `test_config_parser.cpp` | 默认值契约（含 `alert.dedup`、`tracking`、`occupancy`）、`source_type` 与 `rtsp://` 前缀交叉校验、`${VAR:-default}` 展开、灰区颠倒、传感器非法组合、去重/跟踪参数越界、**座位 zone 解析与校验（`rect` 展开为 4 顶点 / `polygon` 顶点数 / 重名 / 空 zone / 票数越界）**、新旧模型格式兼容 |
| `test_seat_occupancy.cpp` | 占座判定（33 例）：坐标细节（底边中点 vs 框中心、包含度 vs IoU、多边形边界算“在”）、**C1b 防误判**（人拎包走过 = 随身物品；**错误的人标签不得把物品判成“在手”**）、**C2 人框高度门限一致性**（一个被滤掉的人框不得透过 C1b 把物品排除掉）、C4/C5 交互（**单帧误检暂停计时而不清零**；物品真被拿走才复位）、C6 上升沿（一个占用只出一次事件，人回来再走开可再报）、投票窗口滑出、**时基不变量（N 帧跨 T 毫秒结论不变，与抽帧率无关）**、多座位不串号、`enabled=false` 短路、证据文案可读性、无检测/空座位边界 |
| `test_thread_safe_queue.cpp` | DropOldest 丢最旧、Block 不丢帧、close 排空后再返回 false、超时 pop、on_drop 计数 |
| `test_roi_utils.cpp` | 外扩取整、贴边/越界裁剪、越界判空、负 padding、crop 深拷贝（跨线程送审的安全前提） |

</details>

> CI（`.github/workflows/ci.yml`）在每次 push/PR 上跑同一套命令，`-DCV_TESTS_REQUIRE_GTEST=ON` 保证「测试没跑」不会被当成「测试通过」。

想跑**真实链路**看四个阶段（不动被锁的 `config/config.yaml`，用配置注入）：

```bash
# 复核服务（否则 Phase C 只能看到 unavailable=N），**二选一**：
#   A. 确定性 mock（回归/CI，零依赖）
python3 scripts/mock_review_server.py --port 50052
#   B. 真 VLM（上游 VLM 先起在 :8000，如 vLLM/Ollama/云 API）
pip install -r vlm_review/requirements.txt
python3 -m vlm_review.server --backend openai \
    --base-url http://127.0.0.1:8000/v1 --model Qwen/Qwen2.5-VL-7B-Instruct
#   单独联调（不启动流水线；proto 首次会自动生成）：
./build/review_client 127.0.0.1:50052 frame.jpg "判断该座位是否被长期占座"

# 主程序：把 cascade / review / sensors+fusion / occupancy 逐段打开（改 config/config.test.yaml 的 enabled）
# 注：config.test.yaml 已默认开启 **占座判定**（座位 A-12），会把“[占座] 座位 A-12: ……证据”写进日志
cd build && ./CVInfer-Gate --config config/config.test.yaml
```

<details>
<summary><b>想一次看全「告警外发 + 指标 + 探针 + 日志轮转」（点开看完整命令）</b></summary>

想一次看全 **告警外发 + 指标 + 探针 + 日志轮转**（`config/config.ops.yaml`，不依赖 MySQL/复核服务）：

```bash
# 0) 伪下游：收 webhook 的一行一条日志（另一个终端）
python3 scripts/alert_receiver.py --port 8899 --token demotoken

# 1) 主程序（file 模式跑 test.mp4；库不可用会降级写 CSV，复核不可用走兜底告警）
cd build && ALERT_TOKEN=demotoken ./CVInfer-Gate --config config/config.ops.yaml

# 2) 指标 / 探针 / 指标含义
curl -s http://127.0.0.1:9100/metrics | head -40
GRPC_AUTH_TOKEN=<同 config>  ./CVInfer-Gate --config config/config.ops.yaml --health-check; echo $?  # 0=健康
#   退出码: 0 健康 / 2 连不上 / 3 超时 / 4 未授权 / 5 不健康

# 3) 优雅退出：看"告警推送统计/日志轮转次数/各阶段统计"
kill -TERM <pid>
```

</details>

| 想看的阶段 | 看哪里 |
|---|---|
| A 抽象层 | 启动日志 `模型配置加载成功: ... (模型数=N)` / `使用检测器: xxx` |
| B 级联灰区 | 启动日志 `使用检测器: cascade (级联模式)` + 退出时 `级联统计: primary/triggered/confirmed/rejected/skipped` |
| C 异步复核 | 复核服务端逐条请求日志（`[vlm]`/`[mock]`）+ 客户端启动日志 `复核服务就绪/暂不可用` + 退出时 `复核统计: submitted/reviewed/confirmed/rejected/timeout/unavailable` |
| D 多模态融合 | 退出时 `融合统计: frames/samples/aligned/targets/matched/unmatched_sensor` |
| 占座（规则层） | 启动时 `占座判定: 已启用 (座位数=N, ...)` + 运行中 `[占座] 座位 X: 检出物品 [...] 已连续 N 秒「有物品且无人使用」` + 退出时 `占座统计: frames/seats/occupied_events` 与**逐座位快照** |

> 提示：CMake 的 POST_BUILD 会用源码目录的 `config/` 覆盖 `build/config/`，所以不要改 `build/config/config.yaml`；
> 要改就改源码 `config/config.yaml`，或用 `--config` 指向另一个文件。

## 验证状态

**架构完备度可以，真实资源验证覆盖率低**

| 层 | 状态 | 说明 |
|---|---|---|
| 架构重构 | 🟢 可信 | 11 个架构级 BUG 全修（线程安全/启动时序/优雅关闭/背压/落库）+ **数据库降级 / RTSP 断流重连**（见下）|
| Phase A~D 四层抽象 | 🟢 可信 | `phase_selftest` 52/52，零外部依赖、秒级 |
| 单元测试 / CI | 🟢 **新增** | `ctest` = `cv_unit_tests`(gtest 179 例) + `phase_selftest`，共 **180 项全绿**；GitHub Actions 每次 push/PR 自动跑（纯文档改动跳过）。覆盖清单见上文折叠块 |
| MySQL 落库 | 🟢 可信 | 表已建，程序正常写入（**不再产生 `db_fallback.csv`**）；**库不可用不再退出**：降级写 CSV + 后台自动重连并回传 |
| Phase B 级联 | 🟡 能跑通 / 精度未回归 | 机制可用（`role=classifier` 注册样例见 `config/model_config.yaml` 注释块，与占座业务无关）；占座链路用不到它（`cascade.enabled=false`）。⚠**未做精度回归**（改 `accept_label`/`accept_conf` 无人能自动报警）|
| Phase C 异步复核 | 🟢 真 VLM 已跑通 / 质量未评测 | 服务端已实现（`vlm_review/`：OpenAI 兼容 / 本地 transformers / mock 三后端 + 鉴权 + Health 探活）；已用 vLLM 起 `Qwen2-VL-2B-Instruct-AWQ` 端到端跑通（未留存日志）。⚠**结论准确率未做定量评测** |
| Phase D 多模态融合 | 🟢 可信（stub 雷达）| 端到端实测 `matched=66`；带框传感器见 `config/config.test.yaml` 注释 |
| `web_gateway` (Flask BFF) | 🟢 本地已联调 | **一键启动** `bash web_gateway/run.sh`（自动建 `.venv` + 装依赖 + 生成桩文件）。两页合一：**单图检测**（拖拽上传 / 无刷新结果 / 点列表行高亮框 / 下载结果图 / 在线徽标）+ **结果回看**（在线播放 `output.avi`（按需转码、支持 Range 拖动）+ 占座/告警事件时间线）|
| RTSP 接入 | 🟢 已补强 | 连通已验证；**断流指数退避重连 + `close()` 可中断**（`interrupt_callback` 直接打断 `av_read_frame`），重连对上层透明 |
| 性能 | 🟢 已定档 | 30.2 ± 0.7 FPS（量化上限 ~33）；瓶颈是推理 FLOP，**不在流水线** |
| 可观测性 / 告警外发 | 🟢 **新增** | `/metrics` 20 组指标（Prometheus 文本格式，实测 5 条真告警全部投递到 webhook、死端口场景 `failed=3 retried=6` 且不影响主链路）+ `--health-check` 探针（实测 0/4 退出码）+ 日志文件按大小轮转（5 条单测）；均默认关闭 |
| 占座判定（规则层） | 🟢 端到端实测 | `config/config.test.yaml`（图书馆场景 test.mp4）实测 `占座统计: frames=1332 seats=1 occupied_events=2`，并 `复核统计: submitted=2`（带结构化证据送审）；33 条确定性单测；退出时逐座位快照（运维最常问的“现在哪些座位被判占了”） |

**已知限制**：
1. 结果视频无大小上限；RTSP 重连后若分辨率变化，sink 端 `VideoWriter` 不会自动重建（换流请重启进程）。
2. **告警去重现在是「身份优先」的**：默认启用跟踪 ⇒ 同一 `track_id` 即使框移开也只告警一次（快速移动目标不再重复）。但**无外观(re-ID)特征**，遮挡/交叉后可能换 id（ID switch），那之后仍可能重新告警一次；`tracking.enabled=false` 则退回纯几何去重。另外 `track_id` **未落库**（`detections` 表无该列，需 schema 迁移），gRPC 响应也没带它。
3. **告警外发/指标是后续新增的**，且各带边界：`/metrics` **无鉴权**（只应暴露在受信网络）、webhook 只支持**明文 http**、推送队列**不落盘**（`kill -9` 时未发出的通知会丢，账在 DB）—— 详见下面的「已知边界」。

<details>
<summary><b>工程化里程碑 · 逐轮详版（点开看：问题 → 做法 → 实测依据）</b></summary>

> 上面的表是**结论**；以下是各轮工程化工作的「问题 → 做法 → 实测依据」。时间紧可只看上表，或直接跳到 [PROJECT_NOTES.md](PROJECT_NOTES.md)。

**复核链路鉴权修复（「只有一半」）**：
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
- 仍未做（当时）：~~主服务自己的 gRPC（对外 50051）仍无鉴权~~ ⇒ **主服务鉴权已补（见下）**；TLS（当前是明文共享密钥，内网够用、公网不行）；`scripts/mock_review_server.py` 不校验 token（开发 mock，忽略即可）。

**可观测性与告警外发（"能被看见"的最后一段）**：
- 问题：闭环缺两段 —— ①告警只写 `alerts` 表，**没有人会知道**（去重做得再好也一样）；②运行状态只能翻日志：无指标端点、无健康探针、日志文件只涨不轮转（长时间跑等于慢慢撑爆磁盘）。
- 做法（三件套 + 一件顺带，**默认全关 ⇒ 对存量配置零行为变化**）：
  1. **告警外发** `src/alert/AlertNotifier.{h,cpp}`：`alerts` 表之外的 webhook。**有界队列 + 独立线程** ⇒ 推送绝不阻塞流水线（满则丢最旧，与帧队列同策略）；失败**重试 + 指数退避**（上限 5s）；停机**排空但有预算**（`drain_timeout_ms`），退避期被打断就不再干等、仍立刻再试（由预算兜底）。传输层（`Transport`）可注入 ⇒ 单测不碰网络。
  2. **指标** `src/utils/Metrics.{h,cpp}` + `src/service/MetricsServer.{h,cpp}`：手写注册表（counter/gauge/带标签/**拉式采集器**）+ 极简 HTTP/1.1 端点（`/metrics` Prometheus 文本格式、`/healthz`）——**不引入 prometheus-cpp 依赖**，与项目“不轻易加依赖”的口径一致。
  3. **探针** `--health-check`：单进程模式，调 gRPC `Health`（不加载模型/不连库，秒级）后退出：`0=健康 / 2=连不上 / 3=超时 / 4=未授权 / 5=不健康`。容器 healthcheck 用它（"./CVInfer-Gate --health-check"）。
  4. 顺带：**日志按大小轮转**（`app.log_max_size_mb` / `app.log_keep_files`，默认 `0=不轮转` ⇒ 行为不变；目录不存在会自建，不再“静默丢掉文件日志”）。
- 配置（`config/config.example.yaml` 已带注释，开关默认关；另有开箱即用的 `config/config.ops.yaml`）：
  `metrics.{enabled,bind,port}`、`alert.push.{enabled,url,timeout_ms,max_retries,retry_backoff_ms,max_queue,drain_timeout_ms,header_name,header_value}`、`app.log_max_size_mb`、`app.log_keep_files`。
- webhook 载荷（**下游集成契约**，已用单测钉住）：`POST <url>`、`Content-Type: application/json`，
  `{"source":"cvinfer-gate","alert_type":"图书馆疑似占座违规","description":"...","frame_seq":1039,"label":"person","confidence":0.8700,"track_id":3,"ts_ms":1730000000000}`；**2xx = 送达**，其它（含 4xx/5xx —— 有响应 ≠ 送到）按失败重试。
- 验证（**真跑**：`config/config.ops.yaml` + `scripts/alert_receiver.py` 当伪下游）：
  `/metrics` 输出 20 组指标（`build_info{version}`、`uptime_seconds`、`frames_{decoded,processed,dropped,emitted}`、`detections`、`db_healthy`/`db_reconnects`、`grpc_requests_total{method,code}`、`alerts_{raised,suppressed}`、`alert_push_{sent,failed,dropped}`、`review_*`、`tracks_active`）；探针实测 `OK/EXIT=0`、`无 token/EXIT=4`、`错 token/EXIT=4`，服务端侧对应 `grpc_requests_total{method="Health",code="OK"}=1` 与 `{code="UNAUTHENTICATED"}=2`（“有人在试 token”一眼可见）；`grpc_client` 正常出 2 个目标且 `{method="Detect",code="OK"}=1`；**5 条告警全部投递**（`pushed=5 sent=5 failed=0 dropped=0`，伪下游逐条收到）；把 URL 指向死端口后 `sent=0 failed=3 retried=6 last_error=连接失败: 127.0.0.1:8877`，而**流水线照常跑完并正常出视频/落库**（失败不致命）；退出后 `/metrics` 立即拒连（`curl` 退出码 7）。
- 轮转行为由 5 条单测钉住（`max_size_mb=0` 不轮转 / 超阈值产生 `.1` / `keep_files` 上限 / `=0` 不留归档 / 续写已有文件时把**已有大小**算进预算）—— 实测那次短跑日志不足 1MB，所以退出时 `日志轮转次数: 0` 是**正确**结果（“没触发”就不假装触发）。
- 一处**实跑才发现**的问题（已修）：配置校验原为“`header_name` 非空 ⇒ `header_value` 也必须非空”，但“名字先填好、值等环境变量注入”（`header_value: "${ALERT_TOKEN:-}"`）是很常见的合法用法 ⇒ 探针会直接 `配置校验失败` 退出 255。现改为**值非空才要求名字**（反方向才是真写错），且只在**名字与值都给全**时才真的发这个头。
- 已知边界：①`/metrics` **无鉴权**（与社区惯例一致）⇒ 只该暴露在受信网络：容器里用 `expose`（不发布到宿主机），裸机建议 `bind: 127.0.0.1`；②webhook 只支持 **http://**（无 TLS ⇒ 公网请走内网转发/侧车）；③推送队列**不落盘** ⇒ `kill -9` 时未发出的通知会丢（**账在 DB**，通知可由库侧补偿）；④`--health-check` 必须走**与主进程相同的配置解析**（尤其 `GRPC_AUTH_TOKEN` / `CVINFER_CONFIG`），否则会因“两份环境不一致”误判（compose 的 healthcheck 已按此写）；⑤**Grafana 面板未做**（只给了 `docker/prometheus.example.yml` 抓取配置 + 关键指标清单）。

**主服务（50051）鉴权**：
- 问题：复核链路（50052）补齐后，**对外的 50051 仍是裸的** —— 任何能连到端口的人都能白嫖推理算力，也能靠连通性/响应快慢探测服务是否在线。
- 做法：`src/service/AuthGuard.h`（header-only，语义**逐条对齐**复核链路的 Python 拦截器，两个服务端不飘）：token 为空 = **完全不校验**（零破坏）；比对**整串** `"Bearer " + token`（大小写敏感、无尾空格容错 ⇒ `bearer x` / `Bearer x ` / `Bearer xx`(前缀攻击) 全拒）；只看 metadata 里**第一个** `authorization`（不挑“能过的那个”）；定长比较防前缀时序泄漏；拒绝码 `UNAUTHENTICATED`(16)。
- 配置/接线：`grpc.auth_token`（建议 `"${GRPC_AUTH_TOKEN:-}"`，走 `ConfigParser::expandEnv` ⇒ 口令不进 git）；`DetectionServiceImpl::Detect` **第一行**校验（在任何业务动作之前），拒绝时按 `context->peer()` 记 WARN（日志突增 = 有人在试 token）；启动时显式打印 `[鉴权] 主服务(50051): 开启/关闭`（“以为开了其实没开”是排查噩梦）。
- 客户端同步（否则一开鉴权就断）：`grpc_client` 读 `GRPC_AUTH_TOKEN` + 退出码 `5 = UNAUTHENTICATED`；`web_gateway/app.py` 读同一个变量并给 `stub.Detect(..., metadata=...)` 带上 Bearer；`docker-compose.yml` 把变量透进容器。
- 验证（**真实端到端**，服务端开 token 跑起来，三种调用各来一次）：
  ```bash
  cd build && GRPC_AUTH_TOKEN=s3cr3t ./CVInfer-Gate --config config/config.test.yaml &
  cd .. && ./build/grpc_client 127.0.0.1:50051                    # 期望 code 16(UNAUTHENTICATED) + 退出码 5
  GRPC_AUTH_TOKEN=wrong  ./build/grpc_client 127.0.0.1:50051      # 期望同上(错 token 与没带等价)
  GRPC_AUTH_TOKEN=s3cr3t ./build/grpc_client 127.0.0.1:50051      # 期望「检测成功」+ 退出码 0
  ```
  实测：前两条 `RPC 调用失败: 16 - 缺少或错误的 authorization metadata`、退出码 5 ；第三条 `检测成功 / 检测到目标数量: 1 (bed 0.62)`、退出码 0 ；服务端侧正好 2 条 `[鉴权] 拒绝 ipv4:127.0.0.1:xxxxx 的 Detect` 。**不设 token 重启后**：`auth=off`、同一客户端调用照旧成功、0 条拒绝 ⇒ 零破坏 。
- 已知边界：①**不是** gRPC 拦截器，而是“每个方法入口一行”—— 结构上不可能出现 handler 类型不匹配（C++ 同步服务直接 `return Status`），代价是**新增 RPC 要手动加这一行**；②明文共享密钥（无 TLS）⇒ 只解决“谁都能调”，不解决窃听/重放，公网必须上 TLS 或反向代理；③ token 建议纯 ASCII 且不含空格（其它语言的客户端可能发不出非 ASCII metadata）。

**[占座] 新增：占座判定（卡在「检测」与「VLM 复核」之间的规则层）**：
- 问题：安防/图书馆场景的真实需求是 **“某座位被长期占用”**，而不是“画面里出现了几本书”。但现有链路只能给出“帧级检测框”：既没有“**座位**”这个概念（所以再加一个检测器也拿不到“物品属于哪个座位”），也没有“**持续了多久**”这个维度（所以单帧 VLM 也答不了）；而“时间”恰恰是占座定义的核心（坐了 1 分钟 vs 占了 1 小时）。
- 做法：`src/occupancy/SeatOccupancyAnalyzer.{h,cpp}`（纯逻辑、不依赖模型/OpenCV 图像之外的东西，可脱离模型确定性单测）。逐个座位解三道题：
  1. **物品属于这个座位吗**（C1）—— 物品**底边中点**在座位多边形内，或与座位区**包含度** ≥ 阈值；再用 C1b 排除“被同一帧里的人**拿着/背着**”（“有人拎包从桌边走过”不能记成“包放在桌上”）。
  2. **人在这座位上吗**（C2）—— 人的**底边中点**在区内（脚/臀的位置）或包含度 ≥ 阈值，并可用 `min_person_height_px` 滤远景小框。
  3. **“物品在 ∧ 人不在”持续了多久**（C3/C4/C5）—— 时序状态机：够 `t_occupied_ms` 才判占座；人短暂出现只**暂停**累计（`t_grace_ms` 内不清零，滤“起身接水/路人被误检”）；再加 M-of-N 投票抵抗单帧漏检/误检。
- 关键设计选择（都是拿命换来的，逐条有单测）：①用**底边中点**而不是框中心（斜靠的书包框中心在半空，底边中点才在桌面上）；②物品在人身上用**包含度**而不是 IoU（人框远大于一本书，IoU 算出来只有 ~0.05，必然漏判“随手带走”）；③C1b 与 C2 **共用同一把“可信人框”尺子** —— 否则“一个被 `min_person_height_px` 滤掉的人框”会透过 C1b 把物品错误地排除掉（自相矛盾）；④时基用**墙钟**且投票窗口按帧数 —— “N 帧跨 T 毫秒结论不变”，与抽帧率无关。
- 位置/交互：与 tracker 一样在 **sink 内**（它**有状态**，必须按 `frame_seq` 递增顺序调用；靠 sink 单线程 + 重排缓冲天然串行），且必须在“无检测早退”**之前**跑（“本帧什么都没检出”正是状态机需要的输入）。`enable=true` 时，命中 `item_labels` 的目标**不再**走“逐物体送审/告警”（否则“书在桌上”会报两次、还白烧 VLM 调用）—— 启动日志会显式提醒这条交互。送审时**把结构化证据拼进 prompt**（`【规则引擎证据】座位 A-12: 检出物品 [laptop], 已连续 412 秒“有物品且无人使用”……`），VLM 从“看图猜场景”变成“对证据做裁判”；ROI 就用座位 zone 本身（**所见即所判**，不引入隐藏缩放）。
- 配置：`occupancy.{enabled, seats[], item_labels, person_label, item_seat_overlap, item_person_overlap, person_seat_iou, min_person_height_px, t_occupied_ms, t_grace_ms, vote_n, vote_m}`；座位两种写法都支持（`polygon: [[x,y],...]` 或 `rect: [x,y,w,h]`）。**默认 `enabled=false` ⇒ 对存量配置零行为变化**；`config/config.test.yaml` 已带一个可开箱即跑的座位（`config.example.yaml` 里默认关，带完整调参注释）。
- 验证（**真跑**）：`cd build && ./CVInfer-Gate --config config/config.test.yaml --model-config config/model_config.yaml`，启动日志 `占座判定: 已启用 (座位数=1, 物品类别=[book, bag, laptop], t_occupied=2s, t_grace=5s, vote=7/10)`；运行中出现 `[占座] 座位 A-12: 检出物品 [laptop], 已连续 2 秒“有物品且无人使用”, 最近一次检测到人在座位上是 4 秒前`（这一行就是送给 VLM 的证据）；退出时 `占座统计: frames=1332 seats=1 occupied_events=2` + `复核统计: submitted=2 unavailable=2`（复核服务未起 ⇒ 按 `alert_on_failure=false` 不告警）+ `告警去重统计: allowed=2 suppressed=0`。另有 16 格网格探针用于**定位实际机位下物品落在哪”，避免“zone 画大了把人框进去 ⇒ 永远命中 C2、永不判占座”。
- 已知边界：①座位 zone 是**静态**的（机位固定则座位是常量；机位会动/球机巡航则需先做标定，本层不处理）；②时基是**墙钟**而非帧时间戳（因为 rtsp 下没有可靠的帧时间戳）⇒ 推流刚建立/卡顿时会偏；跑文件源建议配 `target_fps` 让两者接近；③未做跨机位/跨摄像头去重（同一物理座位被两个机位拍到会各报一次）；④`t_occupied_ms` 是**业务口径**（默认 300000 = 5 分钟），上线前应由业务方拍板，不是技术参数。

**目标跟踪（track_id）**：
- 问题：全链没有 `track_id` —— 告警去重只能靠"框重叠"（快速移动目标照样重复），"停留/徘徊"这类**行为分析**更没有立足点。
- 做法：`src/tracking/TargetTracker.h`（纯逻辑、header-only）：关联式跟踪 —— 同标签候选里 IoU 贪心 + **质心距离兼底**（救快速移动目标），失配后**滑行** `max_age_ms`（容忍遮挡/漏检），超时退休；`track_id` 就地回写进 `DetectionResult`（**加法字段**，默认 -1 ⇒ 未启用跟踪的链路零影响）。**id 单调递增且永不复用**（复用会让去重漏报新目标）。
- 位置：sink 内、**融合之后**（跟踪的是最终参与告警的目标集合）。能安全地"有状态"靠两件事：sink 是**单线程**，且重排缓冲保证 `frame_seq` **单调递增** ⇒ 顺序调用天然无并发（仍防御性忽略回退帧）。
- 联动：`AlertGate` 升级为**身份优先** —— 两边都有 id 时比 id（与框怎么移动无关），没有 id 时退回几何重叠 ⇒ 对未启用跟踪的链路零行为变化。画面标注带 `#id`，肉眼可验证"同一个人只有一个 id"。
- 验证：13 条跟踪单测 + 3 条去重身份用例 + 1 条配置用例；退出日志新增 `目标跟踪统计: frames/spawned/retired/active/matched/longest_dwell`。
- 仍未做：`track_id` 未落库（schema 迁移 + `DBWriter` 两处）、gRPC 响应未带该字段、无轨迹预测/外观特征（ID switch 见上）、停留/徘徊的**告警规则**未做（dwell 已可读）。

**告警去重**：
- 问题：告警判定是**每帧**执行的，而「疑似占座违规」这类结论描述的是**目标状态** —— 同一个目标不动会被连续帧反复告警（每秒 N 条）。OVERVIEW §5 原把它列为 🔴 缺失。
- 做法：新增 `src/utils/AlertGate.h`（纯逻辑、header-only、内部 mutex），在**告警链路**上去重：复核路径打在**送审处**（同一目标只送审一次 ⇒ 不可能重复告警，且省掉重复的 VLM 调用；复核回调拿不到框，无法在那里去重），本地规则路径打在**写告警处**。画框/写视频/检测入库**完全不受影响**。
- 语义：冷却窗按**最近一次命中**刷新 ⇒ 目标持续在画面里只告警一次；消失超过 `alert.dedup.cooldown_ms` 后再次出现才重新告警。`alert.dedup.enabled=false` 可一键回退到旧行为（默认开启是刻意的行为修正）。
- 验证：13 条单测（含并发）+ 2 条配置用例；退出日志新增 `告警去重统计: allowed=/suppressed=/tracked=`。

**多模态融合的一处功能盲区（已修）**：
- `SensorFusion::fuse()` 原为 `if (targets.empty() || vision.empty()) return st;` —— 某帧**视觉一个目标都没有**时整个融合阶段直接返回，传感器证据**既不计数也不产出**，把「雷达/红外测到了、视觉漏检了」这个**最该靠融合兜住**的场景静默作废。现改为**只**在「没有任何传感器目标」时提前返回：空视觉帧照常走未关联逻辑 —— 统计口径恢复真实，且 `emit_sensor_only=true` 时**带框**传感器目标可独立成为目标（默认 `false` ⇒ 只统计不产出，对存量配置**零行为影响**）。已由 `tests/unit/test_sensor_fusion.cpp` 的 `EmptyVisionStillAccountsSensorEvidence` / `EmptyVisionEmitsBoxCarryingSensorOnlyTarget` / `EmptyVisionStillRefusesBoxlessSensorTarget` 三条钉住。

**两条历史限制（已修）**：
1. 数据库不可用时程序**直接退出** → 现为**降级模式**：记录先落本地 CSV，后台按 `reconnect_interval_ms` 自动重建连接池，恢复后回传（启动只做 1 次快速连库尝试，不再白等 ~20s）。
2. RTSP 断流不重连 / `close()` 打不断 `av_read_frame` → 现为 `interrupt_callback` 打断 + 读循环内**指数退避重连**（0.5s→8s 封顶），且**未新增任何接口**。

> 完整推导、逐条实测数据与踩坑记录都在 **[PROJECT_NOTES.md](PROJECT_NOTES.md)**（§20.12–§20.15 = 性能与收尾对账）；想**按文件**读代码先看 **[docs/READING_MAP.md](docs/READING_MAP.md)**。

</details>

## 面向对象（目标人群）

> 适用场景见下文「应用价值 · 能直接落地的场景」。一句话：任何「视频流 +（可选）少量非视频传感器 + 需要落库与告警」的实时检测场景都能套。

- **CV / 后端工程师**：想用 C++ 替换高并发 Python 推理后端，又不愿从零搭流水线与工程基建。
- **算法转工程**：已有 YOLO 权重，需要「模型 → 服务」的完整闭环（推理、后处理、复核、落库、监控）。
- **学生 / 毕设 / 作品集 / 面试**：一个「架构像生产、每一层设计取舍都能讲清」的参考实现。
- **二次开发者**：需要一套「能力可插拔、默认全关、增量接入零破坏」的网关骨架。

## 应用价值

### 1. 能直接落地的场景

| 场景 | 靠哪些能力 | 打开哪个开关 |
|---|---|---|
| 图书馆 / 自习室**占座** | 占座规则层（筛）+ VLM 复核（判） | `occupancy.enabled` |
| 工地 / 车间**安全合规** | 二级分类器 / VLM 复核 | `cascade.enabled` · `review.enabled` |
| 无人货架 / 区域盘点 | 检测 + 落库 + webhook | `alert.push.enabled` |
| 园区周界（视频 + 雷达/红外） | 多模态决策级融合 | `fusion.enabled` |
| 接进你自己的系统 | gRPC `:50051` + webhook 回调 | — |

### 2. 成本上真的省

- **VLM 调用量降 1~2 个数量级** —— 规则层先筛再送审。自建 VLM 方案里最贵的一项，就是“每帧每个目标都去问一次大模型”。
- **纯 CPU 就有 30.2 FPS** —— 中小规模场景不用买 GPU，一台机器起步。
- **单点故障不致命** —— 数据库连不上先写 CSV（后台自动回连补传）、复核服务没起走兜底策略 ⇒ 不会因为一个依赖挂掉就停摆。

### 3. 换业务的成本很低

- **换场景**：改 `review.prompt` + 选 `VLM_SCENARIO`（业务规则在配置里，不在代码里）
- **换复核服务**：只改 `review.endpoint` —— OpenAI 兼容 / 本地 transformers / mock 三选一，**C++ 侧零改动**
- **加新能力**：一律“默认关闭 + 加法字段” ⇒ 接进来**不影响存量配置**

### 4. 当工程参考的价值

- 每一层（模型抽象 / 级联 / 复核 / 融合 / 跟踪 / 占座 / 去重 / 告警外发）都是**接口分层 + 可注入替身** ⇒ 能脱离模型、脱离硬件做单测
- **180 项测试全绿**，其中 `phase_selftest` 一次跑完四层架构（零外部依赖、秒级）⇒ 改代码心里有底
- 文档即资产：阅读地图 / Runbook / 开发史与踩坑记录齐全 ⇒ 接手成本低

## 未来可完善的方向

> 分四类：**性能** / **精度与评测** / **工程健壮性** / **产品化**。每条都说明“为什么值得做”。

### 性能

| 方向 | 预期收益 | 备注 |
|---|---|---|
| **INT8 量化**（唯一剩下的性能杠杆） | 1.5~2.5×（本机 AVX-VNNI；SPR/Xeon 带 AMX 收益更大） | **拿精度换速度**，必须与 FP32 做同视频一致率对比后再上 |
| 更小 / 更专的检测器 | 直接抬高 FPS 天花板 | 场景专用检测器已入库（`models/library_det.*`），可继续剪枝 / 蒸馏 |
| ~~OpenVINO 异步推理~~ | **实测不会提速** | 推理侧已饱和，加并发只是多烧 CPU（PROJECT_NOTES §20.13/§20.14）。**避免重复踩坑** |

### 精度与评测（当前最大的空白）

- **建一个小规模评测集**（几十张标注图 + 一段带占座的视频）：现在“结论准不准”完全没数，改 prompt / 阈值没有任何护栏 —— 有了它才能把“复核质量”变成可回归的数字。
- **级联 B 阶段的精度回归**：`accept_label` / `accept_conf` 改了也没人报警。
- **ID switch 评测**：跟踪没有外观特征，遮挡 / 交叉后会换 id。

### 工程健壮性

- **TLS / mTLS**：当前是明文共享密钥 —— 内网够用，公网必须上（同时解决窃听与重放）。
- **`track_id` 落库**（schema 迁移 + `DBWriter` 两处）+ gRPC 响应带上该字段。
- **结果视频上限**：长时间跑会一直涨（分段写 / 滚动覆盖）。
- **RTSP 重连后分辨率变化**：sink 端 `VideoWriter` 需自动重建（现在要重启进程）。
- **推送队列落盘**：`kill -9` 时未发出的通知会丢（现在靠 DB 侧补偿）。
- **多机位去重**：同一物理座位被两个机位拍到会各报一次。
- **座位自动标定**：座位 zone 现在是静态手填的，机位会动 / 球机巡航时需要标定。

### 产品化

- **Grafana 面板** —— Prometheus 那一半已经有了（`/metrics` + `docker/prometheus.example.yml`），缺的是面板。
- **告警通知渠道** —— webhook 之外接钉钉 / 企微 / 短信。
- **网页端实时监控** —— 支持 RTMP / WebRTC 推流，边跑边看（现在是跑完回看 `output.avi`）。
- **座位可视化配置** —— 在网页上点选座位 zone（现在是手填 `[x,y,w,h]`，已有 16 格网格探针辅助定位）。

