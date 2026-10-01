# vlm_review —— Phase C 真 VLM 复核服务端

把「大模型异步复核」从 **规则 mock** 升级为 **真实 VLM**。
它实现 `proto/review.proto` 的 `review.ReviewService`，把 C++ 网关送来的
ROI 小图交给 VLM 判定「是否未佩戴安全帽」，并返回
`confirmed / label / confidence / reason`。

> **契约不变是刻意的**：C++ 侧 `GrpcLlmReviewer` 与 `VideoPipeline` 零改动，
> 只要把 `config.yaml` 的 `review.endpoint` 指到本服务即可。

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
    --port 50052
```

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
退出码：`0=Ok  1=Failed  2=不可达  3=超时  4=入参错误`。

接入流水线：把 `config.yaml` 的 `review.endpoint` 设为 `127.0.0.1:50052`，
`review.enabled: true`，其余保持默认。可参考 `config/config.test.yaml`。

---

## 4. 环境变量（全部可选，CLI 可覆盖）

| 变量 | 默认 | 说明 |
|---|---|---|
| `VLM_BACKEND` | `openai` | `openai` / `transformers` / `mock` |
| `VLM_MODEL` | 后端默认 | 模型名或本地权重路径 |
| `VLM_BASE_URL` | `http://127.0.0.1:8000/v1` | openai 后端上游地址 |
| `VLM_API_KEY` | `EMPTY` | openai 后端鉴权 |
| `VLM_REQUEST_TIMEOUT_S` | `30` | 单次上游 HTTP 超时 |
| `VLM_MAX_TOKENS` | `160` | 生成长度 |
| `VLM_TEMPERATURE` | `0.0` | 建议 0，保证可复现 |
| `VLM_DEVICE` / `VLM_DTYPE` | `auto` | transformers 后端 |
| `VLM_PROMPT` | `判断该人员是否未佩戴安全帽` | 默认业务提示 |
| `VLM_HOST` / `VLM_PORT` | `0.0.0.0` / `50052` | 监听 |
| `VLM_MAX_WORKERS` | `4` | gRPC 线程池（并发复核数） |
| `VLM_EXTRA_HEADERS` | 空 | `K: V,K2: V2`，附加到上游请求头 |

---

## 5. 设计要点

- **契约不变 / 后端可插拔**：gRPC 层与 VLM 解耦，三类后端（OpenAI 兼容 /
  本地 transformers / mock）实现同一 `VlmBackend` 接口。
- **输出解析分级容错**：VLM 输出格式最容易漂移，`prompts.parse_verdict`
  依次尝试 `JSON → 中英关键词 → 兜底不确认`，解析失败宁可**不告警**（避免误杀）。
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
| 全部 `unavailable=N` | 上游 VLM 没起或 `--base-url` 不对（先 `curl $VLM_BASE_URL/models`） |
| 全部 `timeout=N` | VLM 首次加载/首包慢：调大 C++ `review.timeout_ms` 与 `VLM_REQUEST_TIMEOUT_S` |
| 结论总是 `false` | 模型没按 JSON 输出 → 看服务端日志里的 reason；可换更大模型或收紧提示词 |
| `解析失败` 频发 | 同上；`prompts._POSITIVE_PHRASES` 可扩充领域关键词表 |
