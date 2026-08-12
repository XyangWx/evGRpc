#!/usr/bin/env bash
# evGRpc coverage script.
#
# Builds with --coverage instrumentation, runs the integration test
# binary, runs lcov, asserts >= 95% line coverage on src/services/*.cc,
# and exits non-zero on failure.
#
# Requirements (verified on Linux 7.0, lcov 2.0+):
#   apt-get install -y lcov  # Debian/Ubuntu
#   brew install lcov        # macOS
#
# Usage:
#   DATABASE_URL="postgresql://..." \
#     EVGRPC_TEST_DATABASE_URL="$DATABASE_URL" \
#     ./scripts/coverage.sh
#
# Overridable env vars:
#   BUILD_DIR          cmake-build-cov (default)
#   COVERAGE_THRESHOLD 95            (default)
#   RUNTIME_THRESHOLD  75            (default, seconds)

set -euo pipefail

# --- Config ---
readonly BUILD_DIR="${BUILD_DIR:-cmake-build-cov}"
readonly COVERAGE_THRESHOLD="${COVERAGE_THRESHOLD:-95}"
readonly RUNTIME_THRESHOLD="${RUNTIME_THRESHOLD:-75}"  # wall-clock seconds
readonly SERVICES_DIR="src/services"

# --- Pre-flight ---
# Ensure CWD is the repo root, regardless of how the script is invoked.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR/.."

if ! command -v lcov >/dev/null || ! command -v genhtml >/dev/null; then
  echo "ERROR: lcov/genhtml not found. Install via 'apt install lcov' or 'brew install lcov'." >&2
  exit 1
fi
if [[ -z "${DATABASE_URL:-}" || -z "${EVGRPC_TEST_DATABASE_URL:-}" ]]; then
  echo "ERROR: DATABASE_URL and EVGRPC_TEST_DATABASE_URL must both be set." >&2
  exit 1
fi

# --- Configure + build ---
echo ">>> Configuring (EVGRPC_COVERAGE=ON) ..."
cmake -S . -B "$BUILD_DIR" -DEVGRPC_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null

echo ">>> Building ..."
cmake --build "$BUILD_DIR" --target evgrpc_integration_tests -j

# --- Run tests + measure wall-clock ---
echo ">>> Running evgrpc_integration_tests ..."
START_TS=$(date +%s)
( cd "$BUILD_DIR" && ctest -R evgrpc_integration_tests --output-on-failure )
END_TS=$(date +%s)
ELAPSED=$(( END_TS - START_TS ))
echo ">>> Test wall-clock: ${ELAPSED}s"

if (( ELAPSED > RUNTIME_THRESHOLD )); then
  echo "ERROR: tests exceeded ${RUNTIME_THRESHOLD}s budget (actual: ${ELAPSED}s)." >&2
  exit 1
fi

# --- lcov capture + summary ---
# Capture ALL .cc files (spec §6 scope: services + critical integration like
# db/exec, db/pool, fixtures/pg_container, config/config_loader). Threshold
# check below parses only src/services/*.cc rows from the per-file summary.
echo ">>> Capturing coverage ..."
COVERAGE_INFO="$BUILD_DIR/coverage.info"
lcov --capture --directory "$BUILD_DIR" \
     --output-file "$COVERAGE_INFO" \
     --exclude '*/generated/*' --exclude '*/_deps/*' --exclude '*/tests/*' \
     --ignore-errors mismatch

echo ">>> Services coverage summary:"
lcov --summary "$COVERAGE_INFO" 2>&1 | awk '
  /^src\/services\/[^ ]*\.cc/ {
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^[0-9]+\.[0-9]+%$/) {
        gsub(/%/, "", $i)
        sum += $i
        n++
        break
      }
    }
  }
  END {
    if (n > 0) printf "lines average across %d services files: %.1f%%\n", n, sum/n
    else print "ERROR: no src/services/*.cc files in coverage.info" >&2
  }
'

# Average services coverage as float, e.g. "95.4"
COVERAGE_PCT=$(lcov --summary "$COVERAGE_INFO" 2>&1 | awk '
  /^src\/services\/[^ ]*\.cc/ {
    for (i = 1; i <= NF; i++) {
      if ($i ~ /^[0-9]+\.[0-9]+%$/) {
        gsub(/%/, "", $i)
        sum += $i
        n++
        break
      }
    }
  }
  END { if (n > 0) printf "%.1f", sum/n; else print "" }
')

# Robust parse: empty COVERAGE_PCT -> clear diagnostic, not cryptic arithmetic error.
if [[ -z "${COVERAGE_PCT}" ]]; then
  echo "ERROR: failed to parse services coverage from lcov summary." >&2
  exit 1
fi
# Integer comparison: float-to-int via printf "%.0f"
COVERAGE_PCT_INT=$(printf "%.0f" "$COVERAGE_PCT")

if (( COVERAGE_PCT_INT < COVERAGE_THRESHOLD )); then
  echo "ERROR: $SERVICES_DIR coverage ${COVERAGE_PCT}% < ${COVERAGE_THRESHOLD}% threshold." >&2
  ABS_HTML="$(cd "$BUILD_DIR" && pwd)/coverage_html/index.html"
  echo ">>> HTML: file://${ABS_HTML}"
  exit 1
fi

# --- HTML ---
echo ">>> Generating HTML report ..."
genhtml "$COVERAGE_INFO" --output-directory "$BUILD_DIR/coverage_html" >/dev/null
ABS_HTML="$(cd "$BUILD_DIR" && pwd)/coverage_html/index.html"
echo ">>> HTML: file://${ABS_HTML}"
echo ">>> Coverage ${COVERAGE_PCT}% >= ${COVERAGE_THRESHOLD}% threshold - PASS"