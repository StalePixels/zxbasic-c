#!/usr/bin/env bash
# Strict zxbpp harness: Python at runtime is the oracle, not the .out fixture.
# No fuzzy preprocessing. Stdouts must cmp-identical after path normalisation.
#
# Usage: run_zxbpp_tests_strict.sh <c-zxbpp-binary> <test-dir>
#
# Replaces (eventually, in Sprint 11 / Phase 6) the fuzzy run_zxbpp_tests.sh
# whose trailing-whitespace-strip + empty-line-strip preprocessing masks
# 14 of 86 success tests' real divergences (per Sprint 1 catalog).
#
# This script ships in Sprint 2 alongside the existing run_zxbpp_tests.sh; the
# `make test-zxbpp-fuzzy` target keeps the old harness invokable for
# calibration-disagreement demonstration until Phase 6 deletes it.
#
# Phase 2 / Sprint 3 extends this script with the error-test branch (stderr
# content comparison after path normalisation). Sprint 2's wiring captures
# stderr to its own file but does not yet compare it.

set -uo pipefail

ZXBPP_C="${1:?Usage: $0 <c-zxbpp-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <c-zxbpp-binary> <test-dir>}"

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    echo "       Strict harness will not silently fall back to system python3." >&2
    exit 2
fi

ZXBPP_C=$(cd "$(dirname "$ZXBPP_C")" && pwd)/$(basename "$ZXBPP_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

INCLUDE_ARGS=""
[ -d "$PROJECT_ROOT/src/lib/arch/zx48k/stdlib" ] && \
    INCLUDE_ARGS="-I $PROJECT_ROOT/src/lib/arch/zx48k/stdlib"

PYTHON_INTERNAL_LOG="$PROJECT_ROOT/csrc/tests/phase1-python-internal-skips.log"
: > "$PYTHON_INTERNAL_LOG"

normalise() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); print }
    '
}

is_python_internal_error() {
    grep -qE '^Traceback \(most recent call last\):|^[[:space:]]*(KeyError|TypeError|AttributeError|IndexError|RecursionError|AssertionError):' "$1"
}

PASS=0
FAIL=0
SKIP_HELPER=0
SKIP_PYINTERNAL=0
declare -a FAIL_NAMES

cd "$TEST_DIR"

for bi in *.bi; do
    name=${bi%.bi}
    case "$name" in
        once|once_base|other_arch|init_dot|spectrum)
            SKIP_HELPER=$((SKIP_HELPER + 1)); continue
            ;;
    esac
    # Error tests are Sprint 3 territory; skip here until that branch lands.
    if [ -f "${name}.err" ] && [ ! -f "${name}.out" ]; then
        continue
    fi

    py_out=$(mktemp); py_err=$(mktemp)
    c_out=$(mktemp);  c_err=$(mktemp)

    "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbpp.zxbpp import entry_point
sys.argv = ['zxbpp', '$bi']
sys.exit(entry_point() or 0)
" > "$py_out" 2> "$py_err" || true

    "$ZXBPP_C" "$bi" -e /dev/null $INCLUDE_ARGS > "$c_out" 2> "$c_err" || true

    if is_python_internal_error "$py_err"; then
        SKIP_PYINTERNAL=$((SKIP_PYINTERNAL + 1))
        echo "$name :: $(head -1 "$py_err")" >> "$PYTHON_INTERNAL_LOG"
        rm -f "$py_out" "$py_err" "$c_out" "$c_err"
        continue
    fi

    py_norm=$(mktemp); c_norm=$(mktemp)
    normalise < "$py_out" > "$py_norm"
    normalise < "$c_out"  > "$c_norm"

    if cmp -s "$py_norm" "$c_norm"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAIL_NAMES+=("$name")
        echo "--- FAIL: $name ---"
        diff -u "$py_norm" "$c_norm"
        echo
    fi

    rm -f "$py_out" "$py_err" "$c_out" "$c_err" "$py_norm" "$c_norm"
done

echo "=========================================="
echo "zxbpp strict harness:"
echo "  PASS:                 $PASS"
echo "  FAIL:                 $FAIL"
echo "  SKIP (helper):        $SKIP_HELPER"
echo "  SKIP (py-internal):   $SKIP_PYINTERNAL"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "Failed tests:"
    printf '  %s\n' "${FAIL_NAMES[@]}"
    exit 1
fi
exit 0
