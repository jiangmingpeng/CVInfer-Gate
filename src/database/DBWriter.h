#pragma once
#include <string>
#include <vector>
#include <mysql_connection.h>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include "inference/DetectionResult.h"
#include "utils/ConfigParser.h"

class DBWriter {
public:
    DBWriter() = default;
    ~DBWriter();

    // 初始化数据库连接
    bool init(const AppConfig& config);
    // 批量写入检测结果
    bool writeDetections(const std::vector<DetectionResult>& detections);
    // 写入告警
    bool writeAlert(const std::string& type, const std::string& description);

private:
    sql::Driver* driver_ = nullptr;
    std::unique_ptr<sql::Connection> conn_;
};