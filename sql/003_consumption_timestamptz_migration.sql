-- sql/003_consumption_timestamptz_migration.sql
-- Migrate consumption.Start / EndTime from TIMESTAMP to TIMESTAMPTZ.
-- Idempotent: per-column DO $$ guards check each column independently
-- and only ALTER columns still bare TIMESTAMP. Existing TIMESTAMP
-- values are interpreted as UTC (the dev environment's convention and
-- the spec's recommended assumption for production deploys), matching
-- the charging migration in sql/002_charging_timestamptz_migration.sql.
--
-- Run AFTER sql/001_initial.sql and sql/002_charging_timestamptz_migration.sql
-- have been applied at least once.
--
-- NOTE: after this migration, EXTRACT(YEAR/MONTH FROM Start) and
-- (Start::date) become session-time-zone aware (like charging). Report
-- boundaries may shift for rows near a day/month/year boundary when the
-- session TZ differs from UTC. Confirm the postmaster timezone matches
-- the intended report convention before deploying.

BEGIN;

DO $$ BEGIN
  IF EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema = 'public' AND table_name = 'consumption'
      AND column_name = 'start'
      AND data_type = 'timestamp without time zone'
  ) THEN
    ALTER TABLE consumption
      ALTER COLUMN start TYPE TIMESTAMPTZ USING start AT TIME ZONE 'UTC';
  END IF;
  IF EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_schema = 'public' AND table_name = 'consumption'
      AND column_name = 'endtime'
      AND data_type = 'timestamp without time zone'
  ) THEN
    ALTER TABLE consumption
      ALTER COLUMN endtime TYPE TIMESTAMPTZ USING endtime AT TIME ZONE 'UTC';
  END IF;
END $$;

COMMIT;
