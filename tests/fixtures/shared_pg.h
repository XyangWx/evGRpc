#pragma once
#include <gtest/gtest.h>
#include <memory>
#include "fixtures/pg_container.h"

namespace evgrpc::test {

class SharedPgEnvironment : public ::testing::Environment {
 public:
  void SetUp() override;    // opens PgContainer + applies sql/001_initial.sql
  void TearDown() override;
  static std::shared_ptr<PgContainer> pg();
  static void TruncateAll();
};

}  // namespace evgrpc::test
