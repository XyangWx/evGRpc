#pragma once
#include <chrono>
#include <optional>
#include <pqxx/pqxx>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace evgrpc::db {

namespace detail {

// Format a single argument for debug logging.
std::string FormatParam(const std::string& s);
std::string FormatParam(std::string_view s);
std::string FormatParam(const char* s);
std::string FormatParam(bool b);
std::string FormatParam(int n);
std::string FormatParam(long n);
std::string FormatParam(long long n);
std::string FormatParam(unsigned n);
std::string FormatParam(unsigned long n);
std::string FormatParam(unsigned long long n);
std::string FormatParam(double d);
std::string FormatParam(float f);
template <typename T>
std::string FormatParam(const std::optional<T>& opt) {
    if (!opt.has_value()) return "null";
    return FormatParam(*opt);
}
template <typename T>
std::string FormatParam(const T&) {
    return "<" + std::string(typeid(T).name()) + ">";
}

inline void FormatParamsRecursive(std::ostringstream&) {}

template <typename T, typename... Rest>
void FormatParamsRecursive(std::ostringstream& os, T&& first, Rest&&... rest) {
    if (os.tellp() > 0) os << ",";
    os << FormatParam(std::forward<T>(first));
    FormatParamsRecursive(os, std::forward<Rest>(rest)...);
}

template <typename... Args>
std::string FormatParams(Args&&... args) {
    std::ostringstream os;
    FormatParamsRecursive(os, std::forward<Args>(args)...);
    return os.str();
}

}  // namespace detail

// Forward an SQL string + args to pqxx::transaction_base::exec_params.
// Logs:
//   - At db.debug: stmt + params (line 1), then ok + rows + elapsed_us (line 2)
//   - At db.warn:  FAILED + stmt + errcode + errmsg + elapsed_us (single line)
// On pqxx::sql_error: warns, then rethrows.
//
// Visibility:
//   - debug lines are gated by global log.level (no output at info+)
//   - warn line is always emitted (matches flush_on(err) policy)
template <typename... Args>
pqxx::result Exec(pqxx::transaction_base& tx,
                  std::string_view sql,
                  std::string_view label,
                  Args&&... args) {
  auto log = spdlog::get("db");
  auto t0 = std::chrono::steady_clock::now();

  if (log && log->should_log(spdlog::level::debug)) {
    std::string params = detail::FormatParams(std::forward<Args>(args)...);
    log->debug("sql label={} stmt=\"{}\" params={}",
               label, sql, params);
  }

  try {
    // libpqxx 7.x: exec_params takes pqxx::zview (a zero-terminated view).
    // std::string implicitly converts to zview; std::string_view requires
    // an explicit conversion. Buffer SQL into a std::string for the call.
    std::string sql_str(sql);
    pqxx::result r = tx.exec_params(sql_str, std::forward<Args>(args)...);
    if (log && log->should_log(spdlog::level::debug)) {
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
      log->debug("sql label={} ok rows={} elapsed_us={}",
                 label, r.size(), us);
    }
    return r;
  } catch (const pqxx::sql_error& e) {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (log) {
      log->warn("sql label={} FAILED stmt=\"{}\" errcode={} errmsg=\"{}\" elapsed_us={}",
                label, sql, e.sqlstate(), e.what(), us);
    }
    throw;
  }
}

}  // namespace evgrpc::db
