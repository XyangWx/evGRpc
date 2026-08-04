#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace evgrpc::test {

// RAII wrapper around a PostgreSQL connection used by integration tests.
//
// Pivoted from testcontainers-cpp to a shared local PG (the one the
// user runs at 127.0.0.1:5432) on 2026-08-04 during Task 20: building
// testcontainers-cpp + grpc + the other FetchContent deps from scratch
// would have taken ~45 min on this 2-core VM and was disproportionate
// to the value of per-test ephemeral DBs for the v1 smoke test. The
// testcontainers-cpp FetchContent_Declare stays in cmake/deps.cmake so
// we can re-enable ephemeral containers later if/when we add a full
// parallel e2e suite.
//
// On construction, opens a libpqxx connection using the DATABASE_URL
// env var (or the hardcoded default of 127.0.0.1:5432 with the
// project's `evgrpc` database), and exposes the connection details.
// On destruction, closes the connection.
//
// Tests are responsible for truncating the tables they touch before
// each run. We don't do that here because (a) tests are not yet
// parallelized, (b) the smoke test only inserts one row and asserts on
// it, and (c) explicit truncation makes the test's data setup visible
// at the test site rather than hidden in the fixture.
class PgContainer {
 public:
  PgContainer();
  ~PgContainer();
  PgContainer(const PgContainer&) = delete;
  PgContainer& operator=(const PgContainer&) = delete;
  PgContainer(PgContainer&&) = delete;
  PgContainer& operator=(PgContainer&&) = delete;

  // libpq keyword=value form: "host=127.0.0.1 port=5432 dbname=evgrpc user=evgrpc_admin password=..."
  // Compatible with libpqxx::connection's constructor.
  const std::string& Conninfo() const noexcept;

  // Postgres URL form: "postgresql://evgrpc_admin:***@127.0.0.1:5432/evgrpc"
  const std::string& ConnectionString() const noexcept;

  const std::string& Host() const noexcept;
  uint16_t Port() const noexcept;
  const std::string& Database() const noexcept;
  const std::string& Username() const noexcept;
  const std::string& Password() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace evgrpc::test