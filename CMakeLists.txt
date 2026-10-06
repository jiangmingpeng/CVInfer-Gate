cmake_minimum_required(VERSION 3.16)
project(CVInfer-Gate VERSION 1.0.0 LANGUAGES CXX)

# 1. 设置 C++ 标准与编译选项
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g -Wall -Wno-sign-compare)
else()
    add_compile_options(-O3 -Wall)
endif()

# 2. 查找系统依赖
find_package(OpenCV REQUIRED)
find_package(OpenVINO REQUIRED COMPONENTS Runtime)
find_package(Protobuf REQUIRED)
find_package(yaml-cpp REQUIRED)
# 使用 PkgConfig 查找 gRPC
find_package(PkgConfig REQUIRED)

# ---- gRPC 代码生成: 手动调 protoc / grpc_cpp_plugin 生成桩代码 ----
find_package(Protobuf REQUIRED)
find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

if(NOT PROTOC_EXECUTABLE OR NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "找不到 protoc 或 grpc_cpp_plugin，请检查是否安装了 protobuf-compiler 和 libgrpc++-dev")
endif()

# 2. 指定 proto 文件路径
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/inference.proto)
set(REVIEW_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/review.proto)   # 复核服务契约

# 3. 手动生成 C++ 代码（完全绕过 find_package(gRPC)）
set(GRPC_GENERATED_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/inference.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/inference.grpc.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/review.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/review.grpc.pb.cc
)

add_custom_command(
    OUTPUT ${GRPC_GENERATED_SRCS}
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
         --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
         -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
         ${PROTO_FILE} ${REVIEW_PROTO_FILE}
    DEPENDS ${PROTO_FILE} ${REVIEW_PROTO_FILE}
    COMMENT "正在生成 gRPC C++ 代码..."
)

# 4. 暴露生成的头文件路径
include_directories(${CMAKE_CURRENT_BINARY_DIR})

pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)

# FFmpeg 和 MySQL 也使用 PkgConfig
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
# ==== 查找 MySQL Connector/C++ ====
find_path(MYSQLCPPCONN_INCLUDE_DIR
    NAMES mysql_connection.h jdbc.h
    PATHS /usr/include/mysql-cppconn-8 /usr/include/mysql-cppconn /usr/include
)

find_library(MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn
    PATHS /usr/lib/x86_64-linux-gnu /usr/lib /usr/lib64
)

if(NOT MYSQLCPPCONN_INCLUDE_DIR OR NOT MYSQLCPPCONN_LIBRARY)
    message(FATAL_ERROR "无法找到 MySQL Connector/C++。请确保已安装 libmysqlcppconn-dev。")
endif()

# 3. 收集源文件
file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(${PROJECT_NAME} ${SOURCES} ${GRPC_GENERATED_SRCS})



# gRPC 客户端测试程序
add_executable(grpc_client 
    tests/test_grpc_client.cpp 
    ${GRPC_GENERATED_SRCS} # 复用生成的服务代码
)

target_include_directories(grpc_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR} # 包含生成的 .pb.h
)

target_link_libraries(grpc_client PRIVATE
    ${OpenCV_LIBS}
    protobuf::libprotobuf
    PkgConfig::GRPC
    grpc++ grpc gpr pthread
)

# ============================================================
# 复核服务联调客户端 (Phase C)
# ------------------------------------------------------------
# 不启动流水线, 直接调 review.proto 的 ReviewService, 用于验证
# "真 VLM"复核服务端(或 mock)是否可用。运行: ./review_client <addr> [image]
# ============================================================
add_executable(review_client
    tests/test_review_client.cpp
    ${GRPC_GENERATED_SRCS}   # 复用生成的 review.pb.cc / review.grpc.pb.cc
)

target_include_directories(review_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(review_client PRIVATE
    ${OpenCV_LIBS}
    protobuf::libprotobuf
    PkgConfig::GRPC
    grpc++ grpc gpr pthread
)

# ============================================================
# 阶段自检程序 (Phase A~D)
# ------------------------------------------------------------
# 用"测试内注入的假实现"驱动每个阶段的接缝, 因此**不需要**模型/传感器/复核服务/数据库,
# 也不依赖 config/config.yaml; 也不链接 OpenVINO / gRPC / MySQL, 只编译用到的 5 个 .cpp。
# 运行: ./phase_selftest
# ============================================================
add_executable(phase_selftest
    tests/phase_selftest.cpp
    src/inference/CascadeEngine.cpp          # Phase B
    src/review/ReviewScheduler.cpp           # Phase C
    src/fusion/SensorFusion.cpp              # Phase D
    src/pipeline/MultiSensorPipeline.cpp     # Phase D
    src/sensor/ReplaySensorSource.cpp        # Phase D (stub 传感器)
    src/utils/Logger.cpp                     # 日志
)

target_include_directories(phase_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(phase_selftest PRIVATE
    ${OpenCV_LIBS}
    yaml-cpp
    pthread
)



# 4. 链接所有依赖库
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MYSQLCPPCONN_INCLUDE_DIR}
)

# 版本号注入代码(Health RPC / --health-check / cvinfer_build_info 都用它)
target_compile_definitions(${PROJECT_NAME} PRIVATE CV_GATE_VERSION="${PROJECT_VERSION}")

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    openvino::runtime
    protobuf::libprotobuf
    PkgConfig::GRPC
    PkgConfig::FFMPEG
    ${MYSQLCPPCONN_LIBRARY}
    yaml-cpp
)

# 5. 将配置文件拷贝到构建目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/config $<TARGET_FILE_DIR:${PROJECT_NAME}>/config
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/models $<TARGET_FILE_DIR:${PROJECT_NAME}>/models
)

# 视频文件模式(source_type=file)与诊断工具 grpc_client 默认都读**构建目录**下的 test.mp4
#   (config/*.yaml 的注释里也是这么写的) => 构建时拷一份, 免得"文档说能跑, 实际报读不到视频"。
# copy_if_different: 源文件不改就不会重复拷贝; 源不存在则跳过(不让构建失败)。
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test.mp4)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/test.mp4
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/test.mp4
        COMMENT "拷贝 test.mp4 到构建目录(file 模式 / grpc_client 默认路径)")
endif()

# ============================================================
# 单元测试 (gtest + ctest) —— "立可信度"
# ------------------------------------------------------------
# 覆盖范围: **纯逻辑 / 接缝**, 也就是改动受益面最大、又最容易悄悄改坏的部分:
#   YoloPostProcessor  解码 + 置信度过滤 + NMS + 边界裁剪(不需要模型文件)
#   SensorFusion       时间对齐 + 目标关联 + 加权融合(不需要传感器)
#   ConfigParser       解析 + 校验 + ${VAR:-default} 环境变量展开(不需要外部文件)
#   ThreadSafeQueue    DropOldest/Block/close/统计/超时(纯内存)
#   RoiUtils           外扩 + 裁剪(级联与复核共用)
#   SeatOccupancy     占座判定: 几何归属 + 时序状态机 + 投票(纯逻辑, 不需要模型/视频)
#                     —— 这是"座位算不算被占"这类**业务口径**的回归锁:
#                     它不依赖模型/RTSP/MySQL, 所以能给出真正的确定性断言。
#
# 为什么不测整条流水线: 主干的瓶颈是**外部依赖**(模型/RTSP/MySQL), 单测强依赖
# 它们只会变成摆设; 主干回归交给 phase_selftest + 人工链路验收。
# 依赖: libgtest-dev; 找不到时只警告(CI 用 -DCV_TESTS_REQUIRE_GTEST=ON 强制)。
# 运行: ctest --output-on-failure
# ============================================================
option(CV_BUILD_TESTS "构建单元测试(gtest + ctest)" ON)
option(CV_TESTS_REQUIRE_GTEST "找不到 GTest 时直接报错(CI 用)" OFF)

if(CV_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_test(NAME phase_selftest COMMAND phase_selftest)

    find_package(GTest QUIET)
    if(GTest_FOUND)
        set(CV_GTEST_MAIN GTest::gtest_main)
    elseif(EXISTS "/usr/src/googletest/CMakeLists.txt")
        add_subdirectory(/usr/src/googletest ${CMAKE_BINARY_DIR}/googletest-build EXCLUDE_FROM_ALL)
        set(CV_GTEST_MAIN gtest_main)
    else()
        if(CV_TESTS_REQUIRE_GTEST)
            message(FATAL_ERROR "未找到 GTest, 先 sudo apt install -y libgtest-dev 再 cmake")
        endif()
        message(WARNING "未找到 GTest => 不会生成 cv_unit_tests 目标(故 make cv_unit_tests 报 No rule to make target)")
    endif()

    if(CV_GTEST_MAIN)
        message(STATUS "单元测试已启用: cv_unit_tests + phase_selftest")
        add_executable(cv_unit_tests
            tests/unit/test_thread_safe_queue.cpp
            tests/unit/test_roi_utils.cpp
            tests/unit/test_alert_gate.cpp
            tests/unit/test_target_tracker.cpp
            tests/unit/test_sensor_fusion.cpp
            tests/unit/test_config_parser.cpp
            tests/unit/test_seat_occupancy.cpp
            tests/unit/test_yolo_post_processor.cpp
            tests/unit/test_auth_guard.cpp
            # 运维面: 指标 / 日志轮转 / 告警推送
            tests/unit/test_metrics.cpp
            tests/unit/test_metrics_server.cpp
            tests/unit/test_logger_rotation.cpp
            tests/unit/test_alert_notifier.cpp
            src/fusion/SensorFusion.cpp
            src/utils/ConfigParser.cpp
            src/utils/Logger.cpp           # 轮转
            src/utils/Metrics.cpp
            src/utils/HttpClient.cpp       # 告警推送传输层
            src/service/MetricsServer.cpp  # /metrics 端点
            src/alert/AlertNotifier.cpp    # 告警推送
            src/occupancy/SeatOccupancyAnalyzer.cpp # [占座] 规则判定状态机
            src/inference/YoloPostProcessor.cpp
        )
        target_include_directories(cv_unit_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_BINARY_DIR}
        )
        target_link_libraries(cv_unit_tests PRIVATE
            ${OpenCV_LIBS}
            openvino::runtime
            yaml-cpp
            ${CV_GTEST_MAIN}
            pthread
        )
        include(GoogleTest)
        gtest_discover_tests(cv_unit_tests)
    endif()
endif()