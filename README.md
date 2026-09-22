# CVInfer-Gate: 基于 C++ 与 OpenVINO 的高性能 AI 推理网关

![C++](https://img.shields.io/badge/C++-17-blue.svg)
![OpenVINO](https://img.shields.io/badge/OpenVINO-2025.4.0-green.svg)
![Docker](https://img.shields.io/badge/Docker-Enabled-blue.svg)
![gRPC](https://img.shields.io/badge/gRPC-Supported-orange.svg)

## 项目简介
CVInfer-Gate 是一个面向安防/工地场景（如安全帽检测）的高性能 AI 推理网关 针对传统 Python 后端在高并发视频流处理中存在的 GIL 锁限制和内存管理混乱问题 本项目底层完全采用 **C++17** 构建，结合 **OpenVINO** 实现高性能推理，并通过 **gRPC** 对外提供微服务接口 

项目采用了 **BFF（Backend for Frontend）** 架构，核心推理由 C++ 保障极致性能，前端交互由轻量级 Python Flask 网关进行 HTTP 协议转换 实现了性能与开发效率的平衡

## 核心特性
- **高性能多线程流水线**：基于生产者-消费者模型，手写线程安全队列，实现零拷贝与跳帧策略，防止视频流积压
- **AI 推理引擎**：使用 OpenVINO (C++) 加载 YOLO 模型，手写 NMS (非极大值抑制) 后处理，纯 CPU 推理吞吐达 26+ FPS
- **数据持久化**：集成 MySQL Connector/C++，实现检测记录与异常告警的异步落库
- **微服务与网络通信**：基于 gRPC + Protobuf 提供远程调用接口
- **容器化部署**：提供完整的 Dockerfile 与 docker-compose.yml，一键拉起 MySQL、C++ 网关与 Python Web 网关
- **Web 交互演示**：提供极简 Flask 网关，支持浏览器上传图片并实时返回带框结果

## 系统架构
```text
[视频源/RTSP] ──(FFmpeg)──> [C++ 多线程队列] ──(OpenVINO)──> [YOLO 后处理/NMS]
                                                                  │
[Web 浏览器] <──(HTTP)── [Python Flask BFF] <──(gRPC)─────────────┤
                                                                  │
                                                       [MySQL 数据持久化]


技术栈
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


快速开始 (Docker 部署)

要求
Ubuntu 22.04 或更高版本
Docker 与 Docker Compose

1. 克隆项目
git clone https://github.comjiangmingpeng/CVInfer-Gate.git
cd CVInfer-Gate

目录结构
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
│   └── utils/        # 线程安全队列、配置解析
├── web_gateway/      # Python Flask BFF 网关
├── docker/           # Dockerfile 与 Compose 编排
└── tests/            # gRPC 客户端测试代码


2. 准备模型与视频
将 YOLO 的 OpenVINO 模型文件 (yolov8n.xml, yolov8n.bin) 放入 models/ 目录
准备一个测试视频 test.mp4 放在项目根目录

3.一键启动
cd docker
docker compose build
docker compose up -d

4. 验证服务
查看后端日志：docker compose logs -f cv-infer-gate
浏览器访问 Web 网关：http://localhost:8080，上传图片即可看到检测结果
或使用 gRPC 客户端测试：./build/grpc_client


目前打算的拓展方向
引入 OpenVINO 异步推理 (Async Infer) 与 INT8 量化，尽量达到60+ FPS
支持 RTMP/WebRTC 实时视频流推流，实现网页端实时监控
引入 Prometheus + Grafana 监控推理延迟与系统资源

