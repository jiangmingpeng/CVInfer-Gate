#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <mysql_connection.h>
#include <cppconn/driver.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>

#include "database/ConnectionPool.h"
#include "inference/DetectionResult.h"
#include "utils/ConfigParser.h"
#include "utils/ThreadSafeQueue.h"

// ============================================================
// DBWriter (T9: 连接池 + 异步落库)
// ------------------------------------------------------------
// 与原版差异:
//   1) 单连接 -> ConnectionPool(database.pool_size 个连接)
//   2) writeDetections/writeAlert 改为"异步入队", 立即返回,
//      由后台 writer 线程消费并写库 (真正落地 README 里的"异步落库")
//   3) 新增 flush()/stop(), 析构时自动排空 + 关闭
//   4) 复用 database.max_retries / batch_size / flush_interval_ms
// 注意: 队列有界, 满时按 DropOldest 丢弃并在 stats 中可观测。
//
// T11 功能完善 (在保持对外接口不变的前提下增量增强):
//   A) 数据库状态缓存: 维护 db_healthy_ 状态位。首次写库失败后不再
//      每帧重试; 仅打印一次警告并暂停写库, 之后每 30s 或累积 100 次
//      冲刷后做一次"探测"重试, 以应对偶发网络闪断。
//   B) 异步批量写入: 后台线程把入队任务攒进 batch_, 当条数达到
//      batch_size 或距上次冲刷超过 flush_interval_ms 时, 用
//      多 VALUES 的 INSERT 一条语句写多行 (事务包裹)。仅依赖
//      prepareStatement/setXxx/executeUpdate 等基础 API,
//      不依赖 addBatch()/executeBatch() (部分 Connector/C++ 版本不提供)。
//   C) 本地降级: 当数据库不可用时, 把结构化记录追加写入本地 CSV
//      (database.fallback_path), 保证"数据不丢"; 一旦数据库恢复,
//      自动回传并清理本地文件。
//
// [T36] 修 §20.4「库不可用 ⇒ 程序直接退出」:
//   init() **不再**因连不上库而 return false —— 改为“降级模式启动”
//   (队列/后台线程照常起, 记录先落 CSV), 并由后台线程按
//   reconnect_interval_ms 周期性重建连接池, 恢复后自动回传。
//   配套: 启动只做 1 次快速连库尝试(不再卡启动 ~20 秒); 池空时用
//   ensurePoolAvailable() 单次重建(不重试/不睡眠, 留给下一轮探测)。
// ============================================================
class DBWriter {
public:
    DBWriter() = default;
    ~DBWriter();

    DBWriter(const DBWriter&) = delete;
    DBWriter& operator=(const DBWriter&) = delete;

    // 初始化连接池并启动后台写库线程
    bool init(const AppConfig& config);

    // 以下两个接口均为"异步入队", 返回是否成功入队
    bool writeDetections(const std::vector<DetectionResult>& detections);
    bool writeAlert(const std::string& type, const std::string& description);

    void flush();   // 阻塞直到队列排空 (尽力而为)
    void stop();    // 关闭队列 + join 后台线程 + 关闭连接池

    // [T36] 观测: true=正常写库; false=降级中(记录在本地 CSV)
    bool dbHealthy() const { return db_healthy_.load(); }
    // [T36] 观测: 数据库“不可用 -> 恢复”的累计次数
    std::uint64_t dbReconnects() const { return db_reconnects_.load(); }

private:
    struct Task {
        enum class Type { Detection, Alert } type = Type::Detection;
        std::vector<DetectionResult> detections;   // Detection
        std::string alert_type;                    // Alert
        std::string alert_desc;                    // Alert
    };

    // 统一落库记录 (检测/告警), 供 batch_ 与 CSV 降级共用
    struct Row {
        enum class Type { Detection, Alert } type = Type::Detection;
        // Detection
        int class_id = 0;
        std::string label;
        double confidence = 0.0;
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        // Alert
        std::string alert_type;
        std::string alert_desc;
    };

    void writerLoop();

    // ---- 批量 / 状态缓存 / 降级 ----
    void appendTaskToBatch(const Task& task);            // 入批缓冲
    bool shouldFlush() const;                            // 够量或到点
    void flushBatch();                                   // 落库或降级
    bool tryWriteOnce();                                 // 尝试写一次(含恢复检测)
    bool writeBatch(sql::Connection* conn);              // 批量插入(事务)
    void markUnhealthy(const std::string& reason);       // 转不健康(仅告警一次)
    bool shouldProbe() const;                            // 是否到重试时机
    // [T36] 池为空(启动时就没连上)时, 单次重建连接池; 已连上则直接 true
    bool ensurePoolAvailable();
    void spillToFallback(const std::vector<Row>& rows);  // 本地降级(CSV 追加)
    void replayFallback();                               // 恢复后回传本地缓存

    ConnectionPool pool_;
    DatabaseConfig db_cfg_;                              // [T36] 保存 DB 配置(后台重连要用)
    std::unique_ptr<ThreadSafeQueue<Task>> queue_;
    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    std::chrono::milliseconds pop_timeout_{1000};

    // ---- 批缓冲 (仅 writer 线程访问) ----
    std::vector<Row> batch_;
    std::atomic<std::size_t> buffered_rows_{0};          // 供 flush() 观测

    // ---- 数据库健康状态缓存 ----
    std::atomic<bool> db_healthy_{true};                 // 乐观初值: 首次失败才告警
    std::atomic<std::uint64_t> db_reconnects_{0};        // [T36] 恢复次数(观测)
    std::chrono::steady_clock::time_point last_flush_{};
    std::chrono::steady_clock::time_point last_probe_{};
    int probe_counter_ = 0;                              // 暂停后累计的冲刷次数
    std::atomic<bool> fallback_error_logged_{false};     // 降级写失败仅告警一次

    // ---- 阈值 (来自 DatabaseConfig) ----
    int batch_size_ = 20;                                        // 攒够多少条冲刷
    std::chrono::milliseconds batch_interval_{1000};            // 或每隔多久冲刷
    std::chrono::milliseconds probe_interval_{30000};           // 不健康后重试间隔
    int probe_after_calls_ = 100;                               // 或累计多少次后重试
    std::string fallback_path_ = "db_fallback.csv";             // 本地降级文件
};