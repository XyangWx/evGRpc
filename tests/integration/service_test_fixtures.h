#pragma once
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <memory>
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

 private:
  static std::shared_ptr<TestServer> server_;
  static std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace evgrpc::test