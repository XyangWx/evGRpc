#include "db/pool.h"

namespace evgrpc {

PgConn::PgConn(PgPool* pool, std::unique_ptr<pqxx::connection> conn)
    : pool_(pool), conn_(std::move(conn)) {}

PgConn::~PgConn() {
    if (pool_ && conn_) pool_->release(std::move(conn_));
}

PgConn::PgConn(PgConn&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) { other.pool_ = nullptr; }

PgPool::PgPool(const std::string& url, int size) : url_(url), size_(size) {
    // eagerly open `size_` connections to fail-fast on bad URL
    for (int i = 0; i < size_; ++i) {
        idle_.push(std::make_unique<pqxx::connection>(url_));
    }
}

PgConn PgPool::acquire() {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this]{ return !idle_.empty(); });
    auto c = std::move(idle_.front());
    idle_.pop();
    return PgConn(this, std::move(c));
}

void PgPool::release(std::unique_ptr<pqxx::connection> conn) {
    {
        std::lock_guard lk(mu_);
        idle_.push(std::move(conn));
    }
    cv_.notify_one();
}

}  // namespace evgrpc
