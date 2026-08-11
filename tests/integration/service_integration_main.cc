#include <gtest/gtest.h>
#include "fixtures/shared_pg.h"

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new evgrpc::test::SharedPgEnvironment);
  return RUN_ALL_TESTS();
}