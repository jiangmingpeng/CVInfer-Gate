// 1.
// #include <iostream>
// #include <opencv2/opencv.hpp>
// #include <openvino/openvino.hpp>
// #include <mysql_connection.h>  // C++ Connector 头文件
// #include <cppconn/driver.h>

// extern "C" {
// #include <libavformat/avformat.h>
// }

// int main(int argc, char** argv) {
//     std::cout << "=== CVInfer-Gate 项目启动 ===" << std::endl;

//     // 1. 测试 OpenCV
//     std::cout << "[OK] OpenCV 版本: " << CV_VERSION << std::endl;

//     // 2. 测试 OpenVINO
//     ov::Version ov_ver = ov::get_openvino_version();
//     std::cout << "[OK] OpenVINO 版本: " << ov_ver.buildNumber << std::endl;

//     // 3. 测试 FFmpeg
//     std::cout << "[OK] FFmpeg 版本: " << av_version_info() << std::endl;

//     // 4. 测试 MySQL Connector/C++
//     try {
//         sql::Driver* driver = get_driver_instance();
//         std::cout << "[OK] MySQL Connector/C++ 初始化成功" << std::endl;
//     } catch (const std::exception& e) {
//         std::cerr << "[ERROR] MySQL 测试失败: " << e.what() << std::endl;
//         return -1;
//     }

//     std::cout << "=== 所有核心依赖链接成功，环境测试通过！ ===" << std::endl;
//     return 0;
// }

// 2.
// #include <iostream>
// #include "utils/ConfigParser.h"

// int main(int argc, char** argv) {
//     std::cout << "=== CVInfer-Gate 项目启动 ===" << std::endl;

//     ConfigParser config_parser;

//     // 注意：这里的路径是相对于可执行文件运行时的路径
//     // 我们在 CMake 里配置了将 config 拷贝到构建目录
//     if (!config_parser.loadAppConfig("config/config.yaml")) {
//         std::cerr << "系统配置加载失败，程序退出！" << std::endl;
//         return -1;
//     }

//     if (!config_parser.loadModelConfig("config/model_config.yaml")) {
//         std::cerr << "模型配置加载失败，程序退出！" << std::endl;
//         return -1;
//     }

//     // 打印读取到的配置进行验证
//     const auto& app_cfg = config_parser.getAppConfig();
//     const auto& model_cfg = config_parser.getModelConfig();

//     std::cout << "\n--- 系统配置 ---" << std::endl;
//     std::cout << "视频源类型: " << app_cfg.video_source_type << std::endl;
//     std::cout << "视频源路径: " << app_cfg.video_source_path << std::endl;
//     std::cout << "数据库地址: " << app_cfg.db_host << std::endl;
//     std::cout << "gRPC 端口: " << app_cfg.grpc_port << std::endl;

//     std::cout << "\n--- 模型配置 ---" << std::endl;
//     std::cout << "模型 XML: " << model_cfg.model_xml_path << std::endl;
//     std::cout << "输入尺寸: " << model_cfg.input_width << "x" << model_cfg.input_height << std::endl;
//     std::cout << "置信度阈值: " << model_cfg.conf_threshold << std::endl;

//     std::cout << "\n=== 配置模块测试通过 ===" << std::endl;
//     return 0;
// }


// 3.

// #include <iostream>
// #include <chrono>
// #include "utils/ConfigParser.h"
// #include "video/FileVideoSource.h"

// int main(int argc, char** argv) {
//     std::cout << "=== CVInfer-Gate 项目启动 ===" << std::endl;

//     ConfigParser config_parser;
//     if (!config_parser.loadAppConfig("config/config.yaml")) return -1;
//     if (!config_parser.loadModelConfig("config/model_config.yaml")) return -1;

//     const auto& app_cfg = config_parser.getAppConfig();

//     // 测试视频源
//     FileVideoSource video_source;
//     if (!video_source.open(app_cfg.video_source_path)) {
//         std::cerr << "视频源打开失败，请检查 config.yaml 中的路径和文件是否存在！" << std::endl;
//         return -1;
//     }

//     cv::Mat frame;
//     int frame_count = 0;
//     auto start_time = std::chrono::high_resolution_clock::now();

//     std::cout << "开始读取视频帧..." << std::endl;
//     while (video_source.read(frame)) {
//         frame_count++;
//         // 为了测试速度，不显示图像，只计算帧率
//         // cv::imshow("Test", frame);
//         // cv::waitKey(1);
//     }

//     auto end_time = std::chrono::high_resolution_clock::now();
//     std::chrono::duration<double> elapsed = end_time - start_time;
    
//     std::cout << "读取完毕。共读取 " << frame_count << " 帧。" << std::endl;
//     std::cout << "耗时: " << elapsed.count() << " 秒" << std::endl;
//     std::cout << "解码帧率: " << frame_count / elapsed.count() << " FPS" << std::endl;

//     video_source.close();
//     return 0;
// }


// 4.
// #include <iostream>
// #include <chrono>
// #include "utils/ConfigParser.h"
// #include "video/FileVideoSource.h"
// #include "inference/OpenVINOEngine.h"

// int main(int argc, char** argv) {
//     std::cout << "=== CVInfer-Gate 项目启动 ===" << std::endl;

//     ConfigParser config_parser;
//     if (!config_parser.loadAppConfig("config/config.yaml")) return -1;
//     if (!config_parser.loadModelConfig("config/model_config.yaml")) return -1;

//     const auto& app_cfg = config_parser.getAppConfig();
//     const auto& model_cfg = config_parser.getModelConfig();

//     // 1. 初始化视频源
//     FileVideoSource video_source;
//     if (!video_source.open(app_cfg.video_source_path)) return -1;

//     // 2. 初始化推理引擎
//     OpenVINOEngine inference_engine;
//     if (!inference_engine.init(model_cfg)) return -1;

//     // 3. 读取一帧进行推理测试
//     cv::Mat frame;
//     if (!video_source.read(frame)) {
//         std::cerr << "读取视频帧失败！" << std::endl;
//         return -1;
//     }
//     std::cout << "成功读取一帧，尺寸: " << frame.cols << "x" << frame.rows << std::endl;

//     // 4. 执行推理
//     std::vector<ov::Tensor> outputs;
//     auto start_time = std::chrono::high_resolution_clock::now();
    
//     if (!inference_engine.infer(frame, outputs)) {
//         std::cerr << "推理执行失败！" << std::endl;
//         return -1;
//     }

//     auto end_time = std::chrono::high_resolution_clock::now();
//     std::chrono::duration<double, std::milli> elapsed = end_time - start_time;

//     // 5. 打印输出张量信息
//     std::cout << "\n--- 推理结果 ---" << std::endl;
//     std::cout << "推理耗时: " << elapsed.count() << " ms" << std::endl;
//     std::cout << "输出张量数量: " << outputs.size() << std::endl;
    
//     for (size_t i = 0; i < outputs.size(); ++i) {
//         auto shape = outputs[i].get_shape();
//         std::cout << "输出[" << i << "] 形状: [";
//         for (size_t j = 0; j < shape.size(); ++j) {
//             std::cout << shape[j] << (j == shape.size() - 1 ? "" : ", ");
//         }
//         std::cout << "]" << std::endl;
//     }

//     video_source.close();
//     std::cout << "\n=== 推理引擎模块测试通过 ===" << std::endl;
//     return 0;
// }

// 5.
// #include <iostream>
// #include <fstream>
// #include <chrono>
// #include "utils/ConfigParser.h"
// #include "video/FileVideoSource.h"
// #include "inference/OpenVINOEngine.h"
// #include "inference/YoloPostProcessor.h"

// // 读取标签文件
// std::vector<std::string> loadLabels(const std::string& path) {
//     std::vector<std::string> labels;
//     std::ifstream infile(path);
//     std::string line;
//     while (std::getline(infile, line)) {
//         if (!line.empty()) labels.push_back(line);
//     }
//     return labels;
// }

// int main(int argc, char** argv) {
//     std::cout << "=== CVInfer-Gate 项目启动 ===" << std::endl;

//     ConfigParser config_parser;
//     if (!config_parser.loadAppConfig("config/config.yaml")) return -1;
//     if (!config_parser.loadModelConfig("config/model_config.yaml")) return -1;

//     const auto& app_cfg = config_parser.getAppConfig();
//     const auto& model_cfg = config_parser.getModelConfig();

//     // 加载标签
//     std::vector<std::string> labels = loadLabels(model_cfg.labels_path);
//     std::cout << "加载类别数量: " << labels.size() << std::endl;

//     // 初始化视频源和推理引擎
//     FileVideoSource video_source;
//     if (!video_source.open(app_cfg.video_source_path)) return -1;

//     OpenVINOEngine inference_engine;
//     if (!inference_engine.init(model_cfg)) return -1;

//     // 初始化后处理器
//     YoloPostProcessor post_processor(model_cfg.conf_threshold, model_cfg.nms_threshold);

//     // 读取一帧
//     cv::Mat frame;
//     if (!video_source.read(frame)) {
//         std::cerr << "读取视频帧失败！" << std::endl;
//         return -1;
//     }

//     // 执行推理
//     std::vector<ov::Tensor> outputs;
//     if (!inference_engine.infer(frame, outputs)) return -1;

//     // 后处理
//     auto start_time = std::chrono::high_resolution_clock::now();
//     std::vector<DetectionResult> detections = post_processor.process(outputs[0], frame.size(), labels);
//     auto end_time = std::chrono::high_resolution_clock::now();
//     std::chrono::duration<double, std::milli> elapsed = end_time - start_time;

//     std::cout << "后处理耗时: " << elapsed.count() << " ms" << std::endl;
//     std::cout << "最终检测到 " << detections.size() << " 个目标" << std::endl;

//     // 在图像上画框
//     for (const auto& det : detections) {
//         cv::rectangle(frame, det.box, cv::Scalar(0, 255, 0), 2);
//         std::string text = det.label + " " + std::to_string(static_cast<int>(det.confidence * 100)) + "%";
//         cv::putText(frame, text, cv::Point(det.box.x, det.box.y - 5),
//                     cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
//         std::cout << "  - " << text << " at " << det.box << std::endl;
//     }

//     // 保存结果
//     cv::imwrite("output.jpg", frame);
//     std::cout << "结果已保存至 output.jpg" << std::endl;

//     video_source.close();
//     return 0;
// }

// 6.多帧视频处理
// #include <iostream>
// #include <fstream>
// #include <chrono>
// #include <thread>
// #include "utils/ConfigParser.h"
// #include "utils/ThreadSafeQueue.h"
// #include "video/FileVideoSource.h"
// #include "inference/OpenVINOEngine.h"
// #include "inference/YoloPostProcessor.h"

// // 读取标签文件
// std::vector<std::string> loadLabels(const std::string& path) {
//     std::vector<std::string> labels;
//     std::ifstream infile(path);
//     std::string line;
//     while (std::getline(infile, line)) {
//         if (!line.empty()) labels.push_back(line);
//     }
//     return labels;
// }

// int main(int argc, char** argv) {
//     std::cout << "=== CVInfer-Gate 项目启动（多帧流水线） ===" << std::endl;

//     // 1. 加载配置
//     ConfigParser config_parser;
//     if (!config_parser.loadAppConfig("config/config.yaml")) return -1;
//     if (!config_parser.loadModelConfig("config/model_config.yaml")) return -1;

//     const auto& app_cfg = config_parser.getAppConfig();
//     const auto& model_cfg = config_parser.getModelConfig();
//     std::vector<std::string> labels = loadLabels(model_cfg.labels_path);

//     // 2. 初始化视频源以获取元数据（宽高、FPS）
//     FileVideoSource video_source;
//     if (!video_source.open(app_cfg.video_source_path)) return -1;

//     // 获取原视频 FPS（为了输出视频不加速/减速，直接用cv::VideoCapture偷看一眼元数据）
//     cv::VideoCapture meta_cap(app_cfg.video_source_path);
//     double fps = meta_cap.get(cv::CAP_PROP_FPS);
//     if (fps <= 0 || fps > 120) fps = 30.0; // 默认 30 FPS
//     meta_cap.release();

//     // 3. 初始化推理引擎和后处理器
//     OpenVINOEngine inference_engine;
//     if (!inference_engine.init(model_cfg)) return -1;
//     YoloPostProcessor post_processor(model_cfg.conf_threshold, model_cfg.nms_threshold);

//     // // 4. 初始化视频写入器（用于保存带框的结果视频）
//     // cv::VideoWriter video_writer("output.mp4", 
//     //                              cv::VideoWriter::fourcc('m', 'p', '4', 'v'), 
//     //                              fps, 
//     //                              cv::Size(1280, 720)); // 注意：这里硬编码了1280x720，若视频尺寸变动需调整
//     // if (!video_writer.isOpened()) {
//     //     std::cerr << "[Error] 无法初始化 VideoWriter！" << std::endl;
//     //     return -1;
//     // }
//     // 4. 初始化视频写入器（动态获取视频尺寸，防止花屏）
//     int video_width = video_source.getWidth();
//     int video_height = video_source.getHeight();
//     std::cout << "原视频分辨率: " << video_width << "x" << video_height << std::endl;

//     // 优先尝试 H.264 (avc1)，兼容性最好；如果失败，退回到 mp4v
//     int fourcc = cv::VideoWriter::fourcc('a', 'v', 'c', '1');
//     cv::VideoWriter video_writer("output.mp4", fourcc, fps, cv::Size(video_width, video_height));
    
//     if (!video_writer.isOpened()) {
//         std::cout << "[Warning] avc1 编码器不可用，尝试使用 mp4v..." << std::endl;
//         fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
//         video_writer.open("output.mp4", fourcc, fps, cv::Size(video_width, video_height));
//     }

//     if (!video_writer.isOpened()) {
//         std::cerr << "[Error] 无法初始化 VideoWriter！请检查 OpenCV 的 FFmpeg 支持。" << std::endl;
//         return -1;
//     }

//     // 5. 创建线程安全队列，最大缓存 10 帧（防内存膨胀）
//     ThreadSafeQueue<cv::Mat> frame_queue(10);

//     // 6. 消费者线程：负责推理、后处理、画框、写入视频
//     std::thread consumer_thread([&]() {
//         cv::Mat frame;
//         int frame_count = 0;
//         while (frame_queue.pop(frame)) {
//             std::vector<ov::Tensor> outputs;
//             if (!inference_engine.infer(frame, outputs)) continue;

//             auto detections = post_processor.process(outputs[0], frame.size(), labels);

//             // 画框
//             for (const auto& det : detections) {
//                 cv::rectangle(frame, det.box, cv::Scalar(0, 255, 0), 2);
//                 std::string text = det.label + " " + std::to_string(static_cast<int>(det.confidence * 100)) + "%";
//                 cv::putText(frame, text, cv::Point(det.box.x, det.box.y - 5),
//                             cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
//             }

//             video_writer.write(frame);
//             frame_count++;
            
//             if (frame_count % 30 == 0) {
//                 std::cout << "[消费者] 已处理 " << frame_count << " 帧" << std::endl;
//             }
//         }
//         std::cout << "[消费者] 处理完毕，共处理 " << frame_count << " 帧" << std::endl;
//     });

//     // 7. 生产者线程（主线程）：疯狂读取视频帧塞入队列
//     std::cout << "[生产者] 开始读取视频帧..." << std::endl;
//     cv::Mat frame;
//     int total_frames = 0;
//     auto start_time = std::chrono::high_resolution_clock::now();

//     while (video_source.read(frame)) {
//         frame_queue.push(frame.clone()); // 深拷贝推入队列
//         total_frames++;
//     }

//     // 8. 通知消费者停止，并等待其退出
//     frame_queue.stop();
//     consumer_thread.join();

//     auto end_time = std::chrono::high_resolution_clock::now();
//     std::chrono::duration<double> elapsed = end_time - start_time;
    
//     std::cout << "\n--- 流水线统计 ---" << std::endl;
//     std::cout << "总读取帧数: " << total_frames << std::endl;
//     std::cout << "总耗时: " << elapsed.count() << " 秒" << std::endl;
//     std::cout << "平均吞吐量: " << total_frames / elapsed.count() << " FPS" << std::endl;

//     video_source.close();
//     video_writer.release();
//     std::cout << "结果视频已保存至 output.mp4" << std::endl;

//     return 0;
// }



#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <memory>
#include "utils/ConfigParser.h"
#include "utils/ThreadSafeQueue.h"
#include "video/FileVideoSource.h"
#include "inference/OpenVINOEngine.h"
#include "inference/YoloPostProcessor.h"
#include "database/DBWriter.h"
#include "service/DetectionServiceImpl.h"
#include <grpcpp/grpcpp.h>
#include"video/RtspVideoSource.h"

// 读取标签文件
std::vector<std::string> loadLabels(const std::string& path) {
    std::vector<std::string> labels;
    std::ifstream infile(path);
    std::string line;
    while (std::getline(infile, line)) {
        if (!line.empty()) labels.push_back(line);
    }
    return labels;
}

int main(int argc, char** argv) {
    std::cout << "=== CVInfer-Gate 项目启动（多帧流水线） ===" << std::endl;

    // 0. 加载配置
    ConfigParser config_parser;
    if (!config_parser.loadAppConfig("config/config.yaml")) return -1;
    if (!config_parser.loadModelConfig("config/model_config.yaml")) return -1;

    const auto& app_cfg = config_parser.getAppConfig();
    const auto& model_cfg = config_parser.getModelConfig();
    std::vector<std::string> labels = loadLabels(model_cfg.labels_path);

    DBWriter db_writer;
    if (!db_writer.init(app_cfg)) {
        std::cerr << "数据库初始化失败！请检查 config.yaml 的数据库配置。" << std::endl;
        return -1;
    }

    // 1. 根据配置选择视频源
    std::unique_ptr<IVideoSource> video_source;
    if (app_cfg.video_source_type == "rtsp") {
        video_source = std::make_unique<RtspVideoSource>();
    } else {
        video_source = std::make_unique<FileVideoSource>();
    }
    
    // if (!video_source->open(app_cfg.video_source_path)) return -1;
    bool video_source_ok = video_source->open(app_cfg.video_source_path);
    if (!video_source_ok) {
        std::cerr << "[Warning] 视频源打开失败，将跳过本地视频流水线，但 gRPC 微服务仍会启动！" << std::endl;
    }

    // 2. 初始化视频源以获取元数据（宽高、FPS）
    // FileVideoSource video_source;
    // if (!video_source.open(app_cfg.video_source_path)) return -1;

    // 获取原视频 FPS（为了输出视频不加速/减速，直接用cv::VideoCapture偷看一眼元数据）
    cv::VideoCapture meta_cap(app_cfg.video_source_path);
    double fps = meta_cap.get(cv::CAP_PROP_FPS);
    if (fps <= 0 || fps > 120) fps = 30.0; // 默认 30 FPS
    meta_cap.release();

    // 3. 初始化推理引擎和后处理器
    OpenVINOEngine inference_engine;
    if (!inference_engine.init(model_cfg)) return -1;
    YoloPostProcessor post_processor(model_cfg.conf_threshold, model_cfg.nms_threshold);

    // // 4. 初始化视频写入器（用于保存带框的结果视频）
    // cv::VideoWriter video_writer("output.mp4", 
    //                              cv::VideoWriter::fourcc('m', 'p', '4', 'v'), 
    //                              fps, 
    //                              cv::Size(1280, 720)); // 注意：这里硬编码了1280x720，若视频尺寸变动需调整
    // if (!video_writer.isOpened()) {
    //     std::cerr << "[Error] 无法初始化 VideoWriter！" << std::endl;
    //     return -1;
    // }
    // // 4. 初始化视频写入器（动态获取视频尺寸，防止花屏）
    // int video_width = video_source.getWidth();
    // int video_height = video_source.getHeight();
    // std::cout << "原视频分辨率: " << video_width << "x" << video_height << std::endl;

    // // 优先尝试 H.264 (avc1)，兼容性最好；如果失败，退回到 mp4v
    // int fourcc = cv::VideoWriter::fourcc('a', 'v', 'c', '1');
    // cv::VideoWriter video_writer("output.mp4", fourcc, fps, cv::Size(video_width, video_height));
    
    // if (!video_writer.isOpened()) {
    //     std::cout << "[Warning] avc1 编码器不可用，尝试使用 mp4v..." << std::endl;
    //     fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    //     video_writer.open("output.mp4", fourcc, fps, cv::Size(video_width, video_height));
    // }

    // if (!video_writer.isOpened()) {
    //     std::cerr << "[Error] 无法初始化 VideoWriter！请检查 OpenCV 的 FFmpeg 支持。" << std::endl;
    //     return -1;
    // }
    // 4. 初始化视频写入器（改用兼容性最高的 AVI + MJPG 组合）
    int video_width = video_source->getWidth();
    int video_height = video_source->getHeight();
    std::cout << "原视频分辨率: " << video_width << "x" << video_height << std::endl;

    // 直接使用 AVI 容器 + MJPG 编码器
    cv::VideoWriter video_writer("output.avi", 
                                 cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 
                                 fps, 
                                 cv::Size(video_width, video_height));
    if (!video_writer.isOpened()) 
    {
        std::cerr << "[Error] 无法初始化 VideoWriter！" << std::endl;
        return -1;
    }

    // 5. 创建线程安全队列，最大缓存 10 帧（防内存膨胀）
    ThreadSafeQueue<cv::Mat> frame_queue(10);

    // 6. 消费者线程：负责推理、后处理、画框、写入视频
    std::thread consumer_thread([&]() {
        cv::Mat frame;
        int frame_count = 0;
        while (frame_queue.pop(frame)) {
            std::vector<ov::Tensor> outputs;
            if (!inference_engine.infer(frame, outputs)) continue;

            auto detections = post_processor.process(outputs[0], frame.size(), labels);

            // 画框
            for (const auto& det : detections) {
                cv::rectangle(frame, det.box, cv::Scalar(0, 255, 0), 2);
                std::string text = det.label + " " + std::to_string(static_cast<int>(det.confidence * 100)) + "%";
                cv::putText(frame, text, cv::Point(det.box.x, det.box.y - 5),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);
            }

            video_writer.write(frame);
            // 将检测结果写入数据库
            if (!detections.empty()) {
                db_writer.writeDetections(detections);
                // 例如:如果有人没戴安全帽（或者检测到了 person），记录告警
                for (const auto& det : detections) {
                    if (det.label == "person" && det.confidence > 0.8) {
                        db_writer.writeAlert("安全帽缺失", "检测到未佩戴安全帽的人员，置信度: " + std::to_string(det.confidence));
                    }
                }
            }
            frame_count++;
            
            if (frame_count % 30 == 0) {
                std::cout << "[消费者] 已处理 " << frame_count << " 帧" << std::endl;
            }
        }
        std::cout << "[消费者] 处理完毕，共处理 " << frame_count << " 帧" << std::endl;
    });

    // 7. 生产者线程（主线程）：疯狂读取视频帧塞入队列
    std::cout << "[生产者] 开始读取视频帧..." << std::endl;
    cv::Mat frame;
    int total_frames = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    while (video_source->read(frame)) {
        frame_queue.push(frame.clone()); // 深拷贝推入队列
        total_frames++;
    }

    // 8. 通知消费者停止，并等待其退出
    frame_queue.stop();
    consumer_thread.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    
    std::cout << "\n--- 流水线统计 ---" << std::endl;
    std::cout << "总读取帧数: " << total_frames << std::endl;
    std::cout << "总耗时: " << elapsed.count() << " 秒" << std::endl;
    std::cout << "平均吞吐量: " << total_frames / elapsed.count() << " FPS" << std::endl;

    // 9. 启动 gRPC 服务（放在单独的线程或在此处阻塞，测试先阻塞）
    std::cout << "\n[gRPC] 正在启动微服务，监听端口: " << app_cfg.grpc_port << std::endl;
    std::string server_address("0.0.0.0:" + std::to_string(app_cfg.grpc_port));
    
    DetectionServiceImpl service(inference_engine, post_processor, labels);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "[gRPC] 服务已启动，等待客户端连接..." << std::endl;
    
    // 阻塞等待（按 Ctrl+C 退出）
    server->Wait();


    video_source->close();
    video_writer.release();
    // std::cout << "结果视频已保存至 output.mp4" << std::endl;
    std::cout << "结果视频已保存至 output.avi" << std::endl;

    return 0;
}