#include "fixtures/pg_container.h"

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
        std::string("PgContainer: required env var not set: ") + name);
  }
  return v;
}

// Parses a libpq-style "key=value key=value ..." conninfo string into
// individual fields. Splits on whitespace; honors single-quoted values
// (libpq's escape syntax is overkill for our env-controlled inputs).
struct ParsedConninfo {
  std::string host;
  std::string port;
  std::string dbname;
  std::string user;
  std::string password;
};

ParsedConninfo ParseConninfo(const std::string& s) {
  ParsedConninfo out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;

    // Key.
    size_t eq = s.find('=', i);
    if (eq == std::string::npos) {
      throw std::runtime_error("PgContainer: malformed conninfo (no '='): " + s);
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
      conninfo_ = db_url;  // already libpq-style
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
    // URL form: postgresql://user:password@host:port/dbname. Password
    // may contain URL-unsafe chars — don't URL-encode here; the smoke
    // test runs against a local PG whose password we control.
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