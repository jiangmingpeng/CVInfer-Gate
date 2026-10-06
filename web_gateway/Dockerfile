# ============================================================
# web_gateway (Flask BFF) 镜像
# ------------------------------------------------------------
# 说明: 本镜像把 HTTP(:8080) <-> gRPC(:50051) 的转接层容器化,
#       让 `docker compose up` 真正"一键拉起 MySQL + C++ 网关 + Web 网关"。
#       结果视频/日志走与 cv-infer-gate 共享的卷(见 docker-compose 的 cvinfer-output),
#       故这里只需一个容器内可用的 ffmpeg 做 AVI->mp4 转码即可在线播放。
#
# 构建(仓库根):   docker build -f web_gateway/Dockerfile -t cvinfer-web .
# 或(compose):    docker compose up -d web-gateway
# ============================================================
FROM python:3.11-slim

ENV PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1 \
    WEB_HOST=0.0.0.0 \
    WEB_PORT=8080

# ffmpeg: 结果视频 AVI(MJPEG) -> H.264 mp4 在线播放需要;
# libglib2.0-0: opencv(cv2) 在 slim 上的常见运行期依赖。
RUN apt-get update && apt-get install -y --no-install-recommends \
        ffmpeg libglib2.0-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 1) 先装依赖(利用层缓存: requirements 不变则不重装)
COPY web_gateway/requirements.txt /app/web_gateway/requirements.txt
RUN pip install --no-cache-dir -r /app/web_gateway/requirements.txt

# 2) 再拷代码(gRPC 桩 inference_pb2*.py 已随仓库入库, 无需现场生成)
COPY web_gateway /app/web_gateway

EXPOSE 8080

CMD ["python3", "/app/web_gateway/app.py"]
