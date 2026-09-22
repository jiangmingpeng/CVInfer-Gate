#pragma once
#include "inference/IInferenceEngine.h"

class OpenVINOEngine : public IInferenceEngine {
public:
    OpenVINOEngine() = default;
    ~OpenVINOEngine() override = default;

    bool init(const ModelConfig& config) override;
    bool infer(const cv::Mat& input, std::vector<ov::Tensor>& outputs) override;

private:
    ov::Core core_;
    ov::CompiledModel compiled_model_;
    ov::InferRequest infer_request_;
    int input_width_ = 640;
    int input_height_ = 640;
};