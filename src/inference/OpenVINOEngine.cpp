#include "inference/OpenVINOEngine.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

namespace {
// [T35] 引擎分段计时(预处理 / 推理), 每 100 帧打一行均值 —— 用于验证优化效果。
//   推理 = infer_request_.infer() 的墙钟时间(含 OV 线程池内部工作);
//   预处理 = blob 组装(缩放 + 转换 + HWC->CHW)。
std::atomic<std::uint64_t> g_pre_ns{0};
std::atomic<std::uint64_t> g_infer_ns{0};
std::atomic<std::uint64_t> g_calls{0};
}  // namespace

bool OpenVINOEngine::init(const ModelConfig& config) {
    try {
        input_width_ = config.input_width;
        input_height_ = config.input_height;

        // 1. 读取模型
        std::shared_ptr<ov::Model> model = core_.read_model(config.model_xml_path);
        
        // 2. [T30] 编译模型: 设备/性能模式/线程数可配置(默认与改造前一致: AUTO + OpenVINO 默认)
        //    以字符串键名("PERFORMANCE_HINT"/"INFERENCE_NUM_THREADS")而非 ov::hint::* 对象,
        //    以避开不同 OpenVINO 版本间的命名空间差异。
        //    典型调优: worker_threads>1 时用 performance_mode=throughput -> OpenVINO 自动
        //    按“总线程数≈物理核”分配内部 stream, 避免 N 个引擎各自开满核互相抢。
        ov::AnyMap props;
        if (config.perf_mode == "latency") {
            props["PERFORMANCE_HINT"] = std::string("LATENCY");
        } else if (config.perf_mode == "throughput") {
            props["PERFORMANCE_HINT"] = std::string("THROUGHPUT");
        }
        if (config.num_threads > 0) {
            props["INFERENCE_NUM_THREADS"] = config.num_threads;
        }
        const std::string device = config.device.empty() ? std::string("AUTO") : config.device;
        // 空 props 与不传 props 等价(缺省路径行为不变), 故无需分支。
        // 注: 值统一用 std::string/int —— ov::Any 对 const char* 的处理在各版本不一致。
        compiled_model_ = core_.compile_model(model, device, props);
        
        // 3. 创建推理请求
        infer_request_ = compiled_model_.create_infer_request();

        std::cout << "[OpenVINOEngine] 模型加载成功: " << config.model_xml_path << std::endl;
        std::cout << "[OpenVINOEngine] 输入尺寸: " << input_width_ << "x" << input_height_ << std::endl;
        // [T30] 打印实际生效的性能设置(便于确认调优是否生效)
        std::cout << "[OpenVINOEngine] device=" << device
                  << ", performance_mode=" << (config.perf_mode.empty() ? std::string("(default)") : config.perf_mode)
                  << ", num_threads=" << (config.num_threads > 0 ? std::to_string(config.num_threads) : std::string("(default)"))
                  << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[OpenVINOEngine] 初始化失败: " << e.what() << std::endl;
        return false;
    }
}

bool OpenVINOEngine::infer(const cv::Mat& input, std::vector<ov::Tensor>& outputs) {
    if (input.empty()) return false;

    try {
        const auto t_pre0 = std::chrono::steady_clock::now();

        // 1. 预处理：缩放 + 归一化 + BGR转RGB + HWC转CHW
        //   [T35] 快路径: 直接写进 inference request 自己的输入 tensor, 省掉每帧一次
        //   4.9MB blob 分配 + 一次整体拷贝(这一步原本就在 worker 关键路径上)。
        //   仅在“输入是 f32 / NCHW / 与配置尺寸一致 / 帧是 8UC3”时启用;
        //   其它情况(如 u8 输入的量化模型)退回 blobFromImage, 保证不影响其他模型。
        //   数值上与 blobFromImage(swapRB=true, crop=false) 等价 —— 同为
        //   INTER_LINEAR 普通缩放 + 1/255 缩放 + BGR->RGB + HWC->CHW
        //   (仅浮点乘结合顺序不同, 差异 <=1 ULP, 不影响阈值判定)。
        ov::Output<const ov::Node> input_port = compiled_model_.input();
        const ov::Shape in_shape = input_port.get_shape();
        const bool fast_path = (input_port.get_element_type() == ov::element::f32) &&
                               input.type() == CV_8UC3 && in_shape.size() == 4 &&
                               in_shape[0] == 1 && in_shape[1] == 3 &&
                               static_cast<int>(in_shape[2]) == input_height_ &&
                               static_cast<int>(in_shape[3]) == input_width_;

        if (fast_path) {
            // 每线程复用缩放缓冲(避免每帧一次 1.2MB 的 cv::Mat 分配)
            static thread_local cv::Mat resized;
            cv::resize(input, resized, cv::Size(input_width_, input_height_), 0, 0, cv::INTER_LINEAR);

            ov::Tensor in_tensor = infer_request_.get_input_tensor();
            float* dst = in_tensor.data<float>();
            const int w = input_width_, h = input_height_;
            const int area = w * h;
            const float inv = 1.0f / 255.0f;
            for (int y = 0; y < h; ++y) {
                const uchar* src = resized.ptr<uchar>(y);
                float* base = dst + static_cast<std::size_t>(y) * w;
                for (int x = 0; x < w; ++x, src += 3) {
                    base[x]            = src[2] * inv;   // R
                    base[area + x]     = src[1] * inv;   // G
                    base[2 * area + x] = src[0] * inv;   // B
                }
            }
        } else {
            cv::Mat blob;
            cv::dnn::blobFromImage(input, blob, 1.0 / 255.0,
                                   cv::Size(input_width_, input_height_),
                                   cv::Scalar(), true, false);
            ov::Tensor input_tensor(input_port.get_element_type(), input_port.get_shape(), blob.data);
            infer_request_.set_input_tensor(input_tensor);
        }

        const auto t_inf0 = std::chrono::steady_clock::now();

        // 4. 执行推理
        infer_request_.infer();

        const auto t_inf1 = std::chrono::steady_clock::now();

        // [T35] 分段计时: 每 100 帧打一行均值
        {
            const std::uint64_t pre_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t_inf0 - t_pre0).count();
            const std::uint64_t inf_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(t_inf1 - t_inf0).count();
            g_pre_ns.fetch_add(pre_ns, std::memory_order_relaxed);
            g_infer_ns.fetch_add(inf_ns, std::memory_order_relaxed);
            const std::uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n % 100 == 0) {
                const double k = 1e6 * static_cast<double>(n);
                std::printf("[T35] 引擎分段/帧: 预处理=%.2fms  推理=%.2fms  (累计 %llu 帧)\n",
                            static_cast<double>(g_pre_ns.load()) / k,
                            static_cast<double>(g_infer_ns.load()) / k,
                            static_cast<unsigned long long>(n));
                std::fflush(stdout);
            }
        }

        // 5. 获取输出
        size_t output_size = compiled_model_.outputs().size();
        outputs.clear();
        outputs.reserve(output_size);

        for (size_t i = 0; i < output_size; ++i) {
            ov::Tensor output_tensor = infer_request_.get_output_tensor(i);
            outputs.push_back(output_tensor);
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[OpenVINOEngine] 推理失败: " << e.what() << std::endl;
        return false;
    }
}