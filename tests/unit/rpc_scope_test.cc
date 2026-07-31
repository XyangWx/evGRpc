#include <gtest/gtest.h>
#include "util/rpc_scope.h"

#include <map>
#include <grpcpp/support/string_ref.h>

TEST(RpcScopeTest, SubjectDefaultsToUnknown) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  evgrpc::RpcScope scope("/test.Service/Method", md, /*subject=*/"");
  EXPECT_EQ(scope.subject(), "<unknown>");
  EXPECT_FALSE(scope.req_id().empty());
}

TEST(RpcScopeTest, StatusStartsAsOk) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  evgrpc::RpcScope scope("/test.Service/Method", md, "alice");
  EXPECT_TRUE(scope.status().ok());
}

TEST(RpcScopeTest, SetStatusUpdatesField) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  evgrpc::RpcScope scope("/test.Service/Method", md, "alice");
  scope.set_status(grpc::Status(grpc::StatusCode::NOT_FOUND, "missing"));
  EXPECT_EQ(scope.status().error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(scope.status().error_message(), "missing");
}

TEST(RpcScopeTest, MetadataReferenceRoundTrips) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  md.emplace(grpc::string_ref("authorization"),
             grpc::string_ref("Bearer xyz"));
  evgrpc::RpcScope scope("/test.Service/Method", md, "alice");
  EXPECT_EQ(scope.metadata().size(), 1u);
  auto it = scope.metadata().find(grpc::string_ref("authorization"));
  ASSERT_NE(it, scope.metadata().end());
}

TEST(RpcScopeTest, EachScopeHasUniqueReqId) {
  std::multimap<grpc::string_ref, grpc::string_ref> md;
  evgrpc::RpcScope a("/test.Service/Method", md, "alice");
  evgrpc::RpcScope b("/test.Service/Method", md, "alice");
  EXPECT_NE(a.req_id(), b.req_id());
}