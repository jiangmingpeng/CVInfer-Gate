#include "utils/ConfigParser.h"

bool ConfigParser::loadAppConfig(const std::string& filepath) {
    try {
        YAML::Node config = YAML::LoadFile(filepath);

        // 视频源配置
        if (config["video"]) {
            app_config_.video_source_type = config["video"]["source_type"].as<std::string>("file");
            app_config_.video_source_path = config["video"]["source_path"].as<std::string>("");
        }

        // 数据库配置
        if (config["database"]) {
            app_config_.db_host = config["database"]["host"].as<std::string>("tcp://127.0.0.1:3306");
            app_config_.db_user = config["database"]["user"].as<std::string>("root");
            app_config_.db_password = config["database"]["password"].as<std::string>("");
            app_config_.db_name = config["database"]["dbname"].as<std::string>("cv_infer");
        }

        // gRPC 配置
        if (config["grpc"]) {
            app_config_.grpc_port = config["grpc"]["port"].as<int>(50051);
        }

        std::cout << "[ConfigParser] 系统配置加载成功: " << filepath << std::endl;
        return true;
    } catch (const YAML::Exception& e) {
        std::cerr << "[ConfigParser] 加载系统配置失败: " << e.what() << std::endl;
        return false;
    }
}

bool ConfigParser::loadModelConfig(const std::string& filepath) {
    try {
        YAML::Node config = YAML::LoadFile(filepath);

        // 模型路径配置
        if (config["model"]) {
            model_config_.model_xml_path = config["model"]["xml_path"].as<std::string>("");
            model_config_.model_bin_path = config["model"]["bin_path"].as<std::string>("");
            model_config_.labels_path = config["model"]["labels_path"].as<std::string>("");
            model_config_.input_width = config["model"]["input_width"].as<int>(640);
            model_config_.input_height = config["model"]["input_height"].as<int>(640);
        }

        // 阈值配置
        if (config["thresholds"]) {
            model_config_.conf_threshold = config["thresholds"]["conf"].as<float>(0.25f);
            model_config_.nms_threshold = config["thresholds"]["nms"].as<float>(0.45f);
        }

        std::cout << "[ConfigParser] 模型配置加载成功: " << filepath << std::endl;
        return true;
    } catch (const YAML::Exception& e) {
        std::cerr << "[ConfigParser] 加载模型配置失败: " << e.what() << std::endl;
        return false;
    }
}