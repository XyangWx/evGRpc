#!/usr/bin/env bash
# Pre-commit hook for evGRpc.
#
# Install:
#   ln -s ../../scripts/pre-commit.sh .git/hooks/pre-commit
#   (or copy into .git/hooks/pre-commit)
#
# What it does:
#   1. clang-format check on staged .cc/.h files (auto-fix if -f flag)
#   2. Python syntax check on staged .py files
#   3. JSON validation on config files
#   4. Shellcheck on scripts/*.sh
#   5. Trailing whitespace check
#
# Behavior: returns non-zero on any failure, blocking the commit.
# Override with --no-verify to skip.

set -euo pipefail

# Find the repo root.
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

# Color output (skip if not a TTY).
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[0;33m'
    NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; NC=''
fi

fail() { echo -e "${RED}FAIL: $*${NC}" >&2; exit 1; }
warn() { echo -e "${YELLOW}WARN: $*${NC}" >&2; }
pass() { echo -e "${GREEN}OK: $*${NC}"; }

# 1. clang-format on staged .cc/.h files
STAGED_CCFILES=$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(cc|h)$' || true)
if [ -n "$STAGED_CCFILES" ]; then
    if command -v clang-format >/dev/null 2>&1; then
        echo "==> clang-format check on $(echo "$STAGED_CCFILES" | wc -l) file(s)"
        NEEDS_FORMAT=""
        for f in $STAGED_CCFILES; do
            if [ -f "$f" ]; then
                # Check if the file would be reformatted.
                if ! clang-format --style=file --dry-run --Werror "$f" >/dev/null 2>&1; then
                    NEEDS_FORMAT="$NEEDS_FORMAT $f"
                fi
            fi
        done
        if [ -n "$NEEDS_FORMAT" ]; then
            echo "  Files that need reformatting:"
            for f in $NEEDS_FORMAT; do
                echo "    $f"
            done
            echo "  Run: clang-format -i --style=file <files>"
            fail "clang-format check failed"
        fi
        pass "clang-format"
    else
        warn "clang-format not installed; skipping C++ format check"
        warn "  Install: apt install clang-format (Ubuntu) or brew install clang-format (macOS)"
    fi
fi

# 2. Python syntax check on staged .py files
STAGED_PYFILES=$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.py$' || true)
if [ -n "$STAGED_PYFILES" ]; then
    echo "==> Python syntax check on $(echo "$STAGED_PYFILES" | wc -l) file(s)"
    for f in $STAGED_PYFILES; do
        if [ -f "$f" ]; then
            if ! python3 -m py_compile "$f" 2>&1; then
                fail "Python syntax error in $f"
            fi
        fi
    done
    pass "Python syntax"
fi

# 3. JSON validation on staged .json files
STAGED_JSONFILES=$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.json$' || true)
if [ -n "$STAGED_JSONFILES" ]; then
    echo "==> JSON validation on $(echo "$STAGED_JSONFILES" | wc -l) file(s)"
    for f in $STAGED_JSONFILES; do
        if [ -f "$f" ]; then
            if ! python3 -m json.tool "$f" >/dev/null 2>&1; then
                fail "JSON parse error in $f"
            fi
        fi
    done
    pass "JSON"
fi

# 4. shellcheck on staged .sh files (in scripts/ or .git/hooks/)
STAGED_SHFILES=$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.sh$' || true)
if [ -n "$STAGED_SHFILES" ]; then
    if command -v shellcheck >/dev/null 2>&1; then
        echo "==> shellcheck on $(echo "$STAGED_SHFILES" | wc -l) file(s)"
        for f in $STAGED_SHFILES; do
            if [ -f "$f" ]; then
                if ! shellcheck "$f" 2>&1; then
                    fail "shellcheck failed on $f"
                fi
            fi
        done
        pass "shellcheck"
    else
        warn "shellcheck not installed; skipping shellcheck"
    fi
fi

# 5. Trailing whitespace check
STAGED_ALL=$(git diff --cached --name-only --diff-filter=ACMR || true)
if [ -n "$STAGED_ALL" ]; then
    TRAILING_WS=""
    for f in $STAGED_ALL; do
        if [ -f "$f" ] && file "$f" | grep -q "text"; then
            # Check for trailing whitespace (space/tab on line end).
            if grep -nE ' +$' "$f" >/dev/null 2>&1; then
                TRAILING_WS="$TRAILING_WS $f"
            fi
        fi
    done
    if [ -n "$TRAILING_WS" ]; then
        echo "==> Files with trailing whitespace:"
        for f in $TRAILING_WS; do
            echo "    $f"
        done
        warn "trailing whitespace (use --no-verify to skip or fix with sed -i 's/[[:space:]]*$//')"
        # Don't fail — trailing whitespace is too common to block commits.
    fi
fi

# 6. Catch-all: codegen stubs up to date
# If tests/python/gen/ is staged, ensure the gen script was run.
STAGED_GEN=$(git diff --cached --name-only --diff-filter=ACMR | grep "tests/python/gen/" || true)
if [ -n "$STAGED_GEN" ]; then
    warn "gen/ files staged; ensure scripts/gen_python_stubs.sh was run after proto changes"
fi

echo "==> pre-commit checks passed"