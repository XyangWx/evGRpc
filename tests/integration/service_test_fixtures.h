#pragma once
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include "fixtures/shared_pg.h"
#include "fixtures/test_server.h"

namespace evgrpc::test {

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