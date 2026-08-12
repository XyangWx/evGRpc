#!/usr/bin/env bash
# Run all evGRpc test suites + coverage gate in one shot.
#
# Assumes the binaries are already built:
#   cmake-build-debug/tests/evgrpc_tests
#   cmake-build-debug/tests/integration/evgrpc_integration_tests
#   cmake-build-debug/tests/integration/evgrpc_e2e_tests
#   cmake-build-cov/tests/integration/evgrpc_integration_tests
#
# With the PgContainer config.json fallback (commit ea67689), no env
# vars are required for the unit / integration / e2e suites — they all
# pick up database.url from ./config.json. scripts/coverage.sh still
# expects DATABASE_URL + EVGRPC_TEST_DATABASE_URL set (see the comment
# in that script).
#
# Usage:
#   ./scripts/run_all_tests.sh                  # run everything
#   ./scripts/run_all_tests.sh --no-build       # skip the rebuild step
#   ./scripts/run_all_tests.sh --no-coverage    # skip coverage.sh
#
# Exits non-zero if any suite fails.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/cmake-build-debug}"
COVERAGE_BUILD_DIR="${COVERAGE_BUILD_DIR:-$REPO_ROOT/cmake-build-cov}"

DO_BUILD=1
DO_COVERAGE=1
for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    --no-coverage) DO_COVERAGE=0 ;;
    -h|--help)
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Track results.
declare -a RESULTS=()

run_suite() {
  local name="$1"
  local cmd="$2"
  local start end
  start=$(date +%s)
  echo
  echo "============================================================"
  echo "  $name"
  echo "  \$ ${cmd}"
  echo "============================================================"
  if eval "$cmd"; then
    end=$(date +%s)
    RESULTS+=("PASS  $name  ($((end - start))s)")
  else
    end=$(date +%s)
    RESULTS+=("FAIL  $name  ($((end - start))s)")
  fi
}

# Ensure binaries are built.
if (( DO_BUILD )); then
  echo ">>> Building binaries..."
  cmake --build "$BUILD_DIR" \
    --target evgrpc_tests evgrpc_integration_tests evgrpc_e2e_tests -j4 \
    > /tmp/evgrpc_build.log 2>&1 \
    || { echo "build failed; see /tmp/evgrpc_build.log"; exit 1; }
  cmake --build "$BUILD_DIR" --target evgrpc_server -j4 \
    >> /tmp/evgrpc_build.log 2>&1 || true   # optional
  if (( DO_COVERAGE )); then
    cmake --build "$COVERAGE_BUILD_DIR" \
      --target evgrpc_integration_tests -j4 \
      >> /tmp/evgrpc_build.log 2>&1 \
      || { echo "coverage build failed; see /tmp/evgrpc_build.log"; exit 1; }
  fi
fi

cd "$REPO_ROOT"

# 1. Unit tests.
run_suite "unit (evgrpc_tests, ~99 cases)" \
  "./cmake-build-debug/tests/evgrpc_tests"

# 2. Integration tests.
run_suite "integration (evgrpc_integration_tests, 95 cases)" \
  "./cmake-build-debug/tests/integration/evgrpc_integration_tests"

# 3. E2E tests.
run_suite "e2e (evgrpc_e2e_tests, 2 cases)" \
  "./cmake-build-debug/tests/integration/evgrpc_e2e_tests"

# 4. Coverage gate (optional — needs DATABASE_URL).
if (( DO_COVERAGE )); then
  if [[ -z "${DATABASE_URL:-}" || -z "${EVGRPC_TEST_DATABASE_URL:-}" ]]; then
    echo
    echo ">>> Skipping coverage.sh: DATABASE_URL / EVGRPC_TEST_DATABASE_URL unset."
    echo "    Set them (or unsetting in config.json's database.url fallback) and re-run."
    RESULTS+=("SKIP  coverage.sh  (env vars unset)")
  else
    run_suite "coverage gate (scripts/coverage.sh)" \
      "./scripts/coverage.sh"
  fi
fi

# Summary.
echo
echo "============================================================"
echo "  SUMMARY"
echo "============================================================"
for r in "${RESULTS[@]}"; do
  echo "  $r"
done

# Exit non-zero if any suite failed (not skipped).
for r in "${RESULTS[@]}"; do
  if [[ "$r" == FAIL* ]]; then
    exit 1
  fi
done
exit 0