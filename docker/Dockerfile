FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive


# 1. 安装基础工具和依赖
RUN apt-get update && apt-get install -y \
    build-essential cmake pkg-config git \
    libopencv-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev \
    libmysqlcppconn-dev \
    libyaml-cpp-dev \
    libgtest-dev \
    wget gnupg curl tar \
    && rm -rf /var/lib/apt/lists/*

# 2. 下载并解压 OpenVINO 压缩包
RUN curl -L "https://storage.openvinotoolkit.org/repositories/openvino/packages/2025.4/linux/openvino_toolkit_ubuntu22_2025.4.0.20398.8fdad55727d_x86_64.tgz" \
    --output openvino_2025.4.0.tgz \
    && tar -xf openvino_2025.4.0.tgz \
    && mkdir -p /opt/intel \
    && mv openvino_toolkit_ubuntu22_2025.4.0* /opt/intel/openvino_2025.4.0 \
    && rm openvino_2025.4.0.tgz

# 3. 环境变量
ENV OpenVINO_DIR=/opt/intel/openvino_2025.4.0/runtime/cmake
ENV LD_LIBRARY_PATH=/opt/intel/openvino_2025.4.0/runtime/lib/intel64:$LD_LIBRARY_PATH

# 4. 设置工作目录并拷贝项目
WORKDIR /app
COPY . .

# 5. 编译项目（先删掉可能存在的旧 build 目录，再编译）
RUN rm -rf build && mkdir -p build && cd build \
    && cmake -DOpenVINO_DIR=/opt/intel/openvino_2025.4.0/runtime/cmake .. \
    && make -j$(nproc)



# 6. 默认配置路径(指向**运行时挂载**的 /app/config, 见 docker-compose)。
#    这样改宿主机 config/*.yaml 无需重建镜像; 且 --health-check 探针与主进程
#    共用同一套 CVINFER_CONFIG 解析, 不会出现"探针与服务端两份环境对不上"。
#    (models/ 已由 CMake POST_BUILD 拷进 /app/build/models, 按 CWD 相对路径可用。)
ENV CVINFER_CONFIG=/app/config/config.docker.yaml
ENV CVINFER_MODEL_CONFIG=/app/config/model_config.yaml

# 7. 暴露 gRPC 端口
EXPOSE 50051

# 8. 启动服务(CWD=/app/build; 配置由上面的 CVINFER_CONFIG 决定)
WORKDIR /app/build
CMD ["./CVInfer-Gate"]