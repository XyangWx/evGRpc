#include "fixtures/pg_container.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace evgrpc::test {
namespace {

// Default connection when DATABASE_URL is unset. Matches the
// environment the user runs locally for evGRpc development
// (see MEMORY.md "Active projects — A. chfa.jgdt" for the
// canonical pattern; the same PostgreSQL hosts the evGRpc test DB).
constexpr const char* kDefaultHost = "127.0.0.1";
constexpr uint16_t kDefaultPort = 5432;
constexpr const char* kDefaultDatabase = "evgrpc";
constexpr const char* kDefaultUsername = "evgrpc_admin";
constexpr const char* kDefaultPasswordEnv = "EVGRPC_TEST_DB_PASSWORD";
constexpr const char* kDatabaseUrlEnv = "DATABASE_URL";

std::string GetEnvOrThrow(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    throw std::runtime_error(
        std::string("PgContainer: required env var not set (or set DATABASE_URL to override): ") +
        name);
  }
  return v;
}

struct ParsedConninfo {
  std::string host;
  std::string port;
  std::string dbname;
  std::string user;
  std::string password;
};

// URL-decode helper. Handles %XX hex escapes; non-hex chars after %
// are passed through verbatim (so a malformed % stays visible rather
// than silently dropping).
std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hex_val(s[i + 1]);
      int lo = hex_val(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;  // skip past the two hex chars; for-loop's ++i skips the '%'
      } else {
        out += s[i];
      }
    } else {
      out += s[i];
    }
  }
  return out;
}

// Detects PostgreSQL URI form. Both schemes are accepted; libpq treats
// them identically (the difference is just historical).
//
// Note: "postgres://" is 11 chars (postgres + "://"), not 10 — easy to
// off-by-one when typing the prefix length. Using substr + == instead
// of compare(pos, count, literal) so the comparison length is
// self-evident and not a separate magic number.
bool IsUrlForm(const std::string& s) {
  return s.size() >= 13 && s.substr(0, 13) == "postgresql://" ||
         s.size() >= 11 && s.substr(0, 11) == "postgres://";
}

// Parses postgresql://[user[:password]@][host][:port][/dbname][?params]
// — the form every user-facing tool (psql \conninfo, scripts/smoke.sh,
// the README, docker run -e DATABASE_URL=...) uses by default.
ParsedConninfo ParseUrl(const std::string& s) {
  ParsedConninfo out;

  // 1. Strip scheme.
  size_t i = s.find("://");
  if (i == std::string::npos) {
    throw std::runtime_error("PgContainer: URL missing '://': " + s);
  }
  i += 3;

  // 2. Find authority end (next '/' or '?').
  size_t auth_end = s.size();
  for (size_t j = i; j < s.size(); ++j) {
    if (s[j] == '/' || s[j] == '?') { auth_end = j; break; }
  }

  // 3. userinfo (before '@', if '@' is inside authority).
  size_t at = s.find('@', i);
  if (at != std::string::npos && at < auth_end) {
    std::string userinfo = s.substr(i, at - i);
    size_t colon = userinfo.find(':');
    if (colon != std::string::npos) {
      out.user = UrlDecode(userinfo.substr(0, colon));
      out.password = UrlDecode(userinfo.substr(colon + 1));
    } else {
      out.user = UrlDecode(userinfo);
    }
    i = at + 1;
  }

  // 4. host[:port].
  std::string hostport = s.substr(i, auth_end - i);
  size_t colon = hostport.find(':');
  if (colon != std::string::npos) {
    out.host = hostport.substr(0, colon);
    out.port = hostport.substr(colon + 1);
  } else {
    out.host = hostport;
  }

  // 5. dbname (between first '/' after authority and '?').
  if (auth_end < s.size() && s[auth_end] == '/') {
    i = auth_end + 1;
    size_t q = s.find('?', i);
    if (q == std::string::npos) q = s.size();
    out.dbname = s.substr(i, q - i);
  }

  if (out.host.empty() || out.dbname.empty() || out.user.empty()) {
    throw std::runtime_error(
        "PgContainer: URL missing host/dbname/user: " + s);
  }
  return out;
}

// Parses libpq keyword/value conninfo: "host=... port=... dbname=...
// user=... password=***". Splits on whitespace; honors single-quoted
// values.
ParsedConninfo ParseKeyword(const std::string& s) {
  ParsedConninfo out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;

    // Key.
    size_t eq = s.find('=', i);
    if (eq == std::string::npos) {
      throw std::runtime_error(
          "PgContainer: malformed keyword conninfo (no '='): " + s);
    }
    std::string key = s.substr(i, eq - i);
    i = eq + 1;

    // Value: bare up to next whitespace, or single-quoted.
    std::string value;
    if (i < s.size() && s[i] == '\'') {
      ++i;  // skip opening quote
      size_t end = s.find('\'', i);
      if (end == std::string::npos) {
        throw std::runtime_error("PgContainer: unterminated quote in conninfo");
      }
      value = s.substr(i, end - i);
      i = end + 1;
    } else {
      size_t start = i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      value = s.substr(start, i - start);
    }

    if (key == "host" || key == "hostaddr") out.host = value;
    else if (key == "port") out.port = value;
    else if (key == "dbname") out.dbname = value;
    else if (key == "user") out.user = value;
    else if (key == "password") out.password = value;
  }
  if (out.host.empty() || out.dbname.empty() || out.user.empty()) {
    throw std::runtime_error(
        "PgContainer: conninfo missing host/dbname/user: " + s);
  }
  return out;
}

// Dispatch entry. PostgreSQL accepts both URI and keyword forms; libpq
// itself supports both natively — we just need to parse whichever the
// caller hands us so we can populate the per-field accessors and
// build the synthetic URL for connection_string_.
ParsedConninfo ParseConninfo(const std::string& s) {
  if (IsUrlForm(s)) return ParseUrl(s);
  return ParseKeyword(s);
}

}  // namespace

class PgContainer::Impl {
 public:
  // Resolve conninfo: prefer DATABASE_URL if set, else build from
  // defaults + EVGRPC_TEST_DB_PASSWORD.
  std::string conninfo_;
  std::string connection_string_;
  std::string host_;
  uint16_t port_{};
  std::string database_;
  std::string username_;
  std::string password_;

  Impl() {
    const char* db_url = std::getenv(kDatabaseUrlEnv);
    if (db_url && *db_url) {
      // Accept both URI form (postgresql://u:p@h:1/db) and keyword
      // form (host=... dbname=... user=...). ParseConninfo dispatches.
      conninfo_ = db_url;
      auto p = ParseConninfo(conninfo_);
      host_ = p.host;
      port_ = static_cast<uint16_t>(std::stoi(p.port.empty() ? "5432" : p.port));
      database_ = p.dbname;
      username_ = p.user;
      password_ = p.password;
    } else {
      password_ = GetEnvOrThrow(kDefaultPasswordEnv);
      host_ = kDefaultHost;
      port_ = kDefaultPort;
      database_ = kDefaultDatabase;
      username_ = kDefaultUsername;
      conninfo_ = "host=" + host_ +
                  " port=" + std::to_string(port_) +
                  " dbname=" + database_ +
                  " user=" + username_ +
                  " password=" + password_;
    }
    // Synthetic URL form for callers that want to log/print a portable
    // connection string. Built from the resolved fields, so it always
    // matches the actual connection regardless of which form
    // DATABASE_URL used.
    connection_string_ = "postgresql://" + username_ + ":" + password_ +
                          "@" + host_ + ":" + std::to_string(port_) +
                          "/" + database_;
  }
};

PgContainer::PgContainer() : impl_(std::make_unique<Impl>()) {}
PgContainer::~PgContainer() = default;

const std::string& PgContainer::Conninfo() const noexcept {
  return impl_->conninfo_;
}

const std::string& PgContainer::ConnectionString() const noexcept {
  return impl_->connection_string_;
}

const std::string& PgContainer::Host() const noexcept { return impl_->host_; }

uint16_t PgContainer::Port() const noexcept { return impl_->port_; }

const std::string& PgContainer::Database() const noexcept {
  return impl_->database_;
}

const std::string& PgContainer::Username() const noexcept {
  return impl_->username_;
}

const std::string& PgContainer::Password() const noexcept {
  return impl_->password_;
}

}  // namespace evgrpc::test
