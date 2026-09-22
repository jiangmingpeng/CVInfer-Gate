#pragma once

#include <string>
#include <iostream>
#include <yaml-cpp/yaml.h>

// 系统运行配置
struct AppConfig {
    std::string video_source_type;
    std::string video_source_path;
    std::string db_host;
    std::string db_user;
    std::string db_password;
    std::string db_name;
    int grpc_port;
};

// 模型推理配置
struct ModelConfig {
    std::string model_xml_path;
    std::string model_bin_path;
    std::string labels_path;
    int input_width;
    int input_height;
    float conf_threshold;
    float nms_threshold;
};

class ConfigParser {
public:
    ConfigParser() = default;
    ~ConfigParser() = default;

    // 加载配置文件
    bool loadAppConfig(const std::string& filepath);
    bool loadModelConfig(const std::string& filepath);

    // 获取配置的常量引用
    const AppConfig& getAppConfig() const { return app_config_; }
    const ModelConfig& getModelConfig() const { return model_config_; }

private:
    AppConfig app_config_;
    ModelConfig model_config_;
};