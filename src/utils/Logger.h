#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include "utils/ConfigParser.h"

// ============================================================
// Logger (T2: 分级日志)
// ------------------------------------------------------------
// 让 app.log_level / app.log_file 生效, 逐步替换满项目的
//   std::cout / std::cerr。线程安全, 支持控制台 + 文件双输出。
//
// 用法:
//   Logger::instance().init(app_cfg.log);
//   CVLOG_INFO << "gRPC 监听端口: " << port;
//
// 注意: 宏统一使用 CVLOG_ 前缀, 避免与 <syslog.h> 的 LOG_INFO 冲突。
// ============================================================
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

class Logger {
public:
    static Logger& instance();

    void init(const LogConfig& config);
    void setLevel(LogLevel level);
    LogLevel level() const;
    bool isEnabled(LogLevel level) const;

    void log(LogLevel level, const std::string& message);

    // [T43] 已发生的轮转次数(供 /metrics 与单测观察)
    std::uint64_t rotations() const;

    static LogLevel levelFromString(const std::string& s);
    static const char* levelToString(LogLevel level);

    // 流式构造器 (供 CVLOG 宏使用)
    class LogStream {
    public:
        LogStream(Logger& logger, LogLevel level) : logger_(logger), level_(level) {}
        ~LogStream() { logger_.log(level_, buffer_.str()); }

        template <typename T>
        LogStream& operator<<(const T& value) {
            buffer_ << value;
            return *this;
        }

        // 兼容 std::endl / std::flush 等流操纵符
        LogStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
            buffer_ << manip;
            return *this;
        }

    private:
        Logger& logger_;
        LogLevel level_;
        std::ostringstream buffer_;
    };

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex mtx_;
    LogLevel level_ = LogLevel::Info;
    std::ofstream file_;
    bool to_file_ = false;

    // ---- [T43] 文件轮转状态 ----
    // 轮转策略: 单文件写入量超过 max_bytes_ 时, 把 app.log 改名成 app.log.1,
    //   app.log.1 -> app.log.2 ... 依次后移, 超出的最旧文件删除, 然后重开空的 app.log。
    //   **不做异步/信号驱动**: 轮转发生在写日志的那次调用里(持锁), 避免额外的线程
    //   与并发问题; 日志量小时这点开销可忽略。
    std::string path_;
    std::size_t max_bytes_ = 0;    // 0 = 不轮转
    int keep_files_ = 3;           // 保留的归档数
    std::uint64_t written_ = 0;    // 当前文件已写入字节(含启动前已有内容)
    std::uint64_t rotations_ = 0;

    void rotateLocked();           // 调用方需持锁; 内部不调 CVLOG(会自锁)
};

// ---- 流式日志宏 (安全 for-idiom, 可放心配合 if/else 使用) ----
#define CVLOG(level)                                                              \
    for (bool _cv_log_once = ::Logger::instance().isEnabled(level);               \
         _cv_log_once; _cv_log_once = false)                                      \
        ::Logger::LogStream(::Logger::instance(), level)

#define CVLOG_TRACE CVLOG(::LogLevel::Trace)
#define CVLOG_DEBUG CVLOG(::LogLevel::Debug)
#define CVLOG_INFO  CVLOG(::LogLevel::Info)
#define CVLOG_WARN  CVLOG(::LogLevel::Warn)
#define CVLOG_ERROR CVLOG(::LogLevel::Error)
