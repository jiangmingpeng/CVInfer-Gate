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



# 6. 暴露 gRPC 端口
EXPOSE 50051

# 7. 启动服务
WORKDIR /app/build
CMD ["./CVInfer-Gate"]