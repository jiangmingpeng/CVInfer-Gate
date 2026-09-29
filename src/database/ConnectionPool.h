#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include <mysql_connection.h>
#include <cppconn/driver.h>

#include "utils/ConfigParser.h"

// ============================================================
// ConnectionPool (T9: 数据库连接池)
// ------------------------------------------------------------
// 背景: 原 DBWriter 只持有一个 sql::Connection, 且在消费者线程里
//       同步写库 -> 高并发下成为瓶颈, 且连接一旦失效无恢复能力。
//       本类按 database.pool_size 维护 N 个连接, 用 condition_variable
//       等待空闲连接, 支持超时。
// ============================================================
class ConnectionPool {
public:
    ConnectionPool() = default;
    ~ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    bool init(const DatabaseConfig& config);
    std::shared_ptr<sql::Connection> acquire(std::chrono::milliseconds timeout);
    void release(std::shared_ptr<sql::Connection> conn);
    void close();

    std::size_t size() const;
    std::size_t available() const;

private:
    std::vector<std::shared_ptr<sql::Connection>> conns_;
    std::queue<std::shared_ptr<sql::Connection>> free_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool closed_ = false;
};

// RAII: 出作用域自动归还连接
class ConnectionGuard {
public:
    ConnectionGuard(ConnectionPool& pool, std::chrono::milliseconds timeout);
    ~ConnectionGuard();

    ConnectionGuard(const ConnectionGuard&) = delete;
    ConnectionGuard& operator=(const ConnectionGuard&) = delete;

    bool valid() const { return static_cast<bool>(conn_); }
    sql::Connection* get() const { return conn_.get(); }
    sql::Connection* operator->() const { return conn_.get(); }

private:
    ConnectionPool& pool_;
    std::shared_ptr<sql::Connection> conn_;
};
