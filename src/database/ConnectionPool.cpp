#include "database/ConnectionPool.h"

#include <cppconn/exception.h>

#include <chrono>
#include <iostream>
#include <thread>

ConnectionPool::~ConnectionPool() {
    close();
}

bool ConnectionPool::init(const DatabaseConfig& config, int max_attempts, int retry_sleep_ms) {
    close();   // 重置旧资源

    {
        std::unique_lock<std::mutex> lock(mtx_);
        closed_ = false;
    }
    conns_.clear();
    std::queue<std::shared_ptr<sql::Connection>>().swap(free_);

    sql::Driver* driver = nullptr;
    try {
        driver = get_driver_instance();
    } catch (sql::SQLException& e) {
        std::cerr << "[ConnectionPool] 驱动初始化失败: " << e.what() << std::endl;
        return false;
    }

    const int n = config.pool_size < 1 ? 1 : config.pool_size;
    // [T36] 启动时可只试一次(不卡启动); 后台探测也走这里重连。
    const int retries = (max_attempts > 0)
                            ? max_attempts
                            : (config.max_retries < 1 ? 1 : config.max_retries);
    if (retry_sleep_ms < 0) retry_sleep_ms = 0;

    for (int i = 0; i < n; ++i) {
        std::shared_ptr<sql::Connection> conn;
        bool ok = false;
        for (int attempt = 0; attempt < retries && !ok; ++attempt) {
            try {
                conn.reset(driver->connect(config.host, config.user, config.password));
                conn->setSchema(config.dbname);
                ok = true;
            } catch (sql::SQLException& e) {
                std::cerr << "[ConnectionPool] 连接 " << (i + 1) << "/" << n
                          << " 第 " << (attempt + 1) << "/" << retries
                          << " 次失败: " << e.what();
                const bool will_retry = (attempt + 1) < retries && retry_sleep_ms > 0;
                if (will_retry) std::cerr << " 等待 " << retry_sleep_ms << "ms 重试...";
                std::cerr << std::endl;
                if (will_retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(retry_sleep_ms));
                }
            }
        }
        if (!ok) {
            std::cerr << "[ConnectionPool] 连接池初始化失败 (调用方可选择降级重试)。" << std::endl;
            conns_.clear();
            return false;
        }
        conns_.push_back(conn);
    }

    for (auto& c : conns_) free_.push(c);

    std::cout << "[ConnectionPool] 初始化完成, 连接数: " << conns_.size() << std::endl;
    return true;
}

std::shared_ptr<sql::Connection> ConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mtx_);
    if (closed_) return nullptr;
    const bool ok = cv_.wait_for(lock, timeout, [this] { return closed_ || !free_.empty(); });
    if (!ok || free_.empty()) return nullptr;
    auto conn = free_.front();
    free_.pop();
    return conn;
}

void ConnectionPool::release(std::shared_ptr<sql::Connection> conn) {
    if (!conn) return;
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (closed_) return;
        free_.push(std::move(conn));
    }
    cv_.notify_one();
}

void ConnectionPool::close() {
    {
        std::unique_lock<std::mutex> lock(mtx_);
        closed_ = true;
    }
    cv_.notify_all();
}

std::size_t ConnectionPool::size() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return conns_.size();
}

std::size_t ConnectionPool::available() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return free_.size();
}

ConnectionGuard::ConnectionGuard(ConnectionPool& pool, std::chrono::milliseconds timeout)
    : pool_(pool), conn_(pool.acquire(timeout)) {}

ConnectionGuard::~ConnectionGuard() {
    if (conn_) pool_.release(std::move(conn_));
}
