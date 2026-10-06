# vlm_review —— Phase C 真 VLM 复核服务端

把「大模型异步复核」从 **规则 mock** 升级为 **真实 VLM**。
它实现 `proto/review.proto` 的 `review.ReviewService`，把 C++ 网关送来的
ROI 小图交给 VLM 判定业务结论，并返回 `confirmed / label / confidence / reason`。

> **契约不变是刻意的**：C++ 侧 `GrpcLlmReviewer` 与 `VideoPipeline` 零改动，
> 只要把 `config.yaml` 的 `review.endpoint` 指到本服务即可。

> **业务不写死**：判定什么由**场景**(`Scenario`)决定 —— 目前内置 `helmet`
> (安全帽) 与 `seat_occupancy` (图书馆占座)。用 `VLM_SCENARIO` / `--scenario` 切换；
> 两端**必须用同一场景**，否则 system prompt 与 C++ 侧 `review.prompt` 会打架。

---

## 1. 快速开始（推荐：OpenAI 兼容后端）

现实中最省事的「接真 VLM」方式是让 VLM 暴露 **OpenAI 兼容**的
`/v1/chat/completions`（vLLM、Ollama、LM Studio、DashScope 兼容模式、OpenAI 均支持）。
本服务只做 gRPC ↔ HTTP 的翻译，**换模型只改环境变量**。

```bash
pip install -r vlm_review/requirements.txt

# 假设上游 VLM 已起在 http://127.0.0.1:8000/v1
python3 -m vlm_review.server \
    --backend openai \
    --base-url http://127.0.0.1:8000/v1 \
    --model Qwen/Qwen2.5-VL-7B-Instruct \
    --scenario seat_occupancy \
    --port 50052
```

### 可选场景

| `--scenario` | 正/负 label | 说明 |
|---|---|---|
| `helmet`(默认) | `no_helmet` / `with_helmet` | 工地安全帽合规 |
| `seat_occupancy`(别名 `seat` / `occupancy`) | `occupied` / `not_occupied` | 图书馆/自习室占座 |

占座场景另有对应配置 `config/config.test.yaml`（开箱即用）：它把
`review.trigger.labels` 设为物品、并启用**场景 ROI**（`roi_context_scale` /
`roi_min_side`）与**停留时长闸门**（`min_dwell_ms`），让送审图里能看到
桌面/座位/周围的人，而不是一本书的特写。

### 上游 VLM 怎么起（任选其一）

```bash
# A) vLLM（推荐，吞吐好）
vllm serve Qwen/Qwen2.5-VL-7B-Instruct --port 8000

# B) Ollama（最省事）
ollama run qwen2.5vl:7b          # 默认监听 11434
# 然后 --base-url http://127.0.0.1:11434/v1 --model qwen2.5vl:7b

# C) LM Studio：打开本地服务，--base-url http://127.0.0.1:1234/v1
# D) 云端：--base-url https://dashscope.aliyuncs.com/compatible-mode/v1 \
#          --api-key "$DASHSCOPE_API_KEY" --model qwen-vl-max
```

---

## 2. 其他后端

```bash
# 本地 transformers（Qwen2.5-VL / Qwen2-VL，需额外依赖，体量大）
pip install -r vlm_review/requirements-local.txt
python3 -m vlm_review.server --backend transformers \
    --model Qwen/Qwen2.5-VL-7B-Instruct        # 也可给本地权重目录

# 确定性 mock（自检/CI；行为与 scripts/mock_review_server.py 一致）
python3 -m vlm_review.server --backend mock
```

---

## 3. 验证（不启动整条流水线）

```bash
# 用仓库自带的联调客户端（与流水线内 GrpcLlmReviewer 调同一份 proto）
./build/review_client 127.0.0.1:50052 frame.jpg "判断该人员是否未佩戴安全帽"
# 不带图片路径时会用合成图冒烟：
./build/review_client 127.0.0.1:50052
```

`review_client` 会先探活（`Health`），再送审一张 ROI 并打印结构化结论。
退出码：`0=Ok  1=Failed  2=不可达  3=超时  4=入参错误  5=UNAUTHENTICATED`（ token 不匹配/未带）。

### 打开鉴权（两端必须一致）

```bash
# 服务端：设了 token 就**每个 RPC 都校验**(含 Health)；不设 = 完全不校验(与旧行为一致)
VLM_AUTH_TOKEN=s3cr3t python3 -m vlm_review.server --backend mock --port 50052

# 客户端(C++ 侧)二选一：
REVIEW_AUTH_TOKEN=s3cr3t ./build/review_client 127.0.0.1:50052      # A. 环境变量(推荐: 不落 shell 历史/ps)
# B. 写进配置 review.auth_token: "${VLM_TOKEN:-}" 再 export VLM_TOKEN=s3cr3t
```

⚠️ 带错 token 时连探活(`Health`)都会失败 —— 这是**刻意与客户端对称**的
(客户端每个 RPC 都带 token)，代价是将来新增 RPC 也不会漏校验。

接入流水线：把 `config.yaml` 的 `review.endpoint` 设为 `127.0.0.1:50052`，
`review.enabled: true`，其余保持默认。可参考 `config/config.test.yaml`。

---

## 4. 环境变量（全部可选，CLI 可覆盖）

| 变量 | 默认 | 说明 |
|---|---|---|
| `VLM_BACKEND` | `openai` | `openai` / `transformers` / `mock` |
| `VLM_MODEL` | 后端默认 | 模型名或本地权重路径 |
| `VLM_BASE_URL` | `http://127.0.0.1:8000/v1` | openai 后端上游地址 |
| `VLM_API_KEY` | `EMPTY` | openai 后端**上游**供应商鉴权 |
| `VLM_AUTH_TOKEN` | 空 | **本服务**对调用方的鉴权(Bearer)；空 = 不校验。CLI: `--auth-token` |
| `VLM_REQUEST_TIMEOUT_S` | `30` | 单次上游 HTTP 超时 |
| `VLM_MAX_TOKENS` | `160` | 生成长度 |
| `VLM_TEMPERATURE` | `0.0` | 建议 0，保证可复现 |
| `VLM_DEVICE` / `VLM_DTYPE` | `auto` | transformers 后端 |
| `VLM_SCENARIO` | `helmet` | 业务场景(`helmet` / `seat_occupancy`); 决定 system prompt + label 词表 |
| `VLM_SYSTEM_PROMPT` | 空 | 非空 = **直接覆盖**场景自带的 system prompt(高级用法) |
| `VLM_PROMPT` | 空 | user 侧任务描述; 空 = 用场景自带的 `default_task` |
| `VLM_HOST` / `VLM_PORT` | `0.0.0.0` / `50052` | 监听 |
| `VLM_MAX_WORKERS` | `4` | gRPC 线程池（并发复核数） |
| `VLM_EXTRA_HEADERS` | 空 | `K: V,K2: V2`，附加到上游请求头 |

---

## 5. 设计要点

- **契约不变 / 后端可插拔**：gRPC 层与 VLM 解耦，三类后端（OpenAI 兼容 /
  本地 transformers / mock）实现同一 `VlmBackend` 接口。
- **场景可配置**：`prompts.Scenario` 把一个业务的 system prompt、正/负 label、
  关键词兜底表绑在一起；换业务只换 `VLM_SCENARIO`，解析逻辑不动。
  这修的是一个真实的坑：system 写死"安全帽"而 C++ 侧 `review.prompt` 写的是
  "占座"时，模型会编一个 `long_time_use` 之类的假 label 且 `confirmed` 恒为 false
  —— 表现为"接了 VLM 却从不告警"。
- **脏 label 归一化**：模型给出的词表外 label 会收敛到场景的正/负 label，
  不会把 `long_time_use` 这类自由发挥写进 DB / 告警描述。
- **输出解析分级容错**：VLM 输出格式最容易漂移，`prompts.parse_verdict`
  依次尝试 `JSON → 中英关键词 → 兜底不确认`，解析失败宁可**不告警**（避免误杀）。
  关键词判定按**最长命中**决定归属，因此 `not occupied` 不会被 `occupied` 误命中。
- **错误语义与客户端对齐**：上游不可达 → `UNAVAILABLE`（触发客户端
  `unavailable` 计数与 `alert_on_failure` 兜底）；其它 → `INTERNAL`（计入 `failed`）。
  这样 C++ 侧「确认才告警 / 不可用兜底」两条分支都能被真实复现。
- **零额外 web 依赖**：只用标准库 `urllib` 调上游；硬依赖仅 `grpcio`。
- **免手工生成 proto**：首次启动自动调 `grpc_tools.protoc` 生成 `review_pb2*.py`。
- **并发安全**：请求编号用锁保护（早期版本用 `itertools.count` 会与计数竞态）。

## 6. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `服务端未实现 Health(视为可达)` | 用了旧 pb2；删掉 `build/pyproto` 让服务端重新生成 |
| 客户端退出码 `5` / `UNAUTHENTICATED` | 两端 token 不一致或客户端没带：服务端 `VLM_AUTH_TOKEN` 必须等于客户端 `REVIEW_AUTH_TOKEN`(即配置里的 `review.auth_token`) |
| 全部 `unavailable=N` | 上游 VLM 没起或 `--base-url` 不对（先 `curl $VLM_BASE_URL/models`） |
| 全部 `timeout=N` | VLM 首次加载/首包慢：调大 C++ `review.timeout_ms` 与 `VLM_REQUEST_TIMEOUT_S` |
| 结论总是 `false` | 模型没按 JSON 输出 → 看服务端日志里的 reason；可换更大模型或收紧提示词 |
| 结论的 label 像 `long_time_use` 这种"编出来的" | **两端场景不一致**：C++ 侧 `review.prompt` 是占座，但服务端还是默认 `helmet`。启动时用 `--scenario seat_occupancy`(或 `VLM_SCENARIO`) |
| 占座复核老判 `not_occupied` | 送审图只有物品特写、看不到座位与人。开场景 ROI：C++ 侧 `review.roi_context_scale` / `roi_min_side`(见 `config/config.test.yaml`) |
| `解析失败` 频发 | 同上；`prompts.Scenario.positive_phrases` / `negative_phrases` 可扩充领域关键词表 |
