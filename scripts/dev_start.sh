#!/usr/bin/env bash
# ============================================================
# 本机开发一键启动（幂等：已经起的不会重复起，可反复跑）
# ------------------------------------------------------------
# 规范流程以 docs/RUNBOOK.md 为准；本脚本只把「这台机器上实际用的
# 那串命令」固化下来，省掉每次翻笔记 / 翻历史命令。
#
#   用法:  bash scripts/dev_start.sh          # 起依赖 + 自检
#          bash scripts/dev_start.sh --stop   # 停掉本脚本起的东西
#
# ⚠ 本脚本只管 Python 侧（复核适配层 + Web 网关）与链路自检；
#   vLLM 与 C++ 主程序只**打印**该执行的命令，不代跑 ——
#   它们分别吃 GPU / 独占终端，代跑反而挡住你看实时输出。
# ============================================================
set -uo pipefail

ROOT="/home/jmp/CVInfer-Gate"
PY="$ROOT/.venv/bin/python"
REVIEW_LOG="/tmp/vlm_review.out"
WEB_LOG="/tmp/web_gateway.out"

VLM_BASE_URL="${VLM_BASE_URL:-http://127.0.0.1:8000/v1}"
VLM_MODEL="${VLM_MODEL:-Qwen/Qwen2-VL-2B-Instruct-AWQ}"

info() { echo "[info] $*"; }
ok()   { echo "[ok]   $*"; }
warn() { echo "[warn] $*"; }
die()  { echo "[err]  $*" >&2; exit 1; }

port_up() { ss -ltn 2>/dev/null | grep -q ":$1 "; }

# ---------- --stop ----------
if [[ "${1:-}" == "--stop" ]]; then
  if pkill -f 'vlm_review.server' 2>/dev/null; then ok "已停复核适配层"; else warn "复核适配层本来就没在跑"; fi
  if pkill -f 'web_gateway/app.py' 2>/dev/null;  then ok "已停 Web 网关";    else warn "Web 网关本来就没在跑"; fi
  warn "vLLM 与 C++ 主程序请自行停（主程序: pkill -TERM -f './CVInfer-Gate --config'）"
  exit 0
fi

# ---------- 0) 前置 ----------
[[ -x "$ROOT/build/CVInfer-Gate" ]] || die "还没构建：cd $ROOT/build && cmake .. && make -j\$(nproc)"
[[ -x "$PY" ]] || die "缺 .venv：cd $ROOT && python3 -m venv .venv && source .venv/bin/activate && pip install -r web_gateway/requirements.txt -r vlm_review/requirements.txt"
ok "可执行文件与 venv 都在位"

# ---------- 1) vLLM（只查不代跑）----------
if port_up 8000; then
  ok "vLLM 已在 :8000"
else
  warn "vLLM 没在跑 —— 请另开一个终端执行:"
  SNAP="$(ls -d /home/jmp/.cache/huggingface/hub/models--Qwen--Qwen2-VL-2B-Instruct-AWQ/snapshots/*/ 2>/dev/null | head -1)"
  if [[ -n "$SNAP" && -x /home/jmp/venv_qwen/bin/vllm ]]; then
    echo "    /home/jmp/venv_qwen/bin/vllm serve ${SNAP%/} \\"
    echo "        --host 0.0.0.0 --port 8000 --quantization awq \\"
    echo "        --gpu-memory-utilization 0.7 --max-model-len 2048 --enforce-eager \\"
    echo "        --served-model-name $VLM_MODEL"
  else
    echo "    vllm serve <模型目录> --host 0.0.0.0 --port 8000 --served-model-name $VLM_MODEL"
  fi
fi

# ---------- 2) 复核适配层（50052）----------
# 关键: 只允许**一个**实例。两个实例能同时 bind 同一端口(SO_REUSEPORT),
# 请求随机落到其中之一, 表现为「同一张图一会儿确认、一会儿不确认」。
N_ADAPTER="$(pgrep -fc 'vlm_review.server' || true)"
if [[ "${N_ADAPTER:-0}" -gt 1 ]]; then
  warn "发现 $N_ADAPTER 个复核适配层实例，先收敛为一个 ..."
  pkill -f 'vlm_review.server' 2>/dev/null; sleep 1.5; N_ADAPTER=0
fi
if [[ "${N_ADAPTER:-0}" -eq 1 ]]; then
  ok "复核适配层已在 :50052"
else
  info "启动复核适配层 -> $REVIEW_LOG"
  ( cd "$ROOT" && nohup "$PY" -m vlm_review.server --backend openai \
      --base-url "$VLM_BASE_URL" --model "$VLM_MODEL" --port 50052 > "$REVIEW_LOG" 2>&1 & )
  sleep 5
  port_up 50052 || { sed -n '1,20p' "$REVIEW_LOG"; die "复核适配层没起来（日志见上）"; }
  if grep -q 'scenario=seat_occupancy' "$REVIEW_LOG"; then
    ok "复核适配层就绪（场景=seat_occupancy）"
  else
    warn "复核适配层起了，但场景不是 seat_occupancy —— 去看 $REVIEW_LOG"
  fi
fi

# ---------- 3) Web 网关（8080）----------
if port_up 8080; then
  ok "Web 网关已在 :8080"
else
  info "启动 Web 网关 -> $WEB_LOG"
  ( cd "$ROOT" && nohup bash web_gateway/run.sh > "$WEB_LOG" 2>&1 & )
  sleep 6
  port_up 8080 || { sed -n '1,20p' "$WEB_LOG"; die "Web 网关没起来（日志见上）"; }
  ok "Web 网关就绪: http://localhost:8080"
fi

# ---------- 4) 链路自检（只读本机，不触发任何付费/外部）----------
if port_up 8000 && port_up 50052; then
  if [[ -f /tmp/warm.jpg ]] || ffmpeg -y -loglevel error -i "$ROOT/test.mp4" -vf 'select=eq(n\,300)' -vframes 1 /tmp/warm.jpg 2>/dev/null; then
    info "预热 + 自检复核链路（review_client -> 适配层 -> vLLM）..."
    ( cd "$ROOT/build" && timeout 90 ./review_client 127.0.0.1:50052 /tmp/warm.jpg \
        "判断该座位是否被长期占座（人离开但物品仍在）" 2>&1 | tail -8 )
  fi
fi

# ---------- 5) 剩下要你亲手跑的 ----------
echo
echo "============================================================"
echo " 上面都 [ok] 后，主程序这样起："
echo "------------------------------------------------------------"
echo "   cd $ROOT/build"
echo "   ./CVInfer-Gate --config $ROOT/config/config.test.yaml"
echo
echo " 停止:   pkill -TERM -f './CVInfer-Gate --config'"
echo " 看结果: http://localhost:8080  -> 「③ 流水线结果」"
echo "============================================================"
