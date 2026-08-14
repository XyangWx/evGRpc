#include <gtest/gtest.h>
#include <memory>
#include <pqxx/pqxx>
#include "fixtures/pg_container.h"
#include "fixtures/shared_pg.h"
#include "util/timestamp_parse.h"

namespace evgrpc::test {

// RAII guard: SET TIME ZONE in ctor, RESET TIME ZONE in dtor.
// Throws on pqxx errors during SET (test fails loudly); swallows
// RESET errors (cleanup must not mask test failures via throwing
// from a destructor).
class ScopedSessionTimezone {
 public:
  ScopedSessionTimezone(pqxx::connection& c, const std::string& tz)
      : c_(c) {
    pqxx::work tx(c_);
    tx.exec("SET TIME ZONE '" + tx.esc(tz) + "'");
    tx.commit();
  }
  ~ScopedSessionTimezone() noexcept {
    try {
      pqxx::work tx(c_);
      tx.exec("RESET TIME ZONE");
      tx.commit();
    } catch (...) { /* cleanup best-effort */ }
  }
  ScopedSessionTimezone(const ScopedSessionTimezone&) = delete;
  ScopedSessionTimezone& operator=(const ScopedSessionTimezone&) = delete;
 private:
  pqxx::connection& c_;
};

// Verifies that the DisplayService ChargingReport SQL is correctly
// TZ-aware at the SQL level. The spec assumes session-TZ-aware
// grouping via (c.StartTime::date) and EXTRACT on TIMESTAMPTZ.
//
// A gRPC IT for this would silently pass under the wrong TZ because
// TestServer's PgPool doesn't propagate client-side SET TIME ZONE.
// This unit test exercises the SQL directly via libpqxx (where SET
// actually takes effect on the connection), running the exact query
// the handlers run.
class ChargingReportTzTest : public ::testing::Test {
 protected:
  // SharedPgEnvironment owns the shared PG connection and applies the
  // combined schema (001 + 002) once per suite. SetUp() is an instance
  // method on the ::testing::Environment base, but it writes to the
  // file-static `g_pg`, so we can drive it from a stack instance here
  // (same effect as the integration suite's AddGlobalTestEnvironment).
  static void SetUpTestSuite() {
    if (!SharedPgEnvironment::pg()) {
      static SharedPgEnvironment env;
      env.SetUp();
    }
  }
  static void TearDownTestSuite() {
    if (SharedPgEnvironment::pg()) {
      static SharedPgEnvironment env;
      env.TearDown();
    }
  }

  void SetUp() override {
    pg_ = SharedPgEnvironment::pg();
    conn_ = std::make_shared<pqxx::connection>(pg_->Conninfo());
    // Tests use fixed UUIDs; clear residue from prior runs / other
    // test binaries that share the same DB. CASCADE so FK chains
    // (vehicle -> charging -> source_category) drop in one go.
    pqxx::work tx(*conn_);
    tx.exec("TRUNCATE vehicle, charging, consumption, "
            "source_category, weather CASCADE");
    tx.commit();
  }
  std::shared_ptr<PgContainer> pg_;
  std::shared_ptr<pqxx::connection> conn_;
};

TEST_F(ChargingReportTzTest, Daily_AsiaShanghai_RollsToNextDay) {
  // Insert at 2026-08-12T20:00:00Z = 2026-08-13T04:00:00+08:00.
  // Under session TZ=Asia/Shanghai, the row is in the 2026-08-13 day,
  // NOT 2026-08-12.
  {
    pqxx::work tx(*conn_);
    tx.exec("INSERT INTO vehicle (Id, Brand, CalibratedRange, "
            "  BatteryCapacity, PurchaseDate, LicensePlate) VALUES "
            "  ('00000000-0000-0000-0000-000000000001', 't', 0, 0, "
            "   '2026-01-01', 'tz-plate-1')");
    tx.exec("INSERT INTO source_category (Id, Name) VALUES "
            "  ('00000000-0000-0000-0000-000000000010', 'grid')");
    tx.exec_params(
        "INSERT INTO charging (Id, VehicleId, StartTime, EndTime, "
        "  StartPercent, EndPercent, StartMileage, EndMileage, "
        "  KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, "
        "  ChargerType, SourceCategoryId, Location, Remark) VALUES "
        "  ($1, $2, '2026-08-12T20:00:00Z', '2026-08-12T21:00:00Z', "
        "   0, 0, 0, 0, 0, 0, 0, NULL, 'fast', $3, NULL, NULL)",
        "00000000-0000-0000-0000-0000000000a1",
        "00000000-0000-0000-0000-000000000001",
        "00000000-0000-0000-0000-000000000010");
    tx.commit();
  }

  ScopedSessionTimezone shanghai(*conn_, "Asia/Shanghai");
  pqxx::nontransaction read(*conn_);

  // 2026-08-13 Shanghai — should include.
  auto r1 = read.exec_params(
      "SELECT COUNT(*)::INT FROM charging c "
      "WHERE c.StartTime::date = make_date($1, $2, $3)",
      2026, 8, 13);
  EXPECT_EQ(r1[0][0].as<int>(), 1);

  // 2026-08-12 Shanghai — should NOT include (UTC instant is already
  // 4 AM Aug 13 in Shanghai).
  auto r2 = read.exec_params(
      "SELECT COUNT(*)::INT FROM charging c "
      "WHERE c.StartTime::date = make_date($1, $2, $3)",
      2026, 8, 12);
  EXPECT_EQ(r2[0][0].as<int>(), 0);

  // 2026-08-11 Shanghai — sanity check, also 0.
  auto r3 = read.exec_params(
      "SELECT COUNT(*)::INT FROM charging c "
      "WHERE c.StartTime::date = make_date($1, $2, $3)",
      2026, 8, 11);
  EXPECT_EQ(r3[0][0].as<int>(), 0);
}

TEST_F(ChargingReportTzTest, Monthly_AsiaShanghai_GroupsByLocalMonth) {
  // Insert at 2026-07-31T20:00:00Z = 2026-08-01T04:00:00+08:00.
  // Under Shanghai TZ, this is in 2026-08 (not 2026-07).
  {
    pqxx::work tx(*conn_);
    tx.exec("INSERT INTO vehicle (Id, Brand, CalibratedRange, "
            "  BatteryCapacity, PurchaseDate, LicensePlate) VALUES "
            "  ('00000000-0000-0000-0000-000000000002', 't', 0, 0, "
            "   '2026-01-01', 'tz-plate-2')");
    tx.exec("INSERT INTO source_category (Id, Name) VALUES "
            "  ('00000000-0000-0000-0000-000000000011', 'grid')");
    tx.exec_params(
        "INSERT INTO charging (Id, VehicleId, StartTime, EndTime, "
        "  StartPercent, EndPercent, StartMileage, EndMileage, "
        "  KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, "
        "  ChargerType, SourceCategoryId, Location, Remark) VALUES "
        "  ($1, $2, '2026-07-31T20:00:00Z', '2026-07-31T21:00:00Z', "
        "   0, 0, 0, 0, 0, 0, 0, NULL, 'fast', $3, NULL, NULL)",
        "00000000-0000-0000-0000-0000000000a2",
        "00000000-0000-0000-0000-000000000002",
        "00000000-0000-0000-0000-000000000011");
    tx.commit();
  }

  ScopedSessionTimezone shanghai(*conn_, "Asia/Shanghai");
  pqxx::nontransaction read(*conn_);

  auto r1 = read.exec_params(
      "SELECT COUNT(*)::INT FROM charging c "
      "WHERE EXTRACT(YEAR FROM c.StartTime) = $1 "
      "  AND EXTRACT(MONTH FROM c.StartTime) = $2",
      2026, 8);
  EXPECT_EQ(r1[0][0].as<int>(), 1);

  auto r2 = read.exec_params(
      "SELECT COUNT(*)::INT FROM charging c "
      "WHERE EXTRACT(YEAR FROM c.StartTime) = $1 "
      "  AND EXTRACT(MONTH FROM c.StartTime) = $2",
      2026, 7);
  EXPECT_EQ(r2[0][0].as<int>(), 0);
}

// Regression for the ParseTimestamp tz-offset bug: TIMESTAMPTZ::text
// renders the wall-clock time in the *session* time zone with its UTC
// offset. Under Asia/Shanghai the same instant comes back as
// "2023-11-15 06:13:20+08", and the offset must be applied (not
// dropped) to recover the correct UTC epoch. This exercises the real
// SQL string the RowToCharging handlers read, via a connection where
// SET TIME ZONE actually takes effect.
TEST_F(ChargingReportTzTest, ChargingStartTimeText_ParsesCorrectlyUnderShanghai) {
  const char* kId = "00000000-0000-0000-0000-0000000000c1";
  const char* kVid = "00000000-0000-0000-0000-0000000000c2";
  const char* kSid = "00000000-0000-0000-0000-0000000000c3";
  {
    pqxx::work tx(*conn_);
    tx.exec_params(
        "INSERT INTO vehicle (Id, Brand, CalibratedRange, BatteryCapacity, "
        "  PurchaseDate, LicensePlate) VALUES ($1, 't', 0, 0, "
        "  '2026-01-01', 'tz-parse')",
        kVid);
    tx.exec_params("INSERT INTO source_category (Id, Name) VALUES ($1, 'grid')",
                   kSid);
    tx.exec_params(
        "INSERT INTO charging (Id, VehicleId, StartTime, EndTime, "
        "  StartPercent, EndPercent, StartMileage, EndMileage, "
        "  KwhCharged, Cost, ElectricityUnitPrice, ServiceFee, "
        "  ChargerType, SourceCategoryId, Location, Remark) VALUES "
        "  ($1, $2, '2023-11-14T22:13:20Z', '2023-11-14T23:13:20Z', "
        "   0, 0, 0, 0, 0, 0, 0, NULL, 'fast', $3, NULL, NULL)",
        kId, kVid, kSid);
    tx.commit();
  }

  ScopedSessionTimezone shanghai(*conn_, "Asia/Shanghai");
  pqxx::nontransaction read(*conn_);
  auto r = read.exec_params(
      "SELECT StartTime::text, EndTime::text FROM charging WHERE Id=$1", kId);

  google::protobuf::Timestamp st;
  ASSERT_TRUE(ParseTimestamp(r[0][0].as<std::string>(), &st));
  EXPECT_EQ(st.seconds(), 1700000000);

  google::protobuf::Timestamp et;
  ASSERT_TRUE(ParseTimestamp(r[0][1].as<std::string>(), &et));
  EXPECT_EQ(et.seconds(), 1700003600);
}

}  // namespace evgrpc::test
