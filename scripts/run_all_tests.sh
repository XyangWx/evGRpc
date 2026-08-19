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
  # Configure debug build dir if not already configured. First run takes
  # ~30+ min (FetchContent rebuilds _deps); subsequent runs reuse the cache.
  if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo ">>> Configuring $BUILD_DIR (first run, may take 30+ min)..."
    cmake -G Ninja -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug \
      > /tmp/evgrpc_build.log 2>&1 \
      || { echo "configure failed; see /tmp/evgrpc_build.log"; exit 1; }
  fi
  if (( DO_COVERAGE )); then
    if [[ ! -f "$COVERAGE_BUILD_DIR/CMakeCache.txt" ]] \
       || ! grep -q "EVGRPC_COVERAGE:BOOL=ON" "$COVERAGE_BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
      echo ">>> Configuring $COVERAGE_BUILD_DIR with coverage (first run)..."
      cmake -G Ninja -S "$REPO_ROOT" -B "$COVERAGE_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug -DEVGRPC_COVERAGE=ON \
        >> /tmp/evgrpc_build.log 2>&1 \
        || { echo "coverage configure failed; see /tmp/evgrpc_build.log"; exit 1; }
    fi
  fi

  echo ">>> Building binaries..."
  cmake --build "$BUILD_DIR" \
    --target evgrpc_tests evgrpc_integration_tests evgrpc_e2e_tests -j4 \
    >> /tmp/evgrpc_build.log 2>&1 \
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

# Centralized log directory (commit 19d2c6c). Tests throw if ./log/
# is missing — create it before any suite runs.
mkdir -p "$REPO_ROOT/log"

# 1. Unit tests.
run_suite "unit (evgrpc_tests, ~99 cases)" \
  "./cmake-build-debug/tests/evgrpc_tests"

# 2. Integration tests.
run_suite "integration (evgrpc_integration_tests, 95 cases)" \
  "./cmake-build-debug/tests/integration/evgrpc_integration_tests"

# 3. E2E tests.
run_suite "e2e (evgrpc_e2e_tests, 2 cases)" \
  "./cmake-build-debug/tests/integration/evgrpc_e2e_tests"

# 4. Python gRPC integration tests (pytest, ~129 cases).
# Assumes `evgrpc-tests` conda env exists (created via `conda env create
# -f environment.yml`). Hits the docker-compose stack via the Bearer-token
# helper; namespace prefix isolates test rows. Auto-skips if IdP or
# docker-compose unreachable.
# Note: locate conda on PATH (macOS/Linux default installer puts it at
# /home/$USER/anaconda3 OR /home/$USER/.local/anaconda3 OR /opt/conda).
CONDA_BIN=""
for candidate in \
  "$HOME/anaconda3/bin/conda" \
  "$HOME/.local/anaconda3/bin/conda" \
  "/opt/conda/bin/conda" \
  "/usr/local/anaconda3/bin/conda"; do
  if [[ -x "$candidate" ]]; then CONDA_BIN="$candidate"; break; fi
done
if [[ -z "$CONDA_BIN" ]] && command -v conda >/dev/null; then
  CONDA_BIN="$(command -v conda)"
fi
if [[ -n "$CONDA_BIN" ]]; then
  run_suite "python gRPC IT (pytest, ~129 cases)" \
    "$CONDA_BIN run -n evgrpc-tests pytest tests/python/ --tb=short -q"
else
  echo ">>> Skipping python gRPC IT: conda not found on PATH."
  echo "    Set CONDA_BIN or install conda + create evgrpc-tests env."
  RESULTS+=("SKIP  python gRPC IT  (conda not found)")
fi

# 5. Coverage gate (optional — needs DATABASE_URL).
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

# Log artifacts — all logs centralized to repo-root ./log/.
echo
echo "------------------------------------------------------------"
echo "  Logs (./log/ — centralized location)"
echo "------------------------------------------------------------"
if [[ -d "$REPO_ROOT/log" ]]; then
  find "$REPO_ROOT/log" -type f -printf "  %p  (%s bytes)\n" 2>/dev/null | sort
  total=$(find "$REPO_ROOT/log" -type f -printf '%s\n' 2>/dev/null | awk '{s+=$1} END {print s+0}')
  count=$(find "$REPO_ROOT/log" -type f 2>/dev/null | wc -l)
  echo "  ---"
  echo "  ${count} file(s), ${total} bytes total"
else
  echo "  (no ./log/ directory — tests didn't write any file-sink logs)"
fi

# Exit non-zero if any suite failed (not skipped).
for r in "${RESULTS[@]}"; do
  if [[ "$r" == FAIL* ]]; then
    exit 1
  fi
done
exit 0