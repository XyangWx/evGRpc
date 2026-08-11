#include "tests/integration/service_test_fixtures.h"

namespace evgrpc::test {

std::shared_ptr<TestServer> ServiceITBase::server_;
std::shared_ptr<grpc::Channel> ServiceITBase::channel_;

void ServiceITBase::SetUpTestSuite() {
  server_ = std::make_shared<TestServer>(TestServer::Options{
      .no_auth = true, .pg = SharedPgEnvironment::pg() });
  channel_ = server_->Channel();
}

void ServiceITBase::TearDownTestSuite() {
  channel_.reset();
  server_.reset();
}

void ServiceITBase::SetUp() {
  SharedPgEnvironment::TruncateAll();
}

void ServiceITBase::TearDown() {}

}  // namespace evgrpc::test