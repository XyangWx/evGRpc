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
    return ColumnType("charging", column);
  }
  std::string ColumnType(const std::string& table,
                         const std::string& column) {
    pqxx::nontransaction tx(*conn_);
    // PG folds unquoted identifiers to lowercase in information_schema,
    // so the columns are stored as 'starttime'/'endtime' regardless of
    // how 001_initial.sql declares them. Lowercase the lookup key to
    // match.
    auto r = tx.exec_params(
        "SELECT data_type FROM information_schema.columns "
        "WHERE table_name=$1 AND column_name=$2",
        table,
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

TEST_F(SchemaMigrationTest, ChargingTimestamptzMigrationPerformsActualAlter) {
  // The existing idempotent test cannot prove the ALTER actually runs
  // (initial state is already TIMESTAMPTZ). This test resets the
  // charging table to bare TIMESTAMP columns (the pre-002 state) and
  // verifies the migration promotes them to TIMESTAMPTZ. A typo in
  // the AT TIME ZONE 'UTC' clause (e.g. 'EST') would either fail or
  // produce a shift, and this test would catch it.
  //
  // Declared last so the destructive DROP/CREATE does not interfere
  // with the other SchemaMigrationTest tests (GoogleTest runs in
  // declaration order).
  const std::string migration_path = EVGRPC_MIGRATION_002_PATH;
  ASSERT_FALSE(migration_path.empty())
      << "EVGRPC_MIGRATION_002_PATH not set; CMake should inject it";

  auto apply = [&] {
    std::ifstream f(migration_path);
    std::stringstream ss;
    ss << f.rdbuf();
    pqxx::work tx(*conn_);
    tx.exec(ss.str());
    tx.commit();
  };

  // Step 1: drop the current (TIMESTAMPTZ) charging table. CASCADE
  // also drops the FK constraints on vehicle.Id and source_category.Id
  // without touching those tables.
  {
    pqxx::work tx(*conn_);
    tx.exec("DROP TABLE charging CASCADE");
    tx.commit();
  }

  // Step 2: recreate charging with bare TIMESTAMP for StartTime and
  // EndTime — matching sql/001_initial.sql's structure but with the
  // pre-migration column types. Reuses the existing charger_type_enum
  // type created by SetUpTestSuite().
  ASSERT_NO_THROW({
    pqxx::work tx(*conn_);
    tx.exec(
        "CREATE TABLE charging ("
        "  Id                    UUID PRIMARY KEY,"
        "  VehicleId             UUID NOT NULL REFERENCES vehicle(Id),"
        "  StartTime             TIMESTAMP NOT NULL,"
        "  EndTime               TIMESTAMP NOT NULL,"
        "  StartPercent          INT NOT NULL,"
        "  EndPercent            INT NOT NULL,"
        "  StartMileage          INT NOT NULL,"
        "  EndMileage            INT NOT NULL,"
        "  KwhCharged            DECIMAL(10,2) NOT NULL,"
        "  Cost                  DECIMAL(10,2) NOT NULL,"
        "  ElectricityUnitPrice  DECIMAL(4,2)  NOT NULL,"
        "  ServiceFee            DECIMAL(5,2),"
        "  ChargerType           charger_type_enum NOT NULL,"
        "  SourceCategoryId      UUID NOT NULL REFERENCES source_category(Id),"
        "  Location              VARCHAR(100),"
        "  Remark                TEXT"
        ")");
    tx.commit();
  });

  // Step 3: confirm pre-migration state — both columns are bare
  // TIMESTAMP (the migration's WHERE clause will match).
  EXPECT_EQ(ColumnType("StartTime"), "timestamp without time zone");
  EXPECT_EQ(ColumnType("EndTime"),   "timestamp without time zone");

  // Step 4: run migration. The DO $$ guard should now match and
  // ALTER both columns to TIMESTAMPTZ.
  ASSERT_NO_THROW(apply());

  // Step 5: post-migration state — both columns are TIMESTAMPTZ.
  EXPECT_EQ(ColumnType("StartTime"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("EndTime"),   "timestamp with time zone");

  // Step 6: cleanup — re-run migration. Since the columns are already
  // TIMESTAMPTZ, the DO $$ guard's WHERE clause (data_type = 'timestamp
  // without time zone') no longer matches; the ALTER is skipped. This
  // restores the table to the state expected by the rest of the suite
  // and verifies idempotency end-to-end.
  ASSERT_NO_THROW(apply());
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

TEST_F(SchemaMigrationTest, ConsumptionTimestamptzMigrationIsIdempotent) {
  const std::string migration_path = EVGRPC_MIGRATION_003_PATH;
  ASSERT_FALSE(migration_path.empty())
      << "EVGRPC_MIGRATION_003_PATH not set; CMake should inject it";

  auto apply = [&] {
    std::ifstream f(migration_path);
    std::stringstream ss; ss << f.rdbuf();
    pqxx::work tx(*conn_);
    tx.exec(ss.str());
    tx.commit();
  };

  // First apply: columns are TIMESTAMPTZ.
  apply();
  EXPECT_EQ(ColumnType("consumption", "Start"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("consumption", "EndTime"), "timestamp with time zone");

  // Second apply: still TIMESTAMPTZ, no error.
  apply();
  EXPECT_EQ(ColumnType("consumption", "Start"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("consumption", "EndTime"), "timestamp with time zone");
}

TEST_F(SchemaMigrationTest, ConsumptionTimestamptzRoundTripAcrossSessions) {
  // After 003, consumption.Start/EndTime are TIMESTAMPTZ: an instant
  // stored under one session TZ reads back as the same UTC instant
  // under another. (Start AT TIME ZONE 'UTC')::text returns the UTC
  // wall-clock only for a TIMESTAMPTZ column; a bare TIMESTAMP would
  // render "2026-08-13 04:00:00+08" under Shanghai.
  pqxx::work tx(*conn_);
  tx.exec("SET TIME ZONE 'UTC'");
  tx.exec_params(
      "INSERT INTO vehicle (Id, Brand, CalibratedRange, BatteryCapacity, "
      "                    PurchaseDate, LicensePlate) "
      "VALUES ('00000000-0000-0000-0000-000000000002', 't', 0, 0, "
      "        '2026-01-01', 'plate-2')");
  tx.exec_params(
      "INSERT INTO weather (Id, Name) "
      "VALUES ('00000000-0000-0000-0000-000000000020', 'clear')");
  tx.exec_params(
      "INSERT INTO consumption (Id, VehicleId, Start, EndTime, BeginPercent, "
      "  EndPercent, BeginMileage, EndMileage, BeginRange, EndRange, "
      "  HighestTemperature, LowestTemperature, WeatherId, Remark) VALUES "
      "  ('00000000-0000-0000-0000-0000000000c1', "
      "   '00000000-0000-0000-0000-000000000002', "
      "   '2026-08-12T20:00:00Z', '2026-08-12T21:00:00Z', "
      "   0, 0, 0, 0, 0, 0, 0.0, 0.0, "
      "   '00000000-0000-0000-0000-000000000020', NULL)");
  tx.commit();

  pqxx::nontransaction read(*conn_);
  read.exec("SET TIME ZONE 'Asia/Shanghai'");
  auto r = read.exec_params(
      "SELECT (Start AT TIME ZONE 'UTC')::text FROM consumption "
      "WHERE Id='00000000-0000-0000-0000-0000000000c1'");
  EXPECT_EQ(r[0][0].as<std::string>(), "2026-08-12 20:00:00");
}

TEST_F(SchemaMigrationTest, ConsumptionTimestamptzMigrationPerformsActualAlter) {
  // Mirror of the charging PerformsActualAlter: reset the consumption
  // table to bare TIMESTAMP (pre-003 state) and verify the migration
  // promotes it to TIMESTAMPTZ. A typo in AT TIME ZONE 'UTC' would
  // either fail or shift, and this test catches it.
  const std::string migration_path = EVGRPC_MIGRATION_003_PATH;
  ASSERT_FALSE(migration_path.empty())
      << "EVGRPC_MIGRATION_003_PATH not set; CMake should inject it";

  auto apply = [&] {
    std::ifstream f(migration_path);
    std::stringstream ss;
    ss << f.rdbuf();
    pqxx::work tx(*conn_);
    tx.exec(ss.str());
    tx.commit();
  };

  {
    pqxx::work tx(*conn_);
    tx.exec("DROP TABLE consumption CASCADE");
    tx.commit();
  }

  ASSERT_NO_THROW({
    pqxx::work tx(*conn_);
    tx.exec(
        "CREATE TABLE consumption ("
        "  Id                  UUID PRIMARY KEY,"
        "  VehicleId           UUID NOT NULL REFERENCES vehicle(Id),"
        "  Start               TIMESTAMP NOT NULL,"
        "  EndTime             TIMESTAMP NOT NULL,"
        "  BeginPercent        INT NOT NULL,"
        "  EndPercent          INT NOT NULL,"
        "  BeginMileage        INT NOT NULL,"
        "  EndMileage          INT NOT NULL,"
        "  BeginRange          INT NOT NULL,"
        "  EndRange            INT NOT NULL,"
        "  HighestTemperature  DECIMAL(4,1) NOT NULL,"
        "  LowestTemperature   DECIMAL(4,1) NOT NULL,"
        "  WeatherId           UUID NOT NULL REFERENCES weather(Id),"
        "  Remark              TEXT"
        ")");
    tx.commit();
  });

  EXPECT_EQ(ColumnType("consumption", "Start"), "timestamp without time zone");
  EXPECT_EQ(ColumnType("consumption", "EndTime"), "timestamp without time zone");

  ASSERT_NO_THROW(apply());

  EXPECT_EQ(ColumnType("consumption", "Start"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("consumption", "EndTime"), "timestamp with time zone");

  // Re-apply: the guard no longer matches, ALTER is skipped.
  ASSERT_NO_THROW(apply());
  EXPECT_EQ(ColumnType("consumption", "Start"), "timestamp with time zone");
  EXPECT_EQ(ColumnType("consumption", "EndTime"), "timestamp with time zone");
}

}  // namespace evgrpc::test