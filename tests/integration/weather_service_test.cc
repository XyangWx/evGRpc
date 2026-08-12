// Integration tests for WeatherService.
//
// Uses ServiceITBase (no_auth=true, per-suite TestServer, per-test
// TruncateAll) so the suite can focus on service-shape behavior
// without re-minting bearer tokens or repeating fixture setup.
//
// WeatherService is structurally identical to SourceCategoryService
// (same 2-RPC shape, same Id+Name DB columns, same ^@ starts-with
// search). Chunk 6 groups them because the test patterns are
// identical.
//
// Note: Chunk 4 Task 23 added `data::CreateWeatherId` (direct-SQL)
// for the ConsumptionService FK prerequisite. This file uses the
// REAL WeatherService.CreateWeather gRPC surface (not direct SQL)
// since WeatherService is now implemented and registered.

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <string>

#include "evgrpc/weather.grpc.pb.h"
#include "evgrpc/weather.pb.h"
#include "tests/integration/service_test_fixtures.h"
#include "tests/integration/test_data.h"

namespace evgrpc::test {

// Per-service fixture: derives from ServiceITBase to inherit the
// shared TestServer (no_auth=true) + per-test TruncateAll +
// `channel()` accessor. Mirrors the per-service fixture pattern
// established in Chunks 2-5.
class WeatherServiceIT : public ServiceITBase {};

// Task 44: CreateWeather happy path. Server sets the id on the
// response; client doesn't supply one (CreateWeatherRequest has
// no id field, just name). Asserts id is a non-empty 36-char UUID
// and name round-trips unchanged.
TEST_F(WeatherServiceIT, CreateWeather_HappyPath) {
  auto stub = WeatherService::NewStub(channel());
  CreateWeatherRequest req;
  req.set_name("W-" + data::FreshUuid().substr(0, 8));
  Weather resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->CreateWeather(&ctx, req, &resp).ok());
  EXPECT_FALSE(resp.id().empty());
  EXPECT_EQ(resp.id().size(), 36u);
  EXPECT_EQ(resp.name(), req.name());
}

// Task 44: SearchWeather happy path. Inserts 3 rows with a shared
// "Sunny-" prefix, then searches for that prefix.
TEST_F(WeatherServiceIT, SearchWeather_HappyPath) {
  auto stub = WeatherService::NewStub(channel());
  for (int i = 0; i < 3; ++i) {
    CreateWeatherRequest req;
    req.set_name("Sunny-" + data::FreshUuid().substr(0, 8));
    Weather resp;
    grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateWeather(&c, req, &resp).ok());
  }
  SearchWeatherRequest sreq;
  sreq.set_prefix("Sunny-");
  sreq.set_limit(50);
  SearchWeatherResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchWeather(&ctx, sreq, &resp).ok());
  EXPECT_GE(resp.matches_size(), 3);
}

// Task 44: SearchWeather empty case.
TEST_F(WeatherServiceIT, SearchWeather_Empty) {
  auto stub = WeatherService::NewStub(channel());
  SearchWeatherRequest sreq;
  sreq.set_prefix("ZZZZ-NoSuchPrefix-");
  sreq.set_limit(50);
  SearchWeatherResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchWeather(&ctx, sreq, &resp).ok());
  EXPECT_EQ(resp.matches_size(), 0);
}

// Task 44: SearchWeather limit-capped case. Inserts 5 rows with
// "Cloudy-" prefix, asks for limit=2, asserts result count is <= 2.
TEST_F(WeatherServiceIT, SearchWeather_LimitCapped) {
  auto stub = WeatherService::NewStub(channel());
  for (int i = 0; i < 5; ++i) {
    CreateWeatherRequest req;
    req.set_name("Cloudy-" + data::FreshUuid().substr(0, 8));
    Weather resp;
    grpc::ClientContext c;
    ASSERT_TRUE(stub->CreateWeather(&c, req, &resp).ok());
  }
  SearchWeatherRequest sreq;
  sreq.set_prefix("Cloudy-");
  sreq.set_limit(2);
  SearchWeatherResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(stub->SearchWeather(&ctx, sreq, &resp).ok());
  EXPECT_LE(resp.matches_size(), 2);
}

// Gap closure: duplicate weather.Name triggers pqxx::unique_violation
// -> ToGrpcStatus -> ALREADY_EXISTS via catch block at
// weather_service.cc:36-39.
TEST_F(WeatherServiceIT, CreateWeather_DuplicateName_AlreadyExists) {
  auto stub = WeatherService::NewStub(channel());
  const std::string name = "DUP-" + data::FreshUuid().substr(0, 8);
  CreateWeatherRequest req1;
  req1.set_name(name);
  Weather r1;
  grpc::ClientContext c1;
  ASSERT_TRUE(stub->CreateWeather(&c1, req1, &r1).ok());
  CreateWeatherRequest req2;
  req2.set_name(name);
  Weather r2;
  grpc::ClientContext c2;
  grpc::Status st = stub->CreateWeather(&c2, req2, &r2);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::ALREADY_EXISTS);
}

}  // namespace evgrpc::test
