// End-to-end smoke test: verifies the TestServer fixture's full plumbing
// (testcontainers PG bringup → schema apply → JWKS HTTP server → JWT
// signing → in-process gRPC server → bearer-token channel credentials →
// generated client stub → round-trip VehicleService RPC).
//
// This is intentionally ONE test. If it passes, the fixture works; if it
// fails, the failure point pinpoints which layer broke. Service-specific
// e2e coverage expands in Task 22 (smoke.sh + the e2e suite proper).

#include <grpcpp/grpcpp.h>
#include <google/protobuf/timestamp.pb.h>
#include <gtest/gtest.h>

#include <memory>

#include "evgrpc/vehicle.grpc.pb.h"
#include "evgrpc/vehicle.pb.h"
#include "fixtures/pg_container.h"
#include "fixtures/test_server.h"

namespace evgrpc::test {

TEST(E2ESmoke, CreateThenListVehicle) {
  auto pg = std::make_shared<PgContainer>();
  TestServer ts(pg);
  auto stub = evgrpc::VehicleService::NewStub(ts.Channel());

  evgrpc::CreateVehicleRequest req;
  req.set_brand("Tesla");
  req.set_calibrated_range_km(500);
  req.set_battery_capacity_kwh(75.0);
  google::protobuf::Timestamp purchase_date;
  purchase_date.set_seconds(1704067200);  // 2024-01-01T00:00:00Z
  *req.mutable_purchase_date() = purchase_date;
  req.set_license_plate("TEST123");

  evgrpc::Vehicle created;
  grpc::ClientContext ctx;
  ctx.set_credentials(ts.BearerTokenCredentials());
  auto status = stub->CreateVehicle(&ctx, req, &created);
  ASSERT_TRUE(status.ok())
      << "CreateVehicle RPC failed: error_code=" << status.error_code()
      << " error_message=" << status.error_message();
  EXPECT_FALSE(created.id().empty());
  EXPECT_EQ(created.license_plate(), "TEST123");

  evgrpc::ListVehiclesRequest list_req;
  evgrpc::ListVehiclesResponse list_resp;
  grpc::ClientContext list_ctx;
  list_ctx.set_credentials(ts.BearerTokenCredentials());
  ASSERT_TRUE(stub->ListVehicles(&list_ctx, list_req, &list_resp).ok())
      << "ListVehicles RPC failed";
  ASSERT_EQ(list_resp.vehicles_size(), 1);
  EXPECT_EQ(list_resp.vehicles(0).license_plate(), "TEST123");
  EXPECT_EQ(list_resp.vehicles(0).brand(), "Tesla");
  EXPECT_EQ(list_resp.vehicles(0).id(), created.id());
}

}  // namespace evgrpc::test