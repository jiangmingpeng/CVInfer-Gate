#include "inference/OpenVINOEngine.h"
#include <iostream>

bool OpenVINOEngine::init(const ModelConfig& config) {
    try {
        input_width_ = config.input_width;
        input_height_ = config.input_height;

        // 1. 读取模型
        std::shared_ptr<ov::Model> model = core_.read_model(config.model_xml_path);
        
        // 2. 编译模型（使用 AUTO 设备，自动选择 CPU 或 GPU）
        compiled_model_ = core_.compile_model(model, "AUTO");
        
        // 3. 创建推理请求
        infer_request_ = compiled_model_.create_infer_request();

        std::cout << "[OpenVINOEngine] 模型加载成功: " << config.model_xml_path << std::endl;
        std::cout << "[OpenVINOEngine] 输入尺寸: " << input_width_ << "x" << input_height_ << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[OpenVINOEngine] 初始化失败: " << e.what() << std::endl;
        return false;
    }
}

bool OpenVINOEngine::infer(const cv::Mat& input, std::vector<ov::Tensor>& outputs) {
    if (input.empty()) return false;

    try {
        // 1. 预处理：缩放 + 归一化 + BGR转RGB + HWC转CHW
        cv::Mat blob;
        cv::dnn::blobFromImage(input, blob, 1.0 / 255.0, 
                               cv::Size(input_width_, input_height_), 
                               cv::Scalar(), true, false);

        // 2. 获取输入端口信息
        ov::Output<const ov::Node> input_port = compiled_model_.input();
        
        // 3. 创建输入 Tensor 并拷贝数据
        ov::Tensor input_tensor(input_port.get_element_type(), input_port.get_shape(), blob.data);

        // 4. 设置输入并执行推理
        infer_request_.set_input_tensor(input_tensor);
        infer_request_.infer();

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