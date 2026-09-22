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
# 使用 PkgConfig 查找 gRPC（修正点）
find_package(PkgConfig REQUIRED)
# # --------------------------------------gRPC 代码生成
# find_package(Protobuf REQUIRED)
# find_package(gRPC REQUIRED)

# # 指定 proto 文件路径
# set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/inference.proto)

# # 生成 pb 和 grpc.pb 文件
# protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILE})

# # 手动生成 gRPC 部分
# add_custom_command(
#     OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/inference.grpc.pb.cc ${CMAKE_CURRENT_BINARY_DIR}/inference.grpc.pb.h
#     COMMAND protoc
#     ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
#          --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
#          -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
#          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
#          ${PROTO_FILE}
#     DEPENDS ${PROTO_FILE} gRPC::grpc_cpp_plugin
# )

# # 把生成的文件加入到源文件列表中
# set(GRPC_GENERATED_SRCS
#     ${CMAKE_CURRENT_BINARY_DIR}/inference.pb.cc
#     ${CMAKE_CURRENT_BINARY_DIR}/inference.grpc.pb.cc
# )

# # 将生成的头文件路径暴露给编译器
# include_directories(${CMAKE_CURRENT_BINARY_DIR})
#----------------------------------------------------这里是后面加的
# 1. 找到 protoc 和 grpc_cpp_plugin 的执行文件路径
find_package(Protobuf REQUIRED)
find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

if(NOT PROTOC_EXECUTABLE OR NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "找不到 protoc 或 grpc_cpp_plugin，请检查是否安装了 protobuf-compiler 和 libgrpc++-dev")
endif()

# 2. 指定 proto 文件路径
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/inference.proto)

# 3. 手动生成 C++ 代码（完全绕过 find_package(gRPC)）
set(GRPC_GENERATED_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/inference.pb.cc
    ${CMAKE_CURRENT_BINARY_DIR}/inference.grpc.pb.cc
)

add_custom_command(
    OUTPUT ${GRPC_GENERATED_SRCS}
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
         --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
         -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
         ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "正在生成 gRPC C++ 代码..."
)

# 4. 暴露头文件路径
include_directories(${CMAKE_CURRENT_BINARY_DIR})

#这里是替换上面的生成代码片段






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
# add_executable(${PROJECT_NAME} ${SOURCES})
add_executable(${PROJECT_NAME} ${SOURCES} ${GRPC_GENERATED_SRCS})



# 编译 gRPC 客户端测试程序------客户端执行测试
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



# 4. 链接所有依赖库
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MYSQLCPPCONN_INCLUDE_DIR}  # 加上这一行
)

# 1.
# target_link_libraries(${PROJECT_NAME} PRIVATE
#     ${OpenCV_LIBS}
#     openvino::runtime
#     protobuf::libprotobuf
#     PkgConfig::GRPC          # 使用 PkgConfig 生成的 gRPC 目标
#     PkgConfig::FFMPEG
#     PkgConfig::MYSQLCPPCONN
# )

# 2.
# target_link_libraries(${PROJECT_NAME} PRIVATE
#     ${OpenCV_LIBS}
#     openvino::runtime
#     protobuf::libprotobuf
#     PkgConfig::GRPC   # 使用 PkgConfig 生成的 gRPC 目标
#     PkgConfig::FFMPEG
#     ${MYSQLCPPCONN_LIBRARY}      # 替换掉原来的 PkgConfig::MYSQLCPPCONN
#     yaml-cpp
# )
# 3.
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OpenCV_LIBS}
    openvino::runtime
    protobuf::libprotobuf
    PkgConfig::GRPC   # 使用 PkgConfig 生成的 gRPC 目标
    PkgConfig::FFMPEG
    ${MYSQLCPPCONN_LIBRARY}      # 替换掉原来的 PkgConfig::MYSQLCPPCONN
    yaml-cpp
    # gRPC::grpc++    #gPRC添加 上面的GRPC完全够用
)

# 5. 将配置文件拷贝到构建目录
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/config $<TARGET_FILE_DIR:${PROJECT_NAME}>/config
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/models $<TARGET_FILE_DIR:${PROJECT_NAME}>/models
)