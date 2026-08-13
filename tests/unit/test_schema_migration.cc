#include <gtest/gtest.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <pqxx/pqxx>
#include "fixtures/pg_container.h"
#include "fixtures/shared_pg.h"

namespace evgrpc::test {

// Verifies sql/002_charging_timestamptz_migration.sql:
// (1) running it once moves the columns to TIMESTAMPTZ,
// (2) running it again is a no-op (no error, type stays TIMESTAMPTZ),
// (3) after migration, a row inserted at one session TZ reads back as
//     the same UTC instant at any other session TZ.
class SchemaMigrationTest : public ::testing::Test {
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
  std::string ColumnType(const std::string& column) {
    pqxx::nontransaction tx(*conn_);
    // PG folds unquoted identifiers to lowercase in information_schema,
    // so the columns are stored as 'starttime'/'endtime' regardless of
    // how 001_initial.sql declares them. Lowercase the lookup key to
    // match.
    auto r = tx.exec_params(
        "SELECT data_type FROM information_schema.columns "
        "WHERE table_name='charging' AND column_name=$1",
        [&] {
          std::string lc = column;
          for (auto& c : lc) c = static_cast<char>(std::tolower(c));
          return lc;
        }());
    return r.empty() ? "" : r[0][0].as<std::string>();
  }
  std::shared_ptr<PgContainer> pg_;
  std::shared_ptr<pqxx::connection> conn_;
};

TEST_F(SchemaMigrationTest, ChargingTimestamptzMigrationIsIdempotent) {
  // Ensure both columns are TIMESTAMPTZ before we test idempotency.
  // (SharedPgEnvironment in the integration suite already applies the
  //  combined test schema; for unit tests we apply the migration
  // explicitly here.)
  const std::string migration_path = EVGRPC_MIGRATION_002_PATH;
  ASSERT_FALSE(migration_path.empty())
      << "EVGRPC_MIGRATION_002_PATH not set; CMake should inject it";

  auto apply = [&] {
    std::ifstream f(migration_path);
    std::stringstream ss; ss << f.rdbuf();
    pqxx::work tx(*conn_);
    tx.exec(ss.str());
    tx.commit();
  };

  // First apply: columns are TIMESTAMPTZ.
  apply();
  EXPECT_EQ(ColumnType("StartTime"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("EndTime"),   "timestamp with time zone");

  // Second apply: still TIMESTAMPTZ, no error.
  apply();
  EXPECT_EQ(ColumnType("StartTime"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("EndTime"),   "timestamp with time zone");
}

TEST_F(SchemaMigrationTest, TimestamptzRoundTripAcrossSessions) {
  // After migration, an instant stored in one TZ reads back as the
  // same UTC instant when read in a different TZ. This is the core
  // property that makes the new RPCs correct.
  pqxx::work tx(*conn_);
  // 2026-08-12T20:00:00Z UTC = 2026-08-13T04:00:00+08:00 Shanghai.
  // Insert under session TZ=UTC, read back under session TZ=Shanghai.
  tx.exec("SET TIME ZONE 'UTC'");
  tx.exec_params(
      "INSERT INTO vehicle (Id, Brand, CalibratedRange, BatteryCapacity, "
      "                    PurchaseDate, LicensePlate) "
      "VALUES ('00000000-0000-0000-0000-000000000001', 't', 0, 0, "
      "        '2026-01-01', 'plate-1')");
  tx.exec_params(
      "INSERT INTO source_category (Id, Name) "
      "VALUES ('00000000-0000-0000-0000-000000000010', 'grid')");
  tx.exec_params(
      "INSERT INTO charging (Id, VehicleId, StartTime, EndTime, "
      "                      StartPercent, EndPercent, "
      "                      StartMileage, EndMileage, "
      "                      KwhCharged, Cost, ElectricityUnitPrice, "
      "                      ServiceFee, ChargerType, "
      "                      SourceCategoryId, Location, Remark) "
      "VALUES ('00000000-0000-0000-0000-0000000000a1', "
      "        '00000000-0000-0000-0000-000000000001', "
      "        '2026-08-12T20:00:00Z', '2026-08-12T21:00:00Z', "
      "        0, 0, 0, 0, 0, 0, 0, NULL, 'fast', "
      "        '00000000-0000-0000-0000-000000000010', NULL, NULL)");
  tx.commit();

  pqxx::nontransaction read(*conn_);
  read.exec("SET TIME ZONE 'Asia/Shanghai'");
  // Cast to TIMESTAMP (without TZ) via AT TIME ZONE 'UTC' returns a
  // bare wall-clock literal — no "+00" suffix. The instant is
  // preserved by the value itself (2026-08-12 20:00:00).
  auto r = read.exec_params(
      "SELECT (StartTime AT TIME ZONE 'UTC')::text FROM charging "
      "WHERE Id='00000000-0000-0000-0000-0000000000a1'");
  EXPECT_EQ(r[0][0].as<std::string>(), "2026-08-12 20:00:00");
}

}  // namespace evgrpc::test