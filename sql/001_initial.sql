-- 001_initial.sql
-- Run against an external PostgreSQL 14+ database.
-- Idempotent: safe to re-run.

BEGIN;

DO $$ BEGIN
    CREATE TYPE charger_type_enum AS ENUM ('fast', 'slow');
EXCEPTION
    WHEN duplicate_object THEN null;
END $$;

CREATE TABLE IF NOT EXISTS vehicle (
  Id               UUID PRIMARY KEY,
  Brand            VARCHAR(36)  NOT NULL,
  CalibratedRange  INTEGER       NOT NULL,
  BatteryCapacity  DECIMAL(10,2) NOT NULL,
  PurchaseDate     DATE          NOT NULL,
  LicensePlate     VARCHAR(15)   NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS weather (
  Id    UUID PRIMARY KEY,
  Name  VARCHAR(36) NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS consumption (
  Id                  UUID PRIMARY KEY,
  VehicleId           UUID NOT NULL REFERENCES vehicle(Id),
  Start               TIMESTAMP NOT NULL,
  EndTime             TIMESTAMP NOT NULL,
  BeginPercent        INT NOT NULL,
  EndPercent          INT NOT NULL,
  BeginMileage        INT NOT NULL,
  EndMileage          INT NOT NULL,
  BeginRange          INT NOT NULL,
  EndRange            INT NOT NULL,
  HighestTemperature  DECIMAL(4,1) NOT NULL,
  LowestTemperature   DECIMAL(4,1) NOT NULL,
  WeatherId           UUID NOT NULL REFERENCES weather(Id),
  Remark              TEXT
);

CREATE TABLE IF NOT EXISTS source_category (
  Id    UUID PRIMARY KEY,
  Name  VARCHAR(36) NOT NULL UNIQUE
);

CREATE TABLE IF NOT EXISTS charging (
  Id                    UUID PRIMARY KEY,
  VehicleId             UUID NOT NULL REFERENCES vehicle(Id),
  StartTime             TIMESTAMPTZ NOT NULL,
  EndTime               TIMESTAMPTZ NOT NULL,
  StartPercent          INT NOT NULL,
  EndPercent            INT NOT NULL,
  StartMileage          INT NOT NULL,
  EndMileage            INT NOT NULL,
  KwhCharged            DECIMAL(10,2) NOT NULL,
  Cost                  DECIMAL(10,2) NOT NULL,
  ElectricityUnitPrice  DECIMAL(4,2)  NOT NULL,
  ServiceFee            DECIMAL(5,2),
  ChargerType           charger_type_enum NOT NULL,
  SourceCategoryId      UUID NOT NULL REFERENCES source_category(Id),
  Location              VARCHAR(100),
  Remark                TEXT
);

CREATE INDEX IF NOT EXISTS idx_consumption_vehicle_start ON consumption(VehicleId, Start);
CREATE INDEX IF NOT EXISTS idx_charging_vehicle_starttime ON charging(VehicleId, StartTime);

COMMIT;