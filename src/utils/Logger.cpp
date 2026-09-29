#include "utils/Logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>

namespace {

const char* levelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        default:              return "OFF  ";
    }
}

std::string nowString() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03d", buf, static_cast<int>(ms.count()));
    return std::string(out);
}

} // namespace

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::~Logger() {
    std::unique_lock<std::mutex> lock(mtx_);
    if (file_.is_open()) file_.close();
}

void Logger::init(const LogConfig& config) {
    std::unique_lock<std::mutex> lock(mtx_);
    level_ = levelFromString(config.level);
    if (!config.file.empty()) {
        file_.open(config.file, std::ios::app);
        to_file_ = file_.is_open();
        if (!to_file_) {
            std::cerr << "[Logger] 无法打开日志文件: " << config.file << std::endl;
        }
    } else {
        to_file_ = false;
    }
}

void Logger::setLevel(LogLevel level) {
    std::unique_lock<std::mutex> lock(mtx_);
    level_ = level;
}

LogLevel Logger::level() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return level_;
}

bool Logger::isEnabled(LogLevel level) const {
    std::unique_lock<std::mutex> lock(mtx_);
    if (level == LogLevel::Off || level_ == LogLevel::Off) return false;
    return static_cast<int>(level) >= static_cast<int>(level_);
}

void Logger::log(LogLevel level, const std::string& message) {
    std::unique_lock<std::mutex> lock(mtx_);
    if (level == LogLevel::Off || level_ == LogLevel::Off) return;
    if (static_cast<int>(level) < static_cast<int>(level_)) return;

    std::ostringstream oss;
    oss << nowString() << " [" << levelTag(level) << "] ["
        << std::this_thread::get_id() << "] " << message;
    const std::string line = oss.str();

    std::cout << line << std::endl;
    if (to_file_ && file_.is_open()) {
        file_ << line << std::endl;
        file_.flush();
    }
}

LogLevel Logger::levelFromString(const std::string& s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info")  return LogLevel::Info;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    if (s == "off")   return LogLevel::Off;
    return LogLevel::Info;
}

const char* Logger::levelToString(LogLevel level) {
    return levelTag(level);
}
