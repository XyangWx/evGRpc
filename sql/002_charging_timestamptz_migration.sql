-- sql/002_charging_timestamptz_migration.sql
-- Migrate charging.StartTime / EndTime from TIMESTAMP to TIMESTAMPTZ.
-- Idempotent: the DO $$ BEGIN guard checks BOTH columns and only runs
-- the ALTER if at least one is still bare TIMESTAMP. Existing TIMESTAMP
-- values are interpreted as UTC (the dev environment's convention and
-- the spec's recommended assumption for production deploys).
--
-- Run AFTER sql/001_initial.sql has been applied at least once.

BEGIN;

DO $$ BEGIN
  IF EXISTS (
    SELECT 1 FROM information_schema.columns
    WHERE table_name = 'charging'
      AND column_name IN ('starttime', 'endtime')
      AND data_type = 'timestamp without time zone'
  ) THEN
    ALTER TABLE charging
      ALTER COLUMN starttime TYPE TIMESTAMPTZ USING starttime AT TIME ZONE 'UTC',
      ALTER COLUMN endtime   TYPE TIMESTAMPTZ USING endtime   AT TIME ZONE 'UTC';
  END IF;
END $$;

COMMIT;