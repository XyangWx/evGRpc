#!/usr/bin/env bash
# scripts/run_python_tests.sh — Activate the evgrpc-tests conda env and run
# all Python gRPC integration tests (tests/python/).
#
# This is a STANDALONE Python-only runner. For the full suite
# (C++ unit / integration / e2e + Python), use scripts/run_all_tests.sh.
#
# Prereqs:
#   - conda installed (auto-detected at standard locations, or set CONDA_BIN)
#   - `evgrpc-tests` env created via `conda env create -f environment.yml`
#     (override env name with CONDA_ENV env var)
#   - The evGRpc stack (docker-compose: nginx + evgrpc_server + Postgres)
#     running and reachable on localhost:80
#
# Usage:
#   ./scripts/run_python_tests.sh                       # run all (~130 cases)
#   ./scripts/run_python_tests.sh -v                    # verbose
#   ./scripts/run_python_tests.sh -k test_smoke         # filter by name
#   ./scripts/run_python_tests.sh test_smoke.py         # specific file
#   ./scripts/run_python_tests.sh --check-deps          # verify deps then exit
#   ./scripts/run_python_tests.sh -- tests/foo.py -v    # forward anything after --
#   CONDA_ENV=my-env ./scripts/run_python_tests.sh      # override env name
#
# Exits non-zero on any test failure or if conda/env is missing.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CONDA_ENV="${CONDA_ENV:-evgrpc-tests}"
PYTEST_EXTRA_ARGS=()
VERBOSE=0
CHECK_DEPS_ONLY=0

# --- locate conda -----------------------------------------------------------
CONDA_BIN="${CONDA_BIN:-}"
if [[ -z "$CONDA_BIN" ]]; then
  for candidate in \
    "$HOME/.local/anaconda3/bin/conda" \
    "$HOME/anaconda3/bin/conda" \
    "/opt/conda/bin/conda" \
    "/usr/local/anaconda3/bin/conda"; do
    if [[ -x "$candidate" ]]; then
      CONDA_BIN="$candidate"
      break
    fi
  done
fi
if [[ -z "$CONDA_BIN" ]] && command -v conda >/dev/null 2>&1; then
  CONDA_BIN="$(command -v conda)"
fi
if [[ -z "$CONDA_BIN" ]]; then
  cat >&2 <<EOF
ERROR: conda not found.

Searched:
  \$HOME/.local/anaconda3/bin/conda
  \$HOME/anaconda3/bin/conda
  /opt/conda/bin/conda
  /usr/local/anaconda3/bin/conda
  \$PATH (via 'command -v conda')

Fix one of:
  - Install Miniconda/Anaconda
  - Set CONDA_BIN=/path/to/conda explicitly
  - Put conda on PATH
EOF
  exit 1
fi

# --- source conda.sh so `conda activate` works in non-interactive shells ----
CONDA_BASE_DIR="$(cd "$(dirname "$CONDA_BIN")/.." && pwd)"
CONDA_SH="$CONDA_BASE_DIR/etc/profile.d/conda.sh"
if [[ ! -f "$CONDA_SH" ]]; then
  cat >&2 <<EOF
ERROR: conda.sh not found at:
  $CONDA_SH

'conda activate' requires sourcing conda.sh first. Your conda install
looks unusual — verify it's a standard Miniconda/Anaconda layout.
EOF
  exit 1
fi

# shellcheck source=/dev/null
source "$CONDA_SH"

# --- parse args -------------------------------------------------------------
print_help() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      print_help
      exit 0
      ;;
    -v|--verbose)
      VERBOSE=1
      shift
      ;;
    --check-deps)
      CHECK_DEPS_ONLY=1
      shift
      ;;
    --)
      shift
      PYTEST_EXTRA_ARGS=("$@")
      break
      ;;
    *)
      # Anything else is forwarded to pytest as-is (file path, -k, etc.).
      PYTEST_EXTRA_ARGS+=("$1")
      shift
      ;;
  esac
done

# --- activate env -----------------------------------------------------------
echo ">>> conda: $CONDA_BIN"
echo ">>> env:   $CONDA_ENV"

# `conda activate` returns non-zero if the env doesn't exist. Check first
# to give a clearer error.
if ! "$CONDA_BIN" env list | grep -qE "^${CONDA_ENV}\s"; then
  cat >&2 <<EOF
ERROR: conda env '$CONDA_ENV' does not exist.

Create it:
  conda env create -f environment.yml

Or pick another env:
  CONDA_ENV=my-env $0
EOF
  exit 1
fi

# shellcheck disable=SC1090,SC1091
conda activate "$CONDA_ENV"

# --- verify the right python is now active ----------------------------------
PYTHON_BIN="$(command -v python)"
PY_VERSION="$(python -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])')"
echo ">>> python: $PYTHON_BIN (Python $PY_VERSION)"
case "$PYTHON_BIN" in
  *"$CONDA_ENV"*|*"$CONDA_BASE_DIR/envs/$CONDA_ENV"*) ;;
  *)
    cat >&2 <<EOF
ERROR: activated env '$CONDA_ENV' does not appear to be active.
  'which python' is: $PYTHON_BIN
  Expected: .../envs/$CONDA_ENV/bin/python

This usually means the env exists but 'conda activate' silently failed.
Try: conda activate $CONDA_ENV && which python
EOF
    conda deactivate >/dev/null 2>&1 || true
    exit 1
    ;;
esac

# --- optional: check that test deps are installed ---------------------------
if (( CHECK_DEPS_ONLY )) || [[ "${CHECK_DEPS:-0}" == "1" ]]; then
  echo
  echo ">>> Checking required test dependencies..."
  missing=()
  for pkg in pytest grpc psycopg jwt cryptography; do
    if ! python -c "import $pkg" 2>/dev/null; then
      missing+=("$pkg")
    fi
  done
  if (( ${#missing[@]} > 0 )); then
    cat >&2 <<EOF
ERROR: missing Python packages in env '$CONDA_ENV':
  ${missing[*]}

Reinstall:
  conda env update -f environment.yml --prune
EOF
    exit 1
  fi
  echo ">>> All required deps present (pytest, grpc, psycopg, jwt, cryptography)."
  if (( CHECK_DEPS_ONLY )); then
    exit 0
  fi
fi

# --- run pytest -------------------------------------------------------------
cd "$REPO_ROOT"

PYTEST_FLAGS=(--tb=short)
if (( VERBOSE )); then
  PYTEST_FLAGS+=(-v)
else
  PYTEST_FLAGS+=(-q)
fi

# Default test target (can be overridden by PYTEST_EXTRA_ARGS).
TEST_TARGET="tests/python/"
if (( ${#PYTEST_EXTRA_ARGS[@]} > 0 )); then
  # First arg that looks like a file path or test selector → use as target.
  # Otherwise append flags only.
  first="${PYTEST_EXTRA_ARGS[0]}"
  # Auto-resolve bare test_*.py names to tests/python/<name>.py (pytest.ini
  # sets testpaths=tests/python but only for default discovery).
  if [[ -e "$REPO_ROOT/$first" ]]; then
    TEST_TARGET="$first"
    PYTEST_FLAGS+=("${PYTEST_EXTRA_ARGS[@]:1}")
  elif [[ "$first" == test_*.py && -e "$REPO_ROOT/tests/python/$first" ]]; then
    TEST_TARGET="tests/python/$first"
    PYTEST_FLAGS+=("${PYTEST_EXTRA_ARGS[@]:1}")
  else
    PYTEST_FLAGS+=("${PYTEST_EXTRA_ARGS[@]}")
  fi
fi

echo
echo ">>> Running: pytest ${PYTEST_FLAGS[*]} $TEST_TARGET"
echo

# Run pytest. Don't `set -e` fail on non-zero — we want to always print a
# summary line. But exit non-zero on failure so callers can detect.
set +e
pytest "${PYTEST_FLAGS[@]}" "$TEST_TARGET"
PYTEST_RC=$?
set -e

echo
if (( PYTEST_RC == 0 )); then
  echo ">>> PASS: all Python tests in $TEST_TARGET"
else
  echo ">>> FAIL: pytest exited $PYTEST_RC (see output above)"
fi

# Best-effort cleanup (no error if already deactivated).
conda deactivate >/dev/null 2>&1 || true

exit $PYTEST_RC