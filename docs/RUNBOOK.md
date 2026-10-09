# CVInfer-Gate 全流程 Runbook

> **一条命令链跑通完整链路**：file 源 → 推理 → Phase B 级联 → 真 VLM 复核 → Phase D 融合 → 跟踪去重 → **占座规则层** → 落库 / 告警外发 → 可观测 → 优雅退出。
> 标注：[所需] 所需环境/工具　[命令] 命令　[效果] 启动后应看到的效果。
> 统一基线：OpenVINO **2025.4.0**（CI 与 Docker 镜像均以此版本复现；本机/更新的 2026.x 亦验证可用）。实测环境：g++ 15.2 · CMake 4.2.3 · OpenCV 4.10 · gRPC++ 1.51 · MySQL Connector 1.1.12 · Python 3.14。

---

## 0. 环境与工具

| 依赖 | 版本 / 位置 | 用在哪 |
|---|---|---|
| g++ / CMake | C++17 / ≥ 3.16 | 编译 |
| OpenVINO Runtime | **统一 2025.4.0**（Docker/CI 基线；本机亦可 2026.4.0） | CPU 推理 |
| OpenCV 4 / FFmpeg | 4.10 | 解码、画框 |
| gRPC++ / protobuf | 1.51 | 对外接口 |
| yaml-cpp / MySQL Connector/C++ | 1.1.12 | 配置解析 / 落库 |
| MySQL Server | 本机或 docker(3306) | 落库 |
| Python + venv | 3.14（PEP 668：**必须** venv） | 复核服务 / Web / 伪下游 |
| vLLM（可选） | Qwen2.5-VL-7B | 真 VLM 复核 |

仓库自带素材：`models/{yolov8n,library_det,helmet_cls}.{xml,bin}` + `models/*_labels.txt`、`test.mp4`（`helmet_cls` 是仓库里唯一的分类器样例，仅供 Phase B 注册示例，与占座业务无关）。

---

## 1. 构建

[所需] 上表的 C++ 依赖

```bash
cd /home/jmp/CVInfer-Gate
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

[效果] 产出（`build/` 下）：`CVInfer-Gate`、`grpc_client`、`review_client`、`phase_selftest`、`cv_unit_tests`。
冒烟自检（零外部依赖，秒级）：

```bash
ctest --output-on-failure   # 期望 180/180
./phase_selftest            # 期望 52 项通过, 0 项失败（退出码 0）
```

---

## 2. Python 环境

[所需] Python 3 + venv（本机 PEP 668，`pip install` 直装会报错，必须 venv）

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

[所需] 无。以 `config/config.test.yaml` 为基线（file 源 + Phase B/C/D 全开），按需覆盖：

| 段 | 键 | 全流程取值 | 说明 |
|---|---|---|---|
| `video` | `source_type` / `source_path` | `file` / `../test.mp4` | 相对**启动目录**（`build/`）解析 |
| `pipeline` | `worker_threads` | `2` | 必须 ≤ `model_config.yaml` 的 `models[].pool_size` |
| `database` | `password` | `${DB_PASSWORD:-…}` | 口令走环境变量，不落明文 |
| `cascade` | `enabled` | `false` | 占座链路用不到二级分类器（样例注册写法见 `config/model_config.yaml` 注释块）|
| `review` | `enabled` / `endpoint` | `true` / `127.0.0.1:50052` | Phase C，指向 vlm_review |
| `fusion` | `enabled` | `true` | Phase D（stub 雷达，无需硬件） |
| `alert.push` | `enabled` | `true`（可选） | 告警 webhook 外发 |
| `metrics` | `enabled` | `true`（可选） | `/metrics` 端点 |
| `occupancy` | `enabled` | `true` | **占座判定（规则层）**；座位写法 `seats[].rect: [x,y,w,h]` 或 `polygon: [[x,y],…]`。⚠ **zone 别画到“人坐的地方”**：命中 C2（人在使用）就永远不判占座；先用临时多座位网格探针看物品/人到底落在哪 |

⚠ 改**源码**的 `config/*.yaml`，别改 `build/config/`（构建时会被源码覆盖）。

---

## 4. 起依赖

### 4.1 MySQL（落库）

[所需] MySQL Server（本机装或 docker）

```bash
sudo service mysql start
sudo mysql < /home/jmp/CVInfer-Gate/scripts/schema.sql     # 建库 cv_infer + 表（幂等）
export DB_PASSWORD=<你的 root 口令>                        # config 里是 "${DB_PASSWORD:-…}"
```

[效果] 起主程序后 `cvinfer_db_healthy 1`，且不再产生 `build/db_fallback.csv`。

### 4.2 复核服务（Phase C）

**[零依赖 mock（CI / 回归最常用，不需要 VLM）]**

```bash
python3 scripts/mock_review_server.py --port 50052      # 常驻；Ctrl+C 停
```

[验证] ①端口通：`timeout 2 bash -c 'echo > /dev/tcp/127.0.0.1/50052'` 不报 Connection refused；
②主程序启动日志出现 `[GrpcLlmReviewer] 复核服务就绪: backend=mock, model=mock-vlm, mode=confirm`；
只看到 `复核服务暂不可用(...)` 就是没连上（见坑 6）。`--mode confirm|reject|drop|error|auto` 与
`--delay-ms N` 能把「确认才告警 / 否决不告警 / 不可用兜底 / 超时」四条分支都跑出来。

**[真 VLM]**

[所需] vLLM（或任意 OpenAI 兼容端点）+ venv

```bash
# (1) 上游 VLM
vllm serve Qwen/Qwen2.5-VL-7B-Instruct --port 8000

# (2) gRPC 适配层：把 C++ 送来的 ROI 转成 chat 请求发给 VLM
python3 -m vlm_review.server --backend openai \
    --base-url http://127.0.0.1:8000/v1 --model Qwen/Qwen2.5-VL-7B-Instruct --port 50052
```

[效果] 终端打印监听 `0.0.0.0:50052`。
鉴权（可选，两端同一个 token）：服务端 `VLM_AUTH_TOKEN=s3cr3t python3 -m vlm_review.server …`，网关侧 `export VLM_TOKEN=s3cr3t`，并让 `review.auth_token: "${VLM_TOKEN:-}"`。

### 4.3 告警伪下游（可选）

[所需] 仅 Python 标准库

```bash
python3 /home/jmp/CVInfer-Gate/scripts/alert_receiver.py --port 8899 --token demotoken
# 主程序侧对应：ALERT_TOKEN=demotoken
```

---

## 5. 启动主程序

[所需] 上面依赖已就绪（MySQL 在跑、复核服务在跑）

```bash
cd /home/jmp/CVInfer-Gate/build
ALERT_TOKEN=demotoken ./CVInfer-Gate --config /home/jmp/CVInfer-Gate/config/config.test.yaml
```

> 配置用**绝对路径**，避免被 CMake `POST_BUILD` 拷进 `build/config/` 的那份绕晕。

[效果] **启动期逐行核对（实测原文）**：

```
[ConfigParser] 系统配置加载成功: .../config/config.test.yaml
[ConfigParser] 模型配置加载成功: config/model_config.yaml (模型数=2)
[FileVideoSource] 视频源打开成功: ../test.mp4 (720x1280)
[OpenVINOEngine] 输入尺寸: 640x640 ; device=CPU, performance_mode=latency, num_threads=4
[InferenceEnginePool] 初始化完成, 引擎数量: 2
[ModelPoolManager] 初始化完成, 模型数: 2
使用检测器: cascade (级联模式)          ← Phase B 唯一判据；出不来 = 静默退化为单模型
目标跟踪: 已启用 (iou=0.3, max_age=1000ms)
告警去重: 已启用 (iou=0.3, cooldown=5000ms)
占座判定: 已启用 (座位数=1, 物品类别=[book, bag, laptop], t_occupied=2s, t_grace=5s, vote=7/10)
占座判定: 标签 [book, bag, laptop] 的逐物体送审已抑制, 改由占座事件产生告警   ← 开了占座就不会再报“书本告警”
gRPC 服务已启动, 监听: 0.0.0.0:50051
[指标] /metrics 端点已启动, 端口 9100
服务就绪, 按 Ctrl+C 退出。
引擎分段/帧: 预处理=3.55ms  推理=124.65ms  (累计 100 帧)    ← 每 100 帧一次，看 FPS 靠它
```

- 复核连通时**不**出现 `[GrpcLlmReviewer] 复核服务暂不可用(...)`；出现即没连上 50052。
- ⚠ `decoded` 远大于 `processed` 属**正常**（file 源 30fps、单帧推理 ~125ms，队列 `drop_oldest` 丢帧不积压）。想让每帧都处理，用 `target_fps: 10`。

---

## 6. 启动后的效果 / 对外面

| 看什么 | [命令] 命令 | [效果] 期望 |
|---|---|---|
| 指标 | `curl -s http://127.0.0.1:9100/metrics \| head -40` | 20 组 `cvinfer_*` |
| 健康探针 | `./CVInfer-Gate --config <cfg> --health-check` | `[health-check] OK addr=127.0.0.1:50051 version=1.0.0 … detector=…`，退出码 `0` |
| gRPC | `./grpc_client` | `检测到目标数量: N`，退出码 `0` |
| Web | `bash web_gateway/run.sh`（一键：自动用/建 `.venv`、缺依赖自动装、缺桩文件自动生成）<br>或手动 `cd web_gateway && source ../.venv/bin/activate && python app.py` | 浏览器 `localhost:8080` 上下两块：① **单图检测** —— 拖拽/选择图片 → 无刷新出带框图 + 检测列表；右上角 C++ 服务在线徽标；点击列表行高亮对应框、点击结果图放大（Esc 关闭）、一键下载结果图；② **③ 流水线结果** —— 在线播带框视频（首次自动转码）+ 占座/告警事件时间线 + 产物文件清单 |
| 复核直连 | `./review_client 127.0.0.1:50052 /tmp/frame.jpg "判断该座位是否被长期占座"` | 打印 backend + 结论行 |

**检测结果到底去哪看？（按“直不直观”排序）**

| 产物 | 位置 / 怎么看 | 直观程度 |
|---|---|---|
| **带框结果视频** | `build/output.avi`（C++ 写出的 MJPEG）—— 可**直接在网页看**：`localhost:8080` → 「③ 流水线结果」；后端按需用 ffmpeg 转 H.264 mp4（100MB 约 6s）并缓存，支持拖进度条；也可点「下载原始 AVI」本地播 | 🟢 最直观 |
| **占座/告警事件** | 控制台实时输出 + `build/logs/cvinfer.log`；网页「占座/告警事件」把它拉成时间线（`GET /api/events`），并把座位/物品/已持续时长/投票拆成结构化字段 | 🟢 可直接读 |
| **事件在画面哪里** | 网页「画面示意 · 座位区」：底图 = 结果视频首帧，蓝框 = 配置里的 `occupancy.seats`（`GET /api/scene` + `/api/scene-frame`）；点事件行即高亮对应座位区（A-12 到底是画面哪一块，一眼就知道） | 🟢 一眼定位 |
| **单张图片实时推理** | 网页上半部分（`POST /api/detect`，走 gRPC）—— 上传即出带框图 + 列表。⚠ 它和视频流水线是**两条独立链路**（这条不读 `output.avi`） | 🟢 立即可见 |
| **结构化检测记录** | MySQL `cv_infer.detections` / `alerts`（需写 SQL）；库不可用时降级到 `build/db_fallback.csv` | 🟡 要查库 |
| 指标 / 探针 | `/metrics`、`--health-check` | 🟡 给监控/编排用，不是给人看结果的 |

> ⚠ **网页事件时间线老是空的？** 九成是日志配置没生效。日志相关键全在 **`app` 段**
> （`app.log_level` / `app.log_file` / `app.log_max_size_mb` / `app.log_keep_files`），
> **不是**顶层的 `log:` 段 —— ConfigParser 只读 `app.*`；写成顶层 `log: {level, file}`
> 是**静默失效**的（文件不生成，也不报错，很容易查半天）。
> 自检：启动日志里应该有这行 ——
> `[Logger] 日志轮转已开启: logs/cvinfer.log (...)`；没有就说明 `log_file` 根本没被读到。
> `log_file` 相对 **CWD**（从 `build/` 启动 ⇒ `build/logs/cvinfer.log`，正好是网页默认读的位置），
> 也可用环境变量 `RESULT_LOG` 指到别处。

其它产物：`build/logs/cvinfer.log`（若开 + 轮转）、伪下游逐条打印告警；`build/output.web.mp4` 是网页播放用的转码缓存（源视频更新后自动重转，可直接删）。

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

[效果] 关闭时逐项结算（实测原文）：

```
收到信号 15, 开始优雅关闭...
流水线统计: decoded=… dropped=… processed=… emitted=…
级联统计: primary=… triggered=… confirmed=… rejected=… skipped=…
复核统计: submitted=N reviewed=N confirmed=? rejected=? timeout=0 unavailable=0   ← 连通时 unavailable 必须 0
融合统计: frames=… samples=… matched=…
告警去重统计: allowed=… suppressed=…
目标跟踪统计: frames=… active=… longest_dwell=…ms
占座统计: frames=1332 seats=1 occupied_events=2           ← 每次占用只计一次（上升沿）
  [占座] 座位 A-12: 检出物品 [laptop], 已连续 3 秒「有物品且无人使用」, 最近一次检测到人在座位上是 5 秒前   ← 逐座位快照（运维最常问的“现在哪个座位被判占了”）
告警推送统计: pushed=… sent=… failed=… dropped=… retried=…
结果视频已保存至 output.avi
已安全退出。
```

---

## 8. 常见坑（实测）

1. **改配置改错文件**：构建会用源码 `config/` 覆盖 `build/config/` ⇒ 改源码，或 `--config` 指绝对路径。网页「保存到配置」现已**自动把改动同步到 `build/config/` 镜像**（`web_gateway/app.py::_sync_build_config_mirror`），所以商家照旧 `cd build && ./CVInfer-Gate --config config/config.test.yaml` 就能读到新座位。但**手改**仓库根 `config/` 时仍需自己 `cp config/config.test.yaml build/config/`（或重新构建）—— 否则程序读的还是构建时那份旧副本，表现正是「zone 画得再准也 `occupied_events=0`」。
2. **相对路径按 CWD（`build/`）解析**：`../test.mp4`、`db_fallback.csv`、`logs/`。
3. **`worker_threads` 必须 ≤ `models[].pool_size`**，否则 worker 借不到引擎白等。
4. **`device` 必须显式写 `CPU`**：`AUTO` 在带 NPU 插件的 WSL 会段错误。
5. **级联静默退化**：`cascade.secondary` 必须与 `model_config.yaml` 模型名完全一致。
6. **复核全 `unavailable`**：上游 VLM 没起 / `--base-url` 不对 / 两端 token 不一致。
7. **RTSP**：`source_type` 与 `source_path` 两个键都要改；抽帧用 `frame_interval`，别用 `target_fps`。
8. **`python app.py` 报 `ModuleNotFoundError: No module named 'flask'` ⇒ 不是代码坏了**：依赖装在仓库根的 `.venv` 里，你用的是系统 Python。用 `bash web_gateway/run.sh`（一键），或先 `source .venv/bin/activate`。`app.py` 现已把「缺什么 / 用错了解释器 / 怎么装」打成中文提示（退出码 2），不再只丢一句英文堆栈。
9. **网页「流水线结果」显示“还没有结果视频”**：说明还没跑过 C++ 主程序（没产出 `output.avi`），或 `RESULT_DIR` 不是它在写的位置（默认 `<仓库根>/build`，可用环境变量覆盖）。
10. **网页说「本轮未检测到占座」/ 事件列表空 ⇒ 先看 ③ 顶部那块「本轮为什么是这样」，别先猜 zone**：它从 `build/logs/cvinfer.log` 的**最后一轮**原文里抽结论（每条都能展开看原文）。最常见的三条：①「这轮一帧都没处理：视频源打不开」—— 相对路径 `../test.mp4` 只能在 `build/` 下启动（见坑 2）；②「规则层结论：frames=N、occupied_events=0」+ 每行 `[占座] 座位 X: 未检出物品` —— 这是规则层**自己给的原因**（物品类别是**精确相等**匹配：写了检测器不会输出的词，如 `bag`，那个类别就等于不参与判定）；③「座位 [X] 有 N% 露在画面外」。面板还会提示：网页在改的配置**绝对路径**、程序实际加载的配置是不是同一份、复核服务在不在、`告警推送: 已禁用`、以及**仓库根是否另有一份日志**（= 从仓库根启动过）。④「`复核统计: submitted=1 … timeout=1`」⇒ **候选没丢、VLM 也答了，只是答晚了一步被客户端判超时**：`review.timeout_ms` 小于 VLM 单次耗时（ROI 放大 3 倍后实测约 1.9~3.5s，冷启动更久）。先预热一次 VLM 再跑；仍超时就调大 `review.timeout_ms`（现默认 10000ms；`/tmp/vlm_review.out` 里 `[vlm] #N … 1894ms` 那行就是实测耗时）。
    ⚠ 附带修好一条**旧判据从来没生效**的问题：「程序加载的配置与网页在编辑的是同一份」原先只比对**文件名**（`build/config/config.test.yaml` 与 `config/config.test.yaml` 同名 ⇒ 漏判成「同一份」），而且它依赖日志里的 `系统配置加载成功: <路径>` 行 —— 那行是 `ConfigParser.cpp` 用 `std::cout` 打到 **stdout** 的，**根本不进 `cvinfer.log`**，所以面板里查无此条。现在改成：把程序启动目录(`build/`)下的那份展开成**真实路径**、与网页在改的那份**逐字节比对**，并内建 `build/config` 镜像假设作兜底。
