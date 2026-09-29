#include "database/ConnectionPool.h"

#include <cppconn/exception.h>

#include <chrono>
#include <iostream>
#include <thread>

ConnectionPool::~ConnectionPool() {
    close();
}

bool ConnectionPool::init(const DatabaseConfig& config) {
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
    const int retries = config.max_retries < 1 ? 1 : config.max_retries;

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
                          << " 次失败: " << e.what() << " 等待 2 秒重试..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        if (!ok) {
            std::cerr << "[ConnectionPool] 连接池初始化失败, 已回滚。" << std::endl;
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
