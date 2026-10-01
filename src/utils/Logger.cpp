#include "utils/Logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
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
    if (file_.is_open()) file_.close();   // 可重入 init(测试/多次初始化)

    path_ = config.file;
    max_bytes_ = config.max_size_mb > 0
                     ? static_cast<std::size_t>(config.max_size_mb) * 1024u * 1024u
                     : 0u;
    keep_files_ = config.keep_files;
    written_ = 0;
    rotations_ = 0;
    to_file_ = false;

    if (!path_.empty()) {
        // 目录不存在就建(否则 ofstream::open 失败 ⇒ 文件日志静默失效, 部署时常踩)
        std::error_code mk_ec;
        const std::filesystem::path p(path_);
        if (p.has_parent_path() && !p.parent_path().empty()) {
            std::filesystem::create_directories(p.parent_path(), mk_ec);
        }

        // 续写已有文件时, 把**已有大小**算进 budget, 否则重启后要再写满一个整额才轮转
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path_, ec);
        if (!ec) written_ = static_cast<std::uint64_t>(sz);

        file_.open(path_, std::ios::app);
        to_file_ = file_.is_open();
        if (!to_file_) {
            std::cerr << "[Logger] 无法打开日志文件: " << path_ << std::endl;
        } else if (max_bytes_ > 0) {
            std::cerr << "[Logger] 日志轮转已开启: " << path_ << " (单文件上限 "
                      << config.max_size_mb << "MB, 保留 " << keep_files_ << " 份)"
                      << std::endl;
        }
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
        written_ += line.size() + 1;
        if (max_bytes_ > 0 && written_ >= max_bytes_) rotateLocked();
    }
}

std::uint64_t Logger::rotations() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return rotations_;
}

void Logger::rotateLocked() {
    namespace fs = std::filesystem;
    std::error_code ec;

    // 0) 在旧文件末尾留个记号: 轮转后翻开归档能看出"为什么这里断了"
    if (file_.is_open()) {
        file_ << nowString() << " [INFO ] [" << std::this_thread::get_id()
              << "] [Logger] 文件达到上限(" << (max_bytes_ / (1024u * 1024u))
              << "MB), 轮转到 " << path_ << ".1" << std::endl;
        file_.close();
    }

    if (keep_files_ <= 0) {
        // 不保留历史: 直接开新的(等价于清空)
        fs::remove(path_, ec);
    } else {
        // 1) 删掉最旧的一份(.keep_files)
        fs::remove(path_ + "." + std::to_string(keep_files_), ec);
        // 2) .N-1 -> .N ... .1 -> .2 (从后往前, 避免覆盖)
        for (int i = keep_files_ - 1; i >= 1; --i) {
            const std::string from = path_ + "." + std::to_string(i);
            if (fs::exists(from, ec)) {
                fs::rename(from, path_ + "." + std::to_string(i + 1), ec);
            }
        }
        // 3) 当前 -> .1
        fs::rename(path_, path_ + ".1", ec);
    }

    // 4) 重开空文件继续写
    file_.open(path_, std::ios::trunc);
    to_file_ = file_.is_open();
    written_ = 0;
    rotations_++;
    if (!to_file_) {
        std::cerr << "[Logger] 轮转后无法重新打开日志文件: " << path_ << std::endl;
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
