// Integration tests for SourceCategoryService.
//
// Uses ServiceITBase (no_auth=true, per-suite TestServer, per-test
// TruncateAll) so the suite can focus on service-shape behavior
// without re-minting bearer tokens or repeating fixture setup.
//
// SourceCategoryService has 2 RPCs: CreateSourceCategory + SearchSourceCategory.
// Both insert/select the source_category table only (no FK checks).
// Search uses the ^@ (starts-with) operator with optional limit.
//
// Helper data::FreshUuid() is used to mint unique names so the table's
// UNIQUE(Name) constraint never collides across runs.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <string>

#include "evgrpc/source_category.grpc.pb.h"
#include "evgrpc/source_category.pb.h"
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"

namespace evgrpc::test {

// Per-service fixture: derives from ServiceITBase to inherit the
// shared TestServer (no_auth=true) + per-test TruncateAll +
// `channel()` accessor. Mirrors the per-service fixture pattern
// established in Chunks 2-5.
class SourceCategoryServiceIT : public ServiceITBase {};

// Task 42: CreateSourceCategory happy path. Server sets the id on
// the response; client doesn't supply one (CreateSourceCategoryRequest
// has no id field, just name). Asserts id is a non-empty 36-char
// UUID and name round-trips unchanged.
TEST_F(SourceCategoryServiceIT, CreateSourceCategory_HappyPath) {
  auto stub = SourceCategoryService::NewStub(channel());
  CreateSourceCategoryRequest req;
  req.set_name("SC-" + data::FreshUuid().substr(0, 8));
  SourceCategory resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateSourceCategory(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.id().size(), 36u);
  EXPECT_EQ(resp.name(), req.name());
}

// Task 43: SearchSourceCategory happy path. Inserts 3 rows with a
// shared "Solar-" prefix, then searches for that prefix. Asserts
// at least 3 matches (>= because TruncateAll doesn't drop the
// table, only clears rows; concurrent test runs in the same DB
// wouldn't see each other's data, but the assertion is defensive).
TEST_F(SourceCategoryServiceIT, SearchSourceCategory_HappyPath) {
  auto stub = SourceCategoryService::NewStub(channel());
  for (int i = 0; i < 3; ++i) {
    CreateSourceCategoryRequest req;
    req.set_name("Solar-" + data::FreshUuid().substr(0, 8));
    SourceCategory resp;
    grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateSourceCategory(&c, req, &resp).ok());
  }
  SearchSourceCategoryRequest sreq;
  sreq.set_prefix("Solar-");
  sreq.set_limit(50);
  SearchSourceCategoryResponse resp;
  grpc::ClientContext cx;
  ASSERT_TRUE(stub->SearchSourceCategory(&cx, sreq, &resp).ok());
  EXPECT_GE(resp.matches_size(), 3);
}

// Task 43: SearchSourceCategory empty case. A prefix that no row
// matches returns an empty matches list (NOT an error — the RPC
// succeeds with 0 rows). Confirms the LIKE/^@ filter actually
// filters (doesn't return everything).
TEST_F(SourceCategoryServiceIT, SearchSourceCategory_Empty) {
  auto stub = SourceCategoryService::NewStub(channel());
  SearchSourceCategoryRequest sreq;
  sreq.set_prefix("ZZZZ-NoSuchPrefix-");
  sreq.set_limit(50);
  SearchSourceCategoryResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchSourceCategory(&ctx, sreq, &resp).ok());
  EXPECT_EQ(resp.matches_size(), 0);
}

// Task 43: SearchSourceCategory limit-capped case. Inserts 5 rows
// with "Wind-" prefix, asks for limit=2, asserts result count is
// <= 2. Exercises the LIMIT $2 SQL bind (default 50 when limit <= 0,
// per the impl).
TEST_F(SourceCategoryServiceIT, SearchSourceCategory_LimitCapped) {
  auto stub = SourceCategoryService::NewStub(channel());
  for (int i = 0; i < 5; ++i) {
    CreateSourceCategoryRequest req;
    req.set_name("Wind-" + data::FreshUuid().substr(0, 8));
    SourceCategory resp;
    grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateSourceCategory(&c, req, &resp).ok());
  }
  SearchSourceCategoryRequest sreq;
  sreq.set_prefix("Wind-");
  sreq.set_limit(2);  // cap at 2
  SearchSourceCategoryResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchSourceCategory(&ctx, sreq, &resp).ok());
  EXPECT_LE(resp.matches_size(), 2);
}

}  // namespace evgrpc::test
