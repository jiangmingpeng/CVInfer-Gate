#include "database/DBWriter.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include "utils/Logger.h"

// ------------------------- 生命周期 -------------------------

DBWriter::~DBWriter() {
    stop();
}

bool DBWriter::init(const AppConfig& config) {
    const auto& db = config.database;

    db_cfg_ = db;   // [T36] 保存配置: 后台重连要用

    // 1.  批量 / 重试 / 降级 阈值 (全部来自 DatabaseConfig, 带兜底默认)
    //     注: [T36] 提到“连库”之前 —— 降级提示里要用到 probe_interval_ / fallback_path_
    batch_size_ = db.batch_size < 1 ? 20 : db.batch_size;
    batch_interval_ =
        std::chrono::milliseconds(db.flush_interval_ms < 1 ? 1000 : db.flush_interval_ms);
    probe_interval_ =
        std::chrono::milliseconds(db.reconnect_interval_ms < 1 ? 30000 : db.reconnect_interval_ms);
    probe_after_calls_ = db.reconnect_after_writes < 1 ? 100 : db.reconnect_after_writes;
    fallback_path_ = db.fallback_path;
    pop_timeout_ = batch_interval_;   // 空闲时按批量间隔唤起, 保证定时冲刷

    // 2. [T36] 连接池: **只做一次快速尝试**(不重试、不睡眠)。
    //    失败**不再让调用方退出** —— 转“降级模式”, 由后台线程按
    //    reconnect_interval_ms / reconnect_after_writes 自动重连
    //    (见 ensurePoolAvailable)。这样“数据库没起”不再等于“程序起不来”:
    //    演示/联调时 gRPC 与流水线照常工作, 记录先落本地 CSV。
    if (!pool_.init(db, /*max_attempts=*/1, /*retry_sleep_ms=*/0)) {
        db_healthy_ = false;   // 直接进入“暂停写库 + 定时探测”, 不等第一帧才发现
        CVLOG_WARN << "[DBWriter][T36] 数据库不可用, 以【降级模式】启动: 记录先落本地 "
                   << (fallback_path_.empty() ? "(未配置降级文件!)" : fallback_path_)
                   << ", 每 " << probe_interval_.count() << "ms 或 "
                   << probe_after_calls_ << " 次冲刷后自动重连; 恢复后自动回传并清理。"
                   << " (程序不再因此退出)";
    }

    // 3. 有界任务队列 (容量随 batch_size 放大, 防爆)
    const std::size_t capacity =
        std::max<std::size_t>(256, static_cast<std::size_t>(batch_size_) * 16);
    queue_ = std::make_unique<ThreadSafeQueue<Task>>(
        capacity, ThreadSafeQueue<Task>::Policy::DropOldest);

    // 4. 启动后台写库线程
    running_ = true;
    writer_thread_ = std::thread([this] { writerLoop(); });

    CVLOG_INFO << "[DBWriter] 初始化完成 (连接池=" << pool_.size()
               << (db_healthy_.load() ? "" : " [降级模式]")
               << ", 队列容量=" << capacity << ", batch=" << batch_size_ << "/"
               << batch_interval_.count() << "ms, 降级文件="
               << (fallback_path_.empty() ? "(禁用)" : fallback_path_) << ")";
    // [T36] 无论连没连上库都返回 true: 让进程能继续把 gRPC / 流水线跑起来。
    return true;
}

void DBWriter::flush() {
    if (!queue_) return;
    //  同时等待任务队列与批缓冲排空
    while (running_.load() && (queue_->size() > 0 || buffered_rows_.load() > 0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void DBWriter::stop() {
    if (queue_) queue_->close();
    if (writer_thread_.joinable()) writer_thread_.join();
    running_ = false;
    pool_.close();
}

// ------------------------- 异步入队 -------------------------

bool DBWriter::writeDetections(const std::vector<DetectionResult>& detections) {
    if (!queue_ || !running_.load() || detections.empty()) return false;
    Task task;
    task.type = Task::Type::Detection;
    task.detections = detections;
    queue_->push(std::move(task));
    return true;
}

bool DBWriter::writeAlert(const std::string& type, const std::string& description) {
    if (!queue_ || !running_.load()) return false;
    Task task;
    task.type = Task::Type::Alert;
    task.alert_type = type;
    task.alert_desc = description;
    queue_->push(std::move(task));
    return true;
}

// ------------------------- 后台线程 -------------------------

void DBWriter::writerLoop() {
    const auto start = std::chrono::steady_clock::now();
    last_flush_ = start;
    last_probe_ = start;

    Task task;
    while (true) {
        bool timed_out = false;
        const bool got = queue_->pop(task, pop_timeout_, &timed_out);
        if (got) {
            appendTaskToBatch(task);          // 攒批而非立即写
        } else if (!timed_out) {
            break;                            // 队列已关闭且为空
        }
        if (shouldFlush()) {                  // 够量或到点
            flushBatch();
        }
    }
    // 退出前尽量把剩余缓冲写掉 (尽力而为)
    if (!batch_.empty()) {
        flushBatch();
    }
}

// ------------------------- 批处理 -------------------------

void DBWriter::appendTaskToBatch(const Task& task) {
    if (task.type == Task::Type::Detection) {
        for (const auto& det : task.detections) {
            Row r;
            r.type = Row::Type::Detection;
            r.class_id = det.class_id;
            r.label = det.label;
            r.confidence = det.confidence;
            r.x1 = det.box.x;
            r.y1 = det.box.y;
            r.x2 = det.box.x + det.box.width;
            r.y2 = det.box.y + det.box.height;
            batch_.push_back(std::move(r));
        }
    } else {
        Row r;
        r.type = Row::Type::Alert;
        r.alert_type = task.alert_type;
        r.alert_desc = task.alert_desc;
        batch_.push_back(std::move(r));
    }
    buffered_rows_ = batch_.size();
}

bool DBWriter::shouldFlush() const {
    if (batch_.empty()) return false;
    if (static_cast<int>(batch_.size()) >= batch_size_) return true;               // 够量
    return (std::chrono::steady_clock::now() - last_flush_) >= batch_interval_;    // 到点
}

bool DBWriter::shouldProbe() const {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_probe_ >= probe_interval_) return true;   // 每隔 30s
    if (probe_counter_ >= probe_after_calls_) return true;   // 或累计 100 次冲刷
    return false;
}

// [T36] 池为空(启动时数据库不可用)时, 单次重建。
//   刻意传 (1, 0): 只试一次、不睡眠 —— 探测是“顺路做一下”, 不能把
//   写库线程阻塞几秒; 这次不成, 下一轮(probe_interval_ms 后)再来。
bool DBWriter::ensurePoolAvailable() {
    if (pool_.ready()) return true;
    const bool ok = pool_.init(db_cfg_, /*max_attempts=*/1, /*retry_sleep_ms=*/0);
    if (ok) {
        CVLOG_INFO << "[DBWriter][T36] 连接池已重建, 连接数=" << pool_.size();
    }
    return ok;
}

void DBWriter::flushBatch() {
    if (batch_.empty()) {
        last_flush_ = std::chrono::steady_clock::now();
        return;
    }

    bool written = false;
    if (db_healthy_.load()) {
        written = tryWriteOnce();
    } else if (shouldProbe()) {
        // 到重试时机: 做一次探测性写入
        last_probe_ = std::chrono::steady_clock::now();
        probe_counter_ = 0;
        // [T36] 池里一个连接都没有(启动时就没连上)时, 先单次重建池 ——
        //   否则 acquire() 永远返回 nullptr, 永远恢复不了。
        written = ensurePoolAvailable() && tryWriteOnce();
    } else {
        ++probe_counter_;   // 数据库不可用且未到重试时机: 直接跳过写库
    }

    if (!written) {
        spillToFallback(batch_);   // 降级: 写本地, 保证数据不丢
    }

    batch_.clear();
    buffered_rows_ = 0;
    last_flush_ = std::chrono::steady_clock::now();
}

bool DBWriter::tryWriteOnce() {
    ConnectionGuard conn(pool_, std::chrono::milliseconds(3000));
    if (!conn.valid()) {
        markUnhealthy("获取数据库连接超时");
        return false;
    }
    if (!writeBatch(conn.get())) {
        return false;   // writeBatch 内部已 markUnhealthy
    }
    // 由"不健康"转为"健康": 说明数据库恢复, 回传本地缓存
    if (!db_healthy_.exchange(true)) {
        db_reconnects_.fetch_add(1);   // [T36] 观测
        CVLOG_INFO << "[DBWriter] 数据库已恢复(第 " << db_reconnects_.load()
                   << " 次), 开始回传本地缓存...";
        replayFallback();
    }
    return true;
}

void DBWriter::markUnhealthy(const std::string& reason) {
    // exchange 返回旧值: 仅当由"健康"->"不健康"时打印一次警告
    if (!db_healthy_.exchange(false)) return;
    CVLOG_WARN << "[DBWriter] 数据库不可用, 暂停写库并转本地降级 (后续每 "
               << probe_interval_.count() << "ms 或 " << probe_after_calls_
               << " 次冲刷后重试)。原因: " << reason;
    last_probe_ = std::chrono::steady_clock::now();
    probe_counter_ = 0;
}

bool DBWriter::writeBatch(sql::Connection* conn) {
    if (!conn || batch_.empty()) return true;

    // 拆分为检测行 / 告警行 (只存指针, 零拷贝)
    std::vector<const Row*> dets;
    std::vector<const Row*> alerts;
    dets.reserve(batch_.size());
    alerts.reserve(batch_.size());
    for (const auto& r : batch_) {
        if (r.type == Row::Type::Detection) dets.push_back(&r);
        else alerts.push_back(&r);
    }

    // 单条 INSERT 最多拼多少行: 防 65535 占位符上限 & max_allowed_packet 超限
    constexpr std::size_t kMaxRowsPerStmt = 500;

    try {
        conn->setAutoCommit(false);

        // ---- 检测行: 多 VALUES 的 INSERT, 一条语句写多行(单往返) ----
        // 注: 不使用 addBatch()/executeBatch() (部分 Connector/C++ 版本不提供),
        //     仅依赖 prepareStatement/setXxx/executeUpdate 等基础 API
        for (std::size_t i = 0; i < dets.size(); i += kMaxRowsPerStmt) {
            const std::size_t n = std::min(kMaxRowsPerStmt, dets.size() - i);
            std::string sql =
                "INSERT INTO detections (class_id, label, confidence, x1, y1, x2, y2) VALUES ";
            for (std::size_t j = 0; j < n; ++j) {
                if (j) sql += ',';
                sql += "(?,?,?,?,?,?,?)";
            }
            std::unique_ptr<sql::PreparedStatement> ps(conn->prepareStatement(sql));
            for (std::size_t j = 0; j < n; ++j) {
                const Row& r = *dets[i + j];
                const int base = static_cast<int>(j) * 7;
                ps->setInt(base + 1, r.class_id);
                ps->setString(base + 2, r.label);
                ps->setDouble(base + 3, r.confidence);
                ps->setInt(base + 4, r.x1);
                ps->setInt(base + 5, r.y1);
                ps->setInt(base + 6, r.x2);
                ps->setInt(base + 7, r.y2);
            }
            ps->executeUpdate();
        }

        // ---- 告警行: 同样多 VALUES ----
        for (std::size_t i = 0; i < alerts.size(); i += kMaxRowsPerStmt) {
            const std::size_t n = std::min(kMaxRowsPerStmt, alerts.size() - i);
            std::string sql = "INSERT INTO alerts (alert_type, description) VALUES ";
            for (std::size_t j = 0; j < n; ++j) {
                if (j) sql += ',';
                sql += "(?,?)";
            }
            std::unique_ptr<sql::PreparedStatement> ps(conn->prepareStatement(sql));
            for (std::size_t j = 0; j < n; ++j) {
                const Row& r = *alerts[i + j];
                ps->setString(static_cast<int>(j) * 2 + 1, r.alert_type);
                ps->setString(static_cast<int>(j) * 2 + 2, r.alert_desc);
            }
            ps->executeUpdate();
        }

        conn->commit();
        conn->setAutoCommit(true);
        return true;
    } catch (sql::SQLException& e) {
        try {
            conn->rollback();
            conn->setAutoCommit(true);
        } catch (...) {
            // 回滚阶段的异常忽略
        }
        std::string reason =
            std::string(e.what()) + " (code=" + std::to_string(e.getErrorCode()) + ")";
        if (e.getErrorCode() == 1146) reason += " [表不存在]";
        else if (e.getErrorCode() == 1049) reason += " [数据库不存在]";
        markUnhealthy(reason);
        return false;
    }
}

// ------------------------- 本地降级 / 回传 -------------------------

namespace {

// CSV 字段转义: 含逗号/引号/换行时用双引号包裹, 内部引号翻倍
std::string csvQuote(const std::string& s) {
    const bool need = s.find_first_of(",\"\r\n") != std::string::npos;
    if (!need) return s;
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// 解析一行 CSV (支持双引号包裹与转义)
std::vector<std::string> csvSplit(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur.push_back('"'); ++i; }
                else in_quotes = false;
            } else {
                cur.push_back(c);
            }
        } else if (c == '"') {
            in_quotes = true;
        } else if (c == ',') {
            fields.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    fields.push_back(cur);
    return fields;
}

}  // namespace

void DBWriter::spillToFallback(const std::vector<Row>& rows) {
    if (fallback_path_.empty() || rows.empty()) return;

    std::ofstream f(fallback_path_, std::ios::app);
    if (!f) {
        if (!fallback_error_logged_.exchange(true)) {
            CVLOG_ERROR << "[DBWriter] 无法写入本地降级文件: " << fallback_path_
                        << " (后续不再重复告警)";
        }
        return;
    }
    for (const auto& r : rows) {
        if (r.type == Row::Type::Detection) {
            f << "detection," << r.class_id << ',' << csvQuote(r.label) << ','
              << r.confidence << ',' << r.x1 << ',' << r.y1 << ','
              << r.x2 << ',' << r.y2 << '\n';
        } else {
            f << "alert," << csvQuote(r.alert_type) << ',' << csvQuote(r.alert_desc) << '\n';
        }
    }
    f.flush();
}

void DBWriter::replayFallback() {
    if (fallback_path_.empty()) return;

    std::ifstream in(fallback_path_);
    if (!in) return;   // 没有降级文件

    std::vector<Row> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> f = csvSplit(line);
        if (f.empty()) continue;
        try {
            Row r;
            if (f[0] == "detection" && f.size() >= 8) {
                r.type = Row::Type::Detection;
                r.class_id = std::stoi(f[1]);
                r.label = f[2];
                r.confidence = std::stod(f[3]);
                r.x1 = std::stoi(f[4]);
                r.y1 = std::stoi(f[5]);
                r.x2 = std::stoi(f[6]);
                r.y2 = std::stoi(f[7]);
                rows.push_back(std::move(r));
            } else if (f[0] == "alert" && f.size() >= 3) {
                r.type = Row::Type::Alert;
                r.alert_type = f[1];
                r.alert_desc = f[2];
                rows.push_back(std::move(r));
            }
        } catch (...) {
            // 跳过损坏行
        }
    }
    in.close();

    if (rows.empty()) {
        std::remove(fallback_path_.c_str());
        return;
    }

    ConnectionGuard conn(pool_, std::chrono::milliseconds(3000));
    if (!conn.valid()) {
        CVLOG_WARN << "[DBWriter] 回传失败: 连接不可用, 保留本地文件 " << fallback_path_;
        return;
    }

    // 临时借用 batch_ 作为一次性回传缓冲
    std::vector<Row> saved = std::move(batch_);
    batch_ = std::move(rows);
    const std::size_t replay_count = batch_.size();
    const bool ok = writeBatch(conn.get());
    batch_ = std::move(saved);

    if (ok) {
        std::remove(fallback_path_.c_str());
        CVLOG_INFO << "[DBWriter] 本地缓存已回传并清理, 共 " << replay_count << " 条。";
    } else {
        CVLOG_WARN << "[DBWriter] 回传失败, 保留本地文件以便下次重试。";
    }
}