#!/usr/bin/env bash
#
# load_schema.sh — apply sql/001_initial.sql to a PostgreSQL database.
#
# Used to bring a (typically remote) database up to the schema version
# the current binary expects. 001_initial.sql is idempotent (uses CREATE
# TABLE IF NOT EXISTS + DO $$ ... duplicate_object handler), so safe to
# re-run any time.
#
# Usage:
#   DATABASE_URL='postgresql://user:***@host:5432/db' scripts/load_schema.sh
#   scripts/load_schema.sh 'postgresql://user:***@host:5432/db'
#
# Notes:
#   * The target database must already exist. This script intentionally
#     does NOT call CREATE DATABASE (which can't run inside a
#     transaction anyway). Use `createdb` or your DBA tooling for that.
#   * Password with URL-special chars (`@`, `/`, `:`) MUST be URL-encoded
#     in the connection URL (e.g. `@` → `%40`). libpq's URL parser is
#     strict about this.
#   * Tested with psql 16 (Ubuntu 24.04). psql 10+ accepts both
#     `postgresql://...` URL form and `host=... dbname=...` keyword form.
#
# Exit codes:
#   0  schema applied (or already present)
#   1  DATABASE_URL missing, psql not on PATH, connection failed,
#      load failed, or post-load sanity check failed.

set -euo pipefail

# --- resolve conninfo: env > $1 > die ---
if [[ -z "${DATABASE_URL:-}" ]]; then
  if [[ $# -ge 1 && -n "$1" ]]; then
    DATABASE_URL="$1"
  else
    echo "ERROR: DATABASE_URL not set and no argument given." >&2
    echo "Usage: DATABASE_URL='postgresql://...' $0" >&2
    echo "   or: $0 'postgresql://...'" >&2
    exit 1
  fi
fi

# --- prereqs ---
if ! command -v psql >/dev/null 2>&1; then
  echo "ERROR: psql not on PATH. Install postgresql-client (apt) or libpq (brew)." >&2
  exit 1
fi

# --- locate schema files (relative to this script) ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Apply 001 (canonical schema) then 002 (TIMESTAMPTZ migration for
# existing DBs). 002 is idempotent so safe on fresh installs.
SQL_FILE_001="$SCRIPT_DIR/../sql/001_initial.sql"
SQL_FILE_002="$SCRIPT_DIR/../sql/002_charging_timestamptz_migration.sql"
if [[ ! -f "$SQL_FILE_001" ]]; then
  echo "ERROR: schema file not found: $SQL_FILE_001" >&2
  exit 1
fi
if [[ ! -f "$SQL_FILE_002" ]]; then
  echo "ERROR: schema file not found: $SQL_FILE_002" >&2
  exit 1
fi

# --- mask password in echoed URL ---
# Matches the URL form (postgresql://user:password@host/...). The user
# portion (between :// and :) is preserved for clarity; the password is
# redacted. libpq keyword form (host=... password=...) is passed
# through unchanged — typically used with PGPASSWORD, not inline.
MASKED_URL="$(printf '%s' "$DATABASE_URL" \
  | sed -E 's#://([^:]+):[^@]+@#://\1:***@#')"

echo "→ target:  $MASKED_URL"
echo "→ schema:  $SQL_FILE_001 + $SQL_FILE_002"
echo "→ psql:    $(psql --version)"

# --- verify connection first (cleaner failure mode than mid-load) ---
echo "→ verifying connection..."
if ! psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -c 'SELECT 1' >/dev/null 2>&1; then
  echo "ERROR: connection failed. Check host/port/user/password in DATABASE_URL." >&2
  echo "  (password with '@' must be URL-encoded as '%40')" >&2
  exit 1
fi

# --- apply schema ---
echo "→ applying schema (idempotent)..."
START=$(($(date +%s)))
if ! psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f "$SQL_FILE_001"; then
  echo "ERROR: schema load failed (001_initial.sql). See psql output above." >&2
  exit 1
fi
if ! psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f "$SQL_FILE_002"; then
  echo "ERROR: schema load failed (002_charging_timestamptz_migration.sql). See psql output above." >&2
  exit 1
fi
ELAPSED=$(($(date +%s) - START))
echo "→ applied in ${ELAPSED}s"

# --- post-load sanity check ---
TABLES=$(psql "$DATABASE_URL" -tAc \
  "SELECT count(*) FROM information_schema.tables WHERE table_schema='public'")
VEHICLE=$(psql "$DATABASE_URL" -tAc "SELECT to_regclass('public.vehicle')")
echo "→ public tables: $TABLES"
if [[ "$VEHICLE" != "vehicle" ]]; then
  echo "ERROR: 'vehicle' table missing after load (got '$VEHICLE')." >&2
  exit 1
fi
echo "→ OK (vehicle present)"
