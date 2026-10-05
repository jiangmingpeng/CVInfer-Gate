#!/usr/bin/env bash
# ============================================================
# web_gateway 一键启动（Flask BFF）
# ------------------------------------------------------------
# 用法：   bash web_gateway/run.sh
# 停止：   Ctrl+C
#
# 它做四件事（都是幂等的，重复跑不会出问题）：
#   1) 没有 .venv 就建一个
#   2) 依赖没装齐才装（flask / grpcio / opencv / numpy）
#   3) inference_pb2*.py 桩文件缺失就现场生成
#   4) .env 缺失就从 env.example 拷一份
#
# 之所以需要这个脚本：Python 依赖装在仓库根的 .venv 里，
# 直接 `python app.py` 用系统解释器就会报 ModuleNotFoundError。
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
VENV="$ROOT/.venv"

if [[ ! -d "$VENV" ]]; then
  echo "[run.sh] 未找到虚拟环境，创建：$VENV"
  python3 -m venv "$VENV"
fi

PY="$VENV/bin/python"
if [[ ! -x "$PY" ]]; then
  echo "[run.sh] 虚拟环境里没有可用的 python：$PY" >&2
  exit 1
fi

# 依赖自检：只有缺了才装，避免每次启动都跑 pip
if ! "$PY" -c "import flask, grpc, cv2, numpy" >/dev/null 2>&1; then
  echo "[run.sh] 检测到依赖缺失，开始安装（首次会下载 opencv，稍慢）..."
  "$PY" -m pip install --quiet --upgrade pip
  "$PY" -m pip install -r "$HERE/requirements.txt"
fi

# gRPC 桩文件：缺了就按 proto 重新生成
if [[ ! -f "$HERE/inference_pb2.py" || ! -f "$HERE/inference_pb2_grpc.py" ]]; then
  echo "[run.sh] 生成 gRPC 桩文件 ..."
  ( cd "$ROOT" && "$PY" -m grpc_tools.protoc -I proto \
      --python_out=web_gateway --grpc_python_out=web_gateway \
      proto/inference.proto )
fi

# .env：不存在才从模板拷（已存在就尊重你的修改）
if [[ ! -f "$HERE/.env" && -f "$HERE/env.example" ]]; then
  echo "[run.sh] 生成 .env（拷自 env.example，可按需修改）"
  cp "$HERE/env.example" "$HERE/.env"
fi

echo "[run.sh] 启动 Flask（Ctrl+C 停止）"
exec "$PY" "$HERE/app.py"
