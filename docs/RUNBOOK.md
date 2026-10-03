# CVInfer-Gate 全流程 Runbook

> **一条命令链跑通完整链路**：file 源 → 推理 → Phase B 级联 → 真 VLM 复核 → Phase D 融合 → 跟踪去重 → 落库 / 告警外发 → 可观测 → 优雅退出。
> 标注：🧰 所需环境/工具　▶ 命令　👀 启动后应看到的效果。
> 实测环境：g++ 15.2 · CMake 4.2.3 · OpenVINO 2026.4.0 · OpenCV 4.10 · gRPC++ 1.51 · MySQL Connector 1.1.12 · Python 3.14。

---

## 0. 环境与工具

| 依赖 | 版本 / 位置 | 用在哪 |
|---|---|---|
| g++ / CMake | C++17 / ≥ 3.16 | 编译 |
| OpenVINO Runtime | 2026.4.0（默认 `/opt/intel/openvino_2026.4.0`） | CPU 推理 |
| OpenCV 4 / FFmpeg | 4.10 | 解码、画框 |
| gRPC++ / protobuf | 1.51 | 对外接口 |
| yaml-cpp / MySQL Connector/C++ | 1.1.12 | 配置解析 / 落库 |
| MySQL Server | 本机或 docker(3306) | 落库 |
| Python + venv | 3.14（PEP 668：**必须** venv） | 复核服务 / Web / 伪下游 |
| vLLM（可选） | Qwen2.5-VL-7B | 真 VLM 复核 |

仓库自带素材：`models/{yolov8n,helmet_cls}.{xml,bin}` + `models/*_labels.txt`、`test.mp4`。

---

## 1. 构建

🧰 上表的 C++ 依赖

```bash
cd /home/jmp/CVInfer-Gate
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

👀 产出（`build/` 下）：`CVInfer-Gate`、`grpc_client`、`review_client`、`phase_selftest`、`cv_unit_tests`。
冒烟自检（零外部依赖，秒级）：

```bash
ctest --output-on-failure   # 期望 129/129
./phase_selftest            # 期望 52 项通过, 0 项失败（退出码 0）
```

---

## 2. Python 环境

🧰 Python 3 + venv（本机 PEP 668，`pip install` 直装会报错，必须 venv）

```bash
cd /home/jmp/CVInfer-Gate
python3 -m venv .venv && source .venv/bin/activate
pip install -r vlm_review/requirements.txt     # 复核服务端（仅 grpcio / grpcio-tools）
pip install -r web_gateway/requirements.txt    # Web 网关（flask/opencv/numpy…）

# 复核 proto 的 Python 桩（vlm_review 首次启动也会自动生成）
mkdir -p build/pyproto
python3 -m grpc_tools.protoc -I proto \
    --python_out=build/pyproto --grpc_python_out=build/pyproto proto/review.proto
```

> 每开新终端都要 `source .venv/bin/activate`，否则 `import grpc` 失败。

---

## 3. 配置

🧰 无。以 `config/config.test.yaml` 为基线（file 源 + Phase B/C/D 全开），按需覆盖：

| 段 | 键 | 全流程取值 | 说明 |
|---|---|---|---|
| `video` | `source_type` / `source_path` | `file` / `../test.mp4` | 相对**启动目录**（`build/`）解析 |
| `pipeline` | `worker_threads` | `2` | 必须 ≤ `model_config.yaml` 的 `models[].pool_size` |
| `database` | `password` | `${DB_PASSWORD:-…}` | 口令走环境变量，不落明文 |
| `cascade` | `enabled` | `true` | Phase B；`secondary` 必须 = `helmet_classifier` |
| `review` | `enabled` / `endpoint` | `true` / `127.0.0.1:50052` | Phase C，指向 vlm_review |
| `fusion` | `enabled` | `true` | Phase D（stub 雷达，无需硬件） |
| `alert.push` | `enabled` | `true`（可选） | 告警 webhook 外发 |
| `metrics` | `enabled` | `true`（可选） | `/metrics` 端点 |

⚠️ 改**源码**的 `config/*.yaml`，别改 `build/config/`（构建时会被源码覆盖）。

---

## 4. 起依赖

### 4.1 MySQL（落库）

🧰 MySQL Server（本机装或 docker）

```bash
sudo service mysql start
sudo mysql < /home/jmp/CVInfer-Gate/scripts/schema.sql     # 建库 cv_infer + 表（幂等）
export DB_PASSWORD=<你的 root 口令>                        # config 里是 "${DB_PASSWORD:-…}"
```

👀 起主程序后 `cvinfer_db_healthy 1`，且不再产生 `build/db_fallback.csv`。

### 4.2 复核服务（真 VLM，Phase C）

🧰 vLLM（或任意 OpenAI 兼容端点）+ venv

```bash
# (1) 上游 VLM
vllm serve Qwen/Qwen2.5-VL-7B-Instruct --port 8000

# (2) gRPC 适配层：把 C++ 送来的 ROI 转成 chat 请求发给 VLM
python3 -m vlm_review.server --backend openai \
    --base-url http://127.0.0.1:8000/v1 --model Qwen/Qwen2.5-VL-7B-Instruct --port 50052
```

👀 终端打印监听 `0.0.0.0:50052`。
鉴权（可选，两端同一个 token）：服务端 `VLM_AUTH_TOKEN=s3cr3t python3 -m vlm_review.server …`，网关侧 `export VLM_TOKEN=s3cr3t`，并让 `review.auth_token: "${VLM_TOKEN:-}"`。

### 4.3 告警伪下游（可选）

🧰 仅 Python 标准库

```bash
python3 /home/jmp/CVInfer-Gate/scripts/alert_receiver.py --port 8899 --token demotoken
# 主程序侧对应：ALERT_TOKEN=demotoken
```

---

## 5. 启动主程序

🧰 上面依赖已就绪（MySQL 在跑、复核服务在跑）

```bash
cd /home/jmp/CVInfer-Gate/build
ALERT_TOKEN=demotoken ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.test.yaml
```

> 配置用**绝对路径**，避免被 CMake `POST_BUILD` 拷进 `build/config/` 的那份绕晕。

👀 **启动期逐行核对（实测原文）**：

```
[ConfigParser] 系统配置加载成功: .../config/config.test.yaml
[ConfigParser] 模型配置加载成功: config/model_config.yaml (模型数=2)
[FileVideoSource] 视频源打开成功: ../test.mp4 (720x1280)
[OpenVINOEngine] 输入尺寸: 640x640 ; device=CPU, performance_mode=latency, num_threads=4
[InferenceEnginePool] 初始化完成, 引擎数量: 2
[ModelPoolManager] 初始化完成, 模型数: 2
使用检测器: cascade (级联模式)          ← Phase B 唯一判据；出不来 = 静默退化为单模型
目标跟踪: 已启用 (iou=0.3, max_age=1000ms)
gRPC 服务已启动, 监听: 0.0.0.0:50051
[指标] /metrics 端点已启动, 端口 9100
服务就绪, 按 Ctrl+C 退出。
[T35] 引擎分段/帧: 预处理=3.55ms  推理=124.65ms  (累计 100 帧)    ← 每 100 帧一次，看 FPS 靠它
```

- 复核连通时**不**出现 `[GrpcLlmReviewer] 复核服务暂不可用(...)`；出现即没连上 50052。
- ⚠️ `decoded` 远大于 `processed` 属**正常**（file 源 30fps、单帧推理 ~125ms，队列 `drop_oldest` 丢帧不积压）。想让每帧都处理，用 `target_fps: 10`。

---

## 6. 启动后的效果 / 对外面

| 看什么 | ▶ 命令 | 👀 期望 |
|---|---|---|
| 指标 | `curl -s http://127.0.0.1:9100/metrics \| head -40` | 20 组 `cvinfer_*` |
| 健康探针 | `./CVInfer-Gate --config <cfg> --health-check` | `[health-check] OK addr=127.0.0.1:50051 version=1.0.0 … detector=…`，退出码 `0` |
| gRPC | `./grpc_client` | `检测到目标数量: N`，退出码 `0` |
| Web | `cd web_gateway && cp env.example .env && python3 app.py` | 浏览器 `localhost:8080` 上传图片 → 返回带框图 |
| 复核直连 | `./review_client 127.0.0.1:50052 /tmp/frame.jpg "判断该人员是否未佩戴安全帽"` | 打印 backend + 结论行 |

运行中产物：`build/output.avi`（带框视频）、`build/logs/cvinfer.log`（若开 + 轮转）、MySQL `cv_infer.detections / alerts`、伪下游逐条打印告警。

实测 `/metrics` 关键行（对照数字是否合理）：

```
cvinfer_frames_decoded_total 1332   cvinfer_detections_total 590
cvinfer_alerts_raised_total 3       cvinfer_alerts_suppressed_total 86   ← 去重真的在干活
cvinfer_db_healthy 1                cvinfer_alert_push_sent_total 3
```

---

## 7. 停止（顺序即正确性）

```bash
pkill -TERM -f './CVInfer-Gate --config'
```

👀 关闭时逐项结算（实测原文）：

```
收到信号 15, 开始优雅关闭...
流水线统计: decoded=… dropped=… processed=… emitted=…
级联统计: primary=… triggered=… confirmed=… rejected=… skipped=…
复核统计: submitted=N reviewed=N confirmed=? rejected=? timeout=0 unavailable=0   ← 连通时 unavailable 必须 0
融合统计: frames=… samples=… matched=…
告警去重统计: allowed=… suppressed=…
目标跟踪统计: frames=… active=… longest_dwell=…ms
告警推送统计: pushed=… sent=… failed=… dropped=… retried=…
结果视频已保存至 output.avi
已安全退出。
```

---

## 8. 常见坑（实测）

1. **改配置改错文件**：构建会用源码 `config/` 覆盖 `build/config/` ⇒ 改源码，或 `--config` 指绝对路径。
2. **相对路径按 CWD（`build/`）解析**：`../test.mp4`、`db_fallback.csv`、`logs/`。
3. **`worker_threads` 必须 ≤ `models[].pool_size`**，否则 worker 借不到引擎白等。
4. **`device` 必须显式写 `CPU`**：`AUTO` 在带 NPU 插件的 WSL 会段错误。
5. **级联静默退化**：`cascade.secondary` 必须与 `model_config.yaml` 模型名完全一致。
6. **复核全 `unavailable`**：上游 VLM 没起 / `--base-url` 不对 / 两端 token 不一致。
7. **RTSP**：`source_type` 与 `source_path` 两个键都要改；抽帧用 `frame_interval`，别用 `target_fps`。
