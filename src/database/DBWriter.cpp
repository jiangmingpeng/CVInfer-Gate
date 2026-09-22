#include "database/DBWriter.h"
#include <iostream>
#include <thread>
#include <chrono>

DBWriter::~DBWriter() {
    if (conn_) {
        conn_->close();
    }
}
// 1.
// bool DBWriter::init(const AppConfig& config) {
//     try {
//         driver_ = get_driver_instance();
//         // 假设 config.db_host 是 "tcp://127.0.0.1:3306"
//         conn_.reset(driver_->connect(config.db_host, config.db_user, config.db_password));
//         conn_->setSchema(config.db_name);
//         std::cout << "[DBWriter] 数据库连接成功: " << config.db_name << std::endl;
//         return true;
//     } catch (sql::SQLException& e) {
//         std::cerr << "[DBWriter] 数据库连接失败: " << e.what() << std::endl;
//         return false;
//     }
// }
// 2.
bool DBWriter::init(const AppConfig& config) {
    int max_retries = 10;
    for (int i = 0; i < max_retries; ++i) {
        try {
            driver_ = get_driver_instance();
            // 尝试连接
            conn_.reset(driver_->connect(config.db_host, config.db_user, config.db_password));
            conn_->setSchema(config.db_name);
            std::cout << "[DBWriter] 数据库连接成功: " << config.db_name << std::endl;
            return true;
        } catch (sql::SQLException& e) {
            std::cerr << "[DBWriter] 第 " << (i + 1) << "/" << max_retries 
                      << " 次连接失败: " << e.what() << " 等待 5 秒后重试..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    std::cerr << "[DBWriter] 数据库连接彻底失败，已达最大重试次数。" << std::endl;
    return false;
}


bool DBWriter::writeDetections(const std::vector<DetectionResult>& detections) {
    if (!conn_ || detections.empty()) return false;
    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn_->prepareStatement(
                "INSERT INTO detections (class_id, label, confidence, x1, y1, x2, y2) VALUES (?, ?, ?, ?, ?, ?, ?)"
            )
        );
        
        for (const auto& det : detections) {
            pstmt->setInt(1, det.class_id);
            pstmt->setString(2, det.label);
            pstmt->setDouble(3, det.confidence);
            pstmt->setInt(4, det.box.x);
            pstmt->setInt(5, det.box.y);
            pstmt->setInt(6, det.box.x + det.box.width);
            pstmt->setInt(7, det.box.y + det.box.height);
            pstmt->executeUpdate();
        }
        return true;
    } catch (sql::SQLException& e) {
        std::cerr << "[DBWriter] 写入检测结果失败: " << e.what() << std::endl;
        return false;
    }
}

bool DBWriter::writeAlert(const std::string& type, const std::string& description) {
    if (!conn_) return false;
    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn_->prepareStatement("INSERT INTO alerts (alert_type, description) VALUES (?, ?)")
        );
        pstmt->setString(1, type);
        pstmt->setString(2, description);
        pstmt->executeUpdate();
        return true;
    } catch (sql::SQLException& e) {
        std::cerr << "[DBWriter] 写入告警失败: " << e.what() << std::endl;
        return false;
    }
}