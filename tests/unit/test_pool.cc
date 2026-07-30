#include <gtest/gtest.h>
#include "db/pool.h"

TEST(PgPoolTest, InvalidUrlThrows) {
    EXPECT_THROW(evgrpc::PgPool p("not-a-url"), std::exception);
}
