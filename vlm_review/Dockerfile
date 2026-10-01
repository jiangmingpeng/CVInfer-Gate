# ============================================================
# Phase C 复核服务端(真 VLM) 镜像
# ------------------------------------------------------------
# 说明: 本镜像**只跑 gRPC↔HTTP 适配层**, 不包含 VLM 权重 —— 上游 VLM
#       (vLLM/Ollama/云 API) 由 VLM_BASE_URL 指向。这样镜像小、构建快,
#       且换模型无需重建镜像。
#
# 构建(仓库根):
#   docker build -f vlm_review/Dockerfile -t cvinfer-vlm-review .
# 运行:
#   docker run --rm -p 50052:50052 \
#       -e VLM_BASE_URL=http://host.docker.internal:8000/v1 \
#       -e VLM_MODEL=Qwen/Qwen2.5-VL-7B-Instruct \
#       cvinfer-vlm-review
# 或走 docker-compose(隔离在 profile 内, 不影响默认 up):
#   docker compose --profile vlm up vlm-review
# ============================================================
FROM python:3.11-slim

ENV PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    VLM_HOST=0.0.0.0 \
    VLM_PORT=50052

WORKDIR /app

# 1) 先装依赖(利用层缓存: 依赖不变则不重装)
COPY vlm_review/requirements.txt /app/vlm_review/requirements.txt
RUN pip install --no-cache-dir -r /app/vlm_review/requirements.txt

# 2) 再拷代码与 proto(proto 供首次自动生成 review_pb2*.py)
COPY vlm_review /app/vlm_review
COPY proto /app/proto

EXPOSE 50052

# 注意: _repo_root() = /app, 因此 proto 位于 /app/proto, 生成物落在 /app/build/pyproto
CMD ["python3", "-m", "vlm_review.server"]
