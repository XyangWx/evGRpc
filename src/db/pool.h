#pragma once
#include <memory>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <string>
#include <pqxx/pqxx>

namespace evgrpc {

class PgPool;

class PgConn {
public:
    PgConn(PgPool* pool, std::unique_ptr<pqxx::connection> conn);
    ~PgConn();
    PgConn(const PgConn&) = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&& other) noexcept;
    pqxx::connection& operator*() { return *conn_; }
    pqxx::connection* operator->() { return conn_.get(); }
private:
    PgPool* pool_;
    std::unique_ptr<pqxx::connection> conn_;
};

class PgPool {
public:
    explicit PgPool(const std::string& url, int size = 4);
    PgConn acquire();
    void release(std::unique_ptr<pqxx::connection> conn);
private:
    std::string url_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<pqxx::connection>> idle_;
    int size_;
};

}  // namespace evgrpc
