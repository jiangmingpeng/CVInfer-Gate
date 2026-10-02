# CVInfer-Gate 全流程跑通手册（Runbook）

> 目标：从"刚 clone / 刚构建"到"每一层都亲眼看到它工作"，按 **P0 → P8** 顺序做。
> 每一步都有 **命令** + **期望输出**（本机实测，2026-10-02，OpenVINO 2026.4.0 / g++ 15.2 / Python 3.14.4）。
> 记号：✅ = 本机已实测通过；⚠️ = 有坑，先读再跑。

---

## §0 先体检：这台机器现在缺什么

**已具备**（无需安装）：CMake 4.2.3 · g++ 15.2 · OpenVINO 2026.4.0（`/opt/intel/openvino_2026.4.0`）· OpenCV 4.10 · gRPC++ 1.51 · protobuf · **gtest 1.17** · **MySQL Connector/C++ 1.1.12** · yaml-cpp · FFmpeg/ffprobe · mediamtx 二进制（仓库自带）· curl/ss。
**已经构建好了**：`build/CVInfer-Gate`、`grpc_client`、`review_client`、`phase_selftest`、`cv_unit_tests`。
**缺**（本机实测）：

| 缺什么 | 影响哪一步 | 怎么补（见下） |
|---|---|---|
| MySQL 服务 + 客户端 | P3 落库、P5 演示 | `sudo apt install mysql-server`（或 docker） |
| docker daemon **未启动** | P9 一键部署 | `sudo service docker start`（本机 `docker` 命令在，但 `docker info` 报不可用） |
| Python 包 `grpcio/flask/opencv-python/numpy` | P4/P5 | 见 §1 的 venv（**本机有 PEP 668 限制**：`/usr/lib/python3.14/EXTERNALLY-MANAGED` 存在，`pip3 install` 会直接报错，必须用 venv 或 `--break-system-packages`） |

> **好消息**：P0、P1、P2、P7、P8 **现在就能跑**，一行依赖都不装。

---

## §1 一次性准备（venv + 两个口令）

```bash
# 1) Python 虚拟环境（本机必须这样装，因为 EXTERNALLY-MANAGED）
cd /home/jmp/CVInfer-Gate
python3 -m venv .venv && source .venv/bin/activate
pip install -r web_gateway/requirements.txt      # flask/grpcio/grpcio-tools/opencv/numpy/dotenv
pip install -r vlm_review/requirements.txt       # grpcio/grpcio-tools（复核服务端用）

# 2) 生成复核 proto 的 Python 桩（mock/VLM 服务端都要它；C++ 侧由 CMake 自动生成）
mkdir -p build/pyproto
python3 -m grpc_tools.protoc -I proto \
    --python_out=build/pyproto --grpc_python_out=build/pyproto proto/review.proto
ls build/pyproto            # 期望看到 review_pb2.py  review_pb2_grpc.py
```

> 以后每次新开终端都要 `source .venv/bin/activate`；不激活时 `python3 -c "import grpc"` 会失败。

---

## §2 P0 — 5 分钟验证（零依赖，先证明二进制是好的）

```bash
cd /home/jmp/CVInfer-Gate/build
ctest --output-on-failure        # ✅ 129/129
./phase_selftest                 # ✅ 52 项通过, 0 项失败（退出码 0）
```

**期望**：`100% tests passed, 0 tests failed out of 129` + `===== 结果: 52 项通过, 0 项失败 =====`。
失败就先修环境，别往下走。

---

## §3 P1 — 文件模式真跑（**不依赖 MySQL、不依赖复核服务**）✅ 已实测

用 `config/config.ops.yaml`：它刻意用 `source_type: file` + `../test.mp4`、库连不上就降级写 CSV、复核连不上就走兜底告警 —— **一个人也能把"推理 → 去重 → 跟踪 → 推送 → 指标 → 日志轮转 → 优雅退出"全跑满**。

**终端 A**（伪下游，纯标准库，**不需要 venv**）：
```bash
cd /home/jmp/CVInfer-Gate
python3 scripts/alert_receiver.py --port 8899 --token demotoken
# 期望: [receiver] 监听 http://0.0.0.0:8899/alert(需要 x-alert-token)
```

**终端 B**（主程序）：
```bash
cd /home/jmp/CVInfer-Gate/build
ALERT_TOKEN=demotoken ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.ops.yaml
```
> 用**绝对路径**指 config，免得被 CMake `POST_BUILD` 的 `config/` 覆盖绕晕。相对路径（`../test.mp4`、`db_fallback.csv`、`logs/`）都以**当前工作目录**（这里是 `build/`）为基准。

**启动期该逐行核对（实测原文）**：
```
[ConfigParser] 系统配置加载成功: .../config/config.ops.yaml
[ConfigParser] 模型配置加载成功: config/model_config.yaml (模型数=2)
[Logger] 日志轮转已开启: logs/cvinfer.log (单文件上限 1MB, 保留 3 份)
=== CVInfer-Gate 启动 ===
[ConnectionPool] 连接 1/4 第 1/1 次失败: Can't connect to MySQL server on '127.0.0.1:3306' (111)
[DBWriter][T36] 数据库不可用, 以【降级模式】启动: 记录先落本地 db_fallback.csv ... (程序不再因此退出)   ← 不退出是关键
告警推送: 已启用 -> http://127.0.0.1:8899/alert (timeout=3000ms, retries=2, queue=256)
[FileVideoSource] 视频源打开成功: ../test.mp4 (720x1280)
[OpenVINOEngine] 输入尺寸: 640x640 ; device=CPU, performance_mode=latency, num_threads=4
[InferenceEnginePool] 初始化完成, 引擎数量: 2
[YoloDetector] 初始化完成: name=yolov8_detector, pool=2, labels=81, conf=0.25, nms=0.45
[BehaviorClassifier] 初始化完成: name=helmet_classifier, pool=2, input=416x416
[ModelPoolManager] 初始化完成, 模型数: 2
使用检测器: yolov8_detector (单模型模式)      ← ops 配置没写 cascade 段；想看级联去 P6
[GrpcLlmReviewer] 复核服务暂不可用(...Connection refused); 运行期将按 alert_on_failure 兜底, 不阻塞流水线
目标跟踪: 已启用 (iou=0.3, max_age=1000ms)
[鉴权] 主服务(50051): 关闭(token 为空 => 不校验, 等价改动前行为)
gRPC 服务已启动, 监听: 0.0.0.0:50051
[指标] /metrics 端点已启动, 端口 9100 (无鉴权, 仅建议在内网/受信网络暴露)
服务就绪, 按 Ctrl+C 退出。
[T35] 引擎分段/帧: 预处理=3.55ms  推理=124.65ms  (累计 100 帧)      ← 每 100 帧打一次，看 FPS 就靠它
[T35] 后处理耗时/帧: 4.58 ms  (累计 100 帧)
```
⚠️ **`decoded` 会远大于 `processed`**（实测 `decoded=1332 dropped=1184 processed=148`）—— 这是**正常**的：源 30fps、每帧推理 ~125ms，队列按 `drop_oldest` 丢帧而不是积压。想让每帧都被处理，见 P6（`target_fps: 10`）。

**终端 C**（边跑边看）：
```bash
curl -s http://127.0.0.1:9100/metrics | head -40     # ✅ 实测 20 组指标
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:9100/healthz   # ✅ 200
cd /home/jmp/CVInfer-Gate && ./build/CVInfer-Gate --config $PWD/config/config.ops.yaml --health-check; echo "EXIT=$?"
# ✅ 期望: [health-check] OK addr=127.0.0.1:50051 version=1.0.0 uptime_ms=... detector=yolov8_detector / EXIT=0
```
实测 `/metrics` 关键行（对照你的数字是否合理）：
```
cvinfer_frames_decoded_total 1332        cvinfer_detections_total 590
cvinfer_alerts_raised_total 3            cvinfer_alerts_suppressed_total 86   ← 去重真的在干活
cvinfer_alert_push_sent_total 3          cvinfer_db_healthy 0                 ← 0=降级中
```

**收尾（优雅退出，账目最全）**：在主程序终端按 `Ctrl+C`，或另开终端：
```bash
pkill -TERM -f './CVInfer-Gate --config'
```
实测退出账目：
```
收到信号 15, 开始优雅关闭...
流水线统计: decoded=1332 dropped=1184 processed=148 emitted=148
复核统计: submitted=3 dropped=0 reviewed=3 confirmed=0 rejected=0 timeout=0 unavailable=3 failed=0
融合统计: frames=148 samples=1128 dropped=872 aligned=75 targets=75 matched=66 unmatched_sensor=9
告警去重统计: allowed=3 suppressed=86 tracked=3
目标跟踪统计: frames=139 spawned=32 retired=23 active=9 matched=519 longest_dwell=9414ms
告警推送统计: pushed=3 sent=3 failed=0 dropped=0 retried=0
结果视频已保存至 output.avi
日志轮转次数: 0
已安全退出。
```
**产物核对**：
```bash
ls -l build/output.avi build/db_fallback.csv build/logs/
wc -l build/db_fallback.csv     # ✅ 实测 2404 行（detection + alert 混排）
head -3 build/db_fallback.csv
```
```
detection,59,bed,0.645203,1,4,719,1271
detection,0,person,0.612502,0,1,719,1278
alert,安全帽缺失,"复核不可用(unavailable), 按兜底策略告警, frame=0"
```
伪下游终端应逐条打印（实测 3 条，全部 200）：
```
[receiver] #1 安全帽缺失 | 复核不可用(unavailable), 按兜底策略告警, frame=1 | ... | total_received=1
[receiver] 127.0.0.1 - "POST /alert HTTP/1.1" 200 -
```
⚠️ `日志轮转次数: 0` 是**正确**的：跑 2 分钟只写了 21KB，没到 1MB 阈值（阈值是整数 MB，短跑看不到轮转；想看轮转请长时间跑到超阈值）。

---

## §4 P2 — 探针的 4 种退出码（容器 healthcheck 依据）

| 场景 | 命令 | 期望退出码 |
|---|---|---|
| 健康 | 主程序在跑 + `--health-check` | ✅ **0**（实测输出 `OK ... detector=yolov8_detector`） |
| 服务没起 | 不启动主程序直接探测 | ✅ **2**（实测 `不健康: 连不上(服务未启动?) ... code=14`） |
| 超时 / 未授权 / 不健康 | 见 README [T43] 口径 | 3 / 4 / 5 |

想亲手看到 **4（未授权）**：服务端和探针**两边都**带上同一个 token 才通过，故意给错即 4：
```bash
cd build && GRPC_AUTH_TOKEN=s3cr3t ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.ops.yaml &
GRPC_AUTH_TOKEN=wrong  ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.ops.yaml --health-check; echo "EXIT=$?"  # 期望 4
GRPC_AUTH_TOKEN=s3cr3t ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.ops.yaml --health-check; echo "EXIT=$?"  # 期望 0
```
⚠️ 探针**必须与主进程读到同一份配置/环境**（尤其 `GRPC_AUTH_TOKEN`、`CVINFER_CONFIG`），否则会拿"两份环境"误判。

---

## §5 P3 — 接通 MySQL（让落库从"降级"变"正常"）

**二选一**：

**A. docker（更省事，但本机 daemon 没起）**
```bash
sudo service docker start && docker info | head -3
cd /home/jmp/CVInfer-Gate/docker
printf 'MYSQL_ROOT_PASSWORD=12345678jmp\n' > .env      # compose 从这里读口令（.env 不存在，需你创建）
docker compose up -d mysql-db
docker compose logs -f mysql-db      # 等 "ready for connections"
```

**B. 本机装（无需 docker）**
```bash
sudo apt install -y mysql-server
sudo service mysql start
sudo mysql < /home/jmp/CVInfer-Gate/scripts/schema.sql      # 建库 cv_infer + detections/alerts 表（幂等）
sudo mysql -e "ALTER USER 'root'@'localhost' IDENTIFIED BY '12345678jmp'; FLUSH PRIVILEGES;"
```
> `scripts/schema.sql` 与 `docker/init_db.sql` 等价（后者在容器首次初始化时自动执行）。
> 口令可以不同于 `12345678jmp`：`export DB_PASSWORD=<你的口令>` 即可（config 里是 `"${DB_PASSWORD:-12345678jmp}"`）。

**验证（关键：`db_healthy` 从 0 变 1，且不再产生 CSV）**：
```bash
cd build
rm -f db_fallback.csv
./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.ops.yaml &
sleep 12; curl -s http://127.0.0.1:9100/metrics | grep -E 'db_healthy|db_reconnects'
# 期望 cvinfer_db_healthy 1
ls db_fallback.csv 2>/dev/null || echo "没有降级文件 = 落库正常"
mysql -uroot -p12345678jmp -e "use cv_infer; select count(*) from detections; select * from alerts order by id desc limit 3;"
```
**降级/恢复也能亲眼验**：跑着的时候 `sudo service mysql stop` → `db_healthy 0` + 记录转 CSV；`start` 回来 → 后台自动重连（`reconnect_interval_ms: 30000`，或等 100 次冲刷）→ `db_reconnects_total` +1 + CSV 回传清理。

---

## §6 P4 — 接通 Phase C 复核（mock → 真 VLM）

**C-1 mock（确定性、回归用）**：
```bash
cd /home/jmp/CVInfer-Gate
source .venv/bin/activate
python3 scripts/mock_review_server.py --port 50052 --mode auto
# 期望: 监听 0.0.0.0:50052
```
**C-2 真 VLM 服务端（三种后端）**：
```bash
# 上游 VLM 先起在 :8000（vLLM/Ollama/云 API 任选），然后：
python3 -m vlm_review.server --backend openai \
    --base-url http://127.0.0.1:8000/v1 --model Qwen/Qwen2.5-VL-7B-Instruct
# 没有上游也能自测协议：
python3 -m vlm_review.server --backend mock --port 50052
```
**C-3 单独联调复核服务（不启动流水线）**：
```bash
ffmpeg -y -i test.mp4 -frames:v 1 /tmp/frame.jpg          # 造一张测试图
./build/review_client 127.0.0.1:50052 /tmp/frame.jpg "判断该人员是否未佩戴安全帽"
# 期望: [review_client] 服务就绪: backend=mock ... + 结论行
```
**C-4 鉴权（[T41]）零依赖自测**（不需要 grpcio，用假 grpc 模块驱动真逻辑）：
```bash
python3 -m vlm_review.test_auth_interceptor     # ✅ 实测 [OK] 全部通过
# 端到端（需 grpcio）:
VLM_AUTH_TOKEN=s3cr3t python3 -m vlm_review.server --backend mock --port 50052 &
./build/review_client 127.0.0.1:50052 /tmp/frame.jpg                       # 期望退出码 5 (UNAUTHENTICATED)
REVIEW_AUTH_TOKEN=s3cr3t ./build/review_client 127.0.0.1:50052 /tmp/frame.jpg   # 期望 0
```

**C-5 让主程序用上复核**：用 `config.test.yaml`（`review.enabled: true`），跑完看退出账目：
```
复核统计: submitted=N reviewed=N confirmed=? rejected=? timeout=0 unavailable=0   ← unavailable 必须是 0
```
对比 P1 的 `unavailable=3`（那时没有复核服务端）—— 这一项从 N 变 0 就说明 Phase C 真接通了。

---

## §7 P5 — 主服务对外：grpc_client + 浏览器

**gRPC 客户端**：
```bash
cd build && ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.test.yaml &
cd .. && ./build/grpc_client                    # 默认 127.0.0.1:50051, 超时 5000ms
# 期望: 检测成功 / 检测到目标数量: N (...)  退出码 0
```
**鉴权（[T42]）三连**（服务端带 token 时）：
```bash
cd build && GRPC_AUTH_TOKEN=s3cr3t ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.test.yaml &
cd .. && ./build/grpc_client; echo $?                          # 期望 5 (UNAUTHENTICATED)
GRPC_AUTH_TOKEN=wrong ./build/grpc_client; echo $?             # 期望 5
GRPC_AUTH_TOKEN=s3cr3t ./build/grpc_client; echo $?            # 期望 0
```
服务端日志应出现 `[鉴权] 拒绝 ipv4:127.0.0.1:xxxxx 的 Detect`（两次拒绝）；`/metrics` 里 `grpc_requests_total{method="Detect",code="UNAUTHENTICATED"}=2`。

**Web 网关（浏览器演示）**：
```bash
cd /home/jmp/CVInfer-Gate
source .venv/bin/activate
cp web_gateway/env.example web_gateway/.env      # GRPC_SERVER=127.0.0.1:50051, WEB_PORT=8080
cd web_gateway && python3 app.py
# 期望: [web_gateway] HTTP -> 0.0.0.0:8080
```
浏览器打开 `http://localhost:8080`，上传一张有人的图片 → 返回带框结果。
⚠️ 主程序开了 `GRPC_AUTH_TOKEN` 时，网关**也要**同一个变量，否则页面会报未授权。

---

## §8 P6 — Phase B 级联（`config.test.yaml`）

```bash
cd /home/jmp/CVInfer-Gate/build
./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.test.yaml
```
启动日志要出现（**唯一判据**）：
```
使用检测器: cascade (级联模式)      ← 出不来就是"静默退化为单模型"
```
> 退化的常见原因：`cascade.secondary` 与 `model_config.yaml` 里的**模型名不完全一致**（必须是 `helmet_classifier`）；`accept_label` 必须与 `models/helmet_labels.txt` 的类别名对齐（现配 `safety_helmet`）。
退出时看：`级联统计: primary/triggered/confirmed/rejected/skipped`。
`config.test.yaml` 同时开了 `target_fps: 10`，所以这一次 `processed` 会明显大于 P1（不再狂丢帧）。

---

## §9 P7 — RTSP 真流（mediamtx 推 → 程序拉）

顺序别搞反（**这是最容易踩的坑**）：

```bash
# 1) 起 mediamtx（读仓库根的 mediamtx.yml，rtspAddress :8554）
cd /home/jmp/CVInfer-Gate && ./mediamtx &
# 2) 推一路流进去（-c copy 几乎不耗 CPU；路径必须是 /live）
ffmpeg -re -stream_loop -1 -i test.mp4 -c copy -f rtsp rtsp://127.0.0.1:8554/live
# 3) 先验证能拉（比启动整个程序快得多）
ffprobe -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live
# 4) 再跑程序（config.rtsp.yaml 里 source_type/source_path 都已指向 rtsp://127.0.0.1:8554/live）
cd build && timeout 60 ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.rtsp.yaml
```
- ⚠️ **mediamtx 对"当前没有推流"的 path 一律 404** —— 那是"没人推"，不是路径拼错。推流成功后 mediamtx 日志会出现 `is publishing to path ...`。
- ⚠️ RTSP 是实时流：**用 `frame_interval` 抽帧，别用 `target_fps` 限速**（限速会让 TCP 缓冲越堆越多，画面越来越"过去时"）。
- ⚠️ `source_type` 与 `source_path` **两个键都要改**（只改地址、类型留 `file` 会走 `cv::VideoCapture`，UDP、无超时、易卡死）。
- ⚠️ 结果视频不会自动结束（RTSP 无"读完"概念），长期跑 `ls -l build/output.avi` 看磁盘。
- ✅ [T36] 起 `config.rtsp.yaml` 头部注释里写的"**没有断流重连**"**已过时**：现在有 `interrupt_callback` + 指数退避重连（0.5s→8s）。这一处文档待更新。

---

## §10 P8 — 告警 webhook 的三种结局（投递/重试/丢弃）

```bash
# 伪下游：每条延迟 50ms + 前 2 次故意 500 ⇒ 能亲眼看到重试
python3 scripts/alert_receiver.py --port 8899 --token demotoken --delay-ms 50 --fail-first 2
# 另一个终端：把门限调松，保证短跑也能出告警
cd build && ALERT_TOKEN=demotoken ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.ops.yaml
```
- 投递失败看：`告警推送统计: pushed=? sent=? failed=? dropped=? retried=?`（README 记录的死端口实验：`sent=0 failed=3 retried=6`，**流水线照常跑完并出视频**）。
- 队列满看：`cvinfer_alert_push_dropped_total`（`max_queue` 满则丢最旧，与帧队列同策略）。
- 载荷契约（下游集成）：`POST <url>`、`Content-Type: application/json`、`{"source":"cvinfer-gate","alert_type":...,"frame_seq":...,"label":...,"confidence":...,"track_id":...,"ts_ms":...}`；**2xx 才算送达**（4xx/5xx 都会重试）。

---

## §11 P9 — Docker 一键（⚠️ 三个前置）

```bash
sudo service docker start                  # 本机 daemon 没起
cd /home/jmp/CVInfer-Gate/docker
cat > .env <<'EOF'
MYSQL_ROOT_PASSWORD=12345678jmp
# GRPC_AUTH_TOKEN=s3cr3t        # 可选: 主服务鉴权
# VLM_AUTH_TOKEN=s3cr3t         # 可选: 复核服务鉴权
# ALERT_PUSH_URL=http://host.docker.internal:8899/alert
# ALERT_TOKEN=demotoken
EOF
docker compose build
docker compose up -d
docker compose ps
docker compose logs -f cv-infer-gate
```
编排里的服务：`mysql-db`(3306, 首次自动执行 `init_db.sql`) · `cv-infer-gate`(50051，`expose 9100` 不发布) · `vlm-review`(50052) · `web-gateway`(8080) · 网络 `cv-network`。
挂载：`../config`、`../models`、`../test.mp4`、`../output` ⇒ 所以 **docker 跑时 `test.mp4` 放仓库根**（本地跑时按 CWD 解析，`build/` 下用 `../test.mp4`）。
⚠️ **Dockerfile 与本地版本不一致**：`docker/Dockerfile` 拉的是 **OpenVINO 2025.4.0**（`ubuntu:22.04` 基底），而本机/README 是 **2026.4.0**。容器里的表现（尤其 `device=CPU`、`num_threads` 行为）可能与本地实测不同 —— 想要一致的结论，优先用本地二进制跑。
⚠️ Kubernetes/编排用探针：`./CVInfer-Gate --health-check`（0/2/3/4/5）。

---

## §12 跑完全套后的"体检清单"

| 阶段 | 该看到的数字/字样 | 看不到就是这坏了 |
|---|---|---|
| P0 | `129/129`、`52 项通过` | 二进制/环境坏 |
| P1 | `db_healthy 0` + `db_fallback.csv` 有行 | 降级链断 |
| P1 | `alerts_raised 3 / suppressed 86` | 去重没生效 |
| P1 | `sent=3 failed=0` + 伪下游 3 条 200 | webhook 链断 |
| P2 | `EXIT=0`（健康）/`EXIT=2`（没起） | 探针口径或配置不一致 |
| P3 | `db_healthy 1`、无 CSV、`select count(*)` 增长 | MySQL/schema/口令 |
| P4 | `unavailable=0` + `reviewed=N` | 复核服务端没起 / endpoint 不对 |
| P5 | `检测到目标数量: N`、退出码 0；浏览器出带框图 | gRPC 端口/鉴权/图片 |
| P6 | `使用检测器: cascade (级联模式)` | secondary 名字对不上（静默退化） |
| P7 | `[RtspVideoSource] RTSP 流打开成功` + 帧数增长 | 推流没起 / path 不是 `/live` |
| P8 | `retried>0 failed>0` 但视频照常出 | 推送失败被当致命 |

---

## §13 收尾：停服务 / 清现场 / 常见坑

```bash
# 停干净
pkill -TERM -f './CVInfer-Gate --config'; pkill -f alert_receiver.py
pkill -f mock_review_server.py; pkill -f 'vlm_review.server'; pkill -f mediamtx; pkill -f 'ffmpeg.*rtsp'
ss -ltn | grep -E '50051|50052|8899|9100|8554'    # 应为空

# 现场残留（跑完可删，都在 git 忽略之外）
rm -rf build/output.avi build/db_fallback.csv build/logs
ls -1 output/ Testing/ *.log 2>/dev/null    # 根目录还留着 alive_probe.log/ffmpeg.log/mediamtx.log/chat.py 等历史残留
```

**四个最容易踩的坑**（都实测过）：
1. **改配置改错文件**：CMake `POST_BUILD` 会用源码 `config/` 覆盖 `build/config/` ⇒ 永远改**源码**的 `config/*.yaml`，或用 `--config` 指绝对路径。
2. **相对路径按 CWD 解析**：`../test.mp4`、`db_fallback.csv`、`logs/` 都相对**启动目录**（`build/`）——从别处启动就会找不到视频/文件落在别处。
3. **`worker_threads` 必须 ≤ `models[].pool_size`**：不等时借不到引擎的 worker 会白等（`model_config.yaml` 里 `pool_size: 2` 与 `worker_threads: 2` 是配套的）。
4. **`device` 必须显式写 `CPU`**（`model_config.yaml` 已写）：用 `AUTO` 会让 OpenVINO 去枚举设备，在带 NPU 插件的 WSL 环境会段错误（[T33] 记录）。
