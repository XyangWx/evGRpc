#include <gtest/gtest.h>
#include "db/error.h"
#include <pqxx/pqxx>

TEST(ErrorMapTest, UniqueViolationMapsToAlreadyExists) {
    pqxx::unique_violation ex("duplicate key value violates unique constraint \"vehicle_licenseplate_key\"");
    auto status = evgrpc::ToGrpcStatus(ex);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

TEST(ErrorMapTest, ForeignKeyViolationMapsToInvalidArgument) {
    pqxx::foreign_key_violation ex("violates foreign key constraint");
    auto status = evgrpc::ToGrpcStatus(ex);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}
