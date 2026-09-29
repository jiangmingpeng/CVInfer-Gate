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
- **微服务与网络通信**：基于 gRPC + Protobuf 提供远程调用接口
- **容器化部署**：提供完整的 Dockerfile 与 docker-compose.yml，一键拉起 MySQL、C++ 网关与 Python Web 网关
- **Web 交互演示**：提供极简 Flask 网关，支持浏览器上传图片并实时返回带框结果
- **多模态决策级融合（可选）**：视频(主模态) + 雷达/红外(统一时间戳采样)，时间对齐 + 目标关联 + 加权置信度融合；默认关闭，关闭时与纯视觉链路行为完全一致
- **可自检的四层架构**：模型抽象 / 级联复核 / 异步大模型复核 / 多模态融合 均为接口分层，可被测试内替身替换；`phase_selftest` 一次跑完四个阶段（零外部依赖，秒级）

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
├── scripts/          # 建表脚本 schema.sql / 复核服务 mock / 传感器回放示例
├── docker/           # Dockerfile 与 Compose 编排
└── tests/            # gRPC 客户端 + Phase A~D 阶段自检 (phase_selftest)
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
```

想跑**真实链路**看四个阶段（不动被锁的 `config/config.yaml`，用配置注入）：

```bash
# 复核服务 mock（否则 Phase C 只能看到 unavailable=N）
pip install grpcio grpcio-tools
mkdir -p build/pyproto
python3 -m grpc_tools.protoc -I proto --python_out=build/pyproto \
    --grpc_python_out=build/pyproto proto/review.proto
python3 scripts/mock_review_server.py --port 50052

# 主程序：把 cascade / review / sensors+fusion 逐段打开（改 config/config.test.yaml 的 enabled）
cd build && ./CVInfer-Gate --config config/config.test.yaml
```

| 想看的阶段 | 看哪里 |
|---|---|
| A 抽象层 | 启动日志 `模型配置加载成功: ... (模型数=N)` / `使用检测器: xxx` |
| B 级联灰区 | 启动日志 `使用检测器: cascade (级联模式)` + 退出时 `级联统计: primary/triggered/confirmed/rejected/skipped` |
| C 异步复核 | mock 进程的逐条请求日志 + 退出时 `复核统计: submitted/reviewed/confirmed/rejected/timeout/unavailable` |
| D 多模态融合 | 退出时 `融合统计: frames/samples/aligned/targets/matched/unmatched_sensor` |

> 提示：CMake 的 POST_BUILD 会用源码目录的 `config/` 覆盖 `build/config/`，所以不要改 `build/config/config.yaml`；
> 要改就改源码 `config/config.yaml`，或用 `--config` 指向另一个文件。

## 验证状态（本项目“结尾”时的账目）

**一句话：架构完备度高，真实资源验证覆盖率低。**

| 层 | 状态 | 说明 |
|---|---|---|
| 架构重构 T12–T35 | 🟢 可信 | 11 个架构级 BUG 全修（线程安全/启动时序/优雅关闭/背压/落库）|
| Phase A~D 四层抽象 | 🟢 可信 | `phase_selftest` 50/50，零外部依赖、秒级 |
| MySQL 落库 | 🟢 可信 | 表已建，程序正常写入（**不再产生 `db_fallback.csv`**）|
| Phase B 级联 | 🟡 仅自检背书 | **本仓库无 `role=classifier` 模型**，开了会退化成单模型 |
| Phase C 异步复核 | 🟡 仅 mock 验证 | 真 VLM 服务端未接（`scripts/mock_review_server.py`）|
| Phase D 多模态融合 | 🟢 可信（stub 雷达）| 端到端实测 `matched=66`；带框传感器见 `config/config.test.yaml` 注释 |
| `web_gateway` (Flask BFF) | 🟡 未联调 | 需先 `cp web_gateway/env.example web_gateway/.env` |
| RTSP 接入 | 🟡 连通已验证 | **无断流重连**；`close()` 打不断 `av_read_frame` |
| 性能 | 🟢 已定档 | 30.2 ± 0.7 FPS（量化上限 ~33）；瓶颈是推理 FLOP，**不在流水线** |

**已知限制（未修，属已知取舍）**：数据库不可用时程序**直接退出**而非降级；RTSP 断流不重连；结果视频无大小上限。

> 完整推导、逐条实测数据与踩坑记录都在 **[PROJECT_NOTES.md](PROJECT_NOTES.md)**（§20.12–§20.15 = 性能与收尾对账）。

## 目前打算的拓展方向
- **INT8 量化**（唯一剩下的性能杠杆）：预计 1.5~2.5×（本机 AVX-VNNI）；阿里云 SPR/Xeon 带 AMX 收益更大 —— 但这是**拿精度换速度**，必须与 FP32 做同视频一致率对比
- **OpenVINO 异步推理（Async Infer）**：⚠️ 本项目的实测结论是**它不会提速**（推理侧已饱和，加并发只是多烧 CPU），见 PROJECT_NOTES §20.13/§20.14
- 支持 RTMP/WebRTC 实时视频流推流，实现网页端实时监控
- 引入 Prometheus + Grafana 监控推理延迟与系统资源

