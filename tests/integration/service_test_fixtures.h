#pragma once
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include "fixtures/pg_container.h"
#include "fixtures/shared_pg.h"
#include "fixtures/test_server.h"

namespace evgrpc::test {

// Single-TestServer-per-suite fixture for SERVICE-SHAPE tests only
// (no_auth=true). The shared static `server_` is hard-coded to
// `Options{.no_auth = true}` and cannot be overridden by derived
// fixtures — this prevents silent drift toward a real JWKS / bearer-token
// round-trip setup. Auth tests should NOT derive from this class; if/when
// added, create a parallel `ServiceITBaseAuthed` with per-suite storage.
class ServiceITBase : public ::testing::Test {
 protected:
  static void SetUpTestSuite();   // creates TestServer once per suite
  static void TearDownTestSuite();
  void SetUp() override;          // calls TruncateAll
  void TearDown() override;

  std::shared_ptr<grpc::Channel> channel() const { return channel_; }
  // Direct PG access for tests that need to seed rows outside the
  // gRPC surface (e.g., direct-SQL weather row insertion in Chunk 4
  // Task 23 — WeatherService isn't implemented until Chunk 6).
  std::shared_ptr<PgContainer> pg() const { return SharedPgEnvironment::pg(); }

 private:
  static std::shared_ptr<TestServer> server_;
  static std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace evgrpc::test