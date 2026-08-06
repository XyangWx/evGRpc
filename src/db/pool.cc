#include "db/pool.h"
#include "log/log.h"
#include <chrono>
#include <spdlog/spdlog.h>

namespace evgrpc {

PgConn::PgConn(PgPool* pool, std::unique_ptr<pqxx::connection> conn)
  : pool_(pool), conn_(std::move(conn)) {}

PgConn::~PgConn() {
  if (pool_ && conn_) pool_->release(std::move(conn_));
}

PgConn::PgConn(PgConn&& other) noexcept
  : pool_(other.pool_), conn_(std::move(other.conn_)) { other.pool_ = nullptr; }

PgPool::PgPool(const std::string& url, int size) : url_(url), size_(size) {
  for (int i = 0; i < size_; ++i) {
    idle_.push(std::make_unique<pqxx::connection>(url_));
  }
}

PgConn PgPool::acquire() {
  auto log = spdlog::get("db");
  bool want_log = log && log->should_log(spdlog::level::debug);
  auto t0 = std::chrono::steady_clock::now();
  std::unique_ptr<pqxx::connection> c;
  {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [this]{ return !idle_.empty(); });
    c = std::move(idle_.front());
    idle_.pop();
  }
  if (want_log) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    log->debug("pool.acquire idle_remaining={} wait_us={}", idle_.size(), us);
  }
  return PgConn(this, std::move(c));
}

void PgPool::release(std::unique_ptr<pqxx::connection> conn) {
  {
    std::lock_guard lk(mu_);
    idle_.push(std::move(conn));
  }
  cv_.notify_one();
  auto log = spdlog::get("db");
  if (log && log->should_log(spdlog::level::debug)) {
    log->debug("pool.release idle_remaining={}", idle_.size());
  }
}

}  // namespace evgrpc
