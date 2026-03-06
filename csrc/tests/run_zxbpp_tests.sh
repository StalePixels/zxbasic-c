#!/bin/bash
#
# Test harness for the zxbpp C preprocessor.
#
# Usage: run_zxbpp_tests.sh <zxbpp-binary> <test-dir>
#
# For each .bi file in test-dir that has a matching .out file,
# runs zxbpp on the .bi and diffs the output against the .out.
#
# Exit code: 0 if all tests pass, 1 if any fail.

set -euo pipefail

ZXBPP="${1:?Usage: $0 <zxbpp-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <zxbpp-binary> <test-dir>}"

PASS=0
FAIL=0
SKIP=0
ERRORS=""

# Normalize paths
ZXBPP=$(cd "$(dirname "$ZXBPP")" && echo "$(pwd)/$(basename "$ZXBPP")")
TEST_DIR=$(cd "$TEST_DIR" && pwd)

# Find project root (where src/lib exists)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    if [ -d "$PROJECT_ROOT/src/lib" ]; then
        break
    fi
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

# Build include paths for standard library
INCLUDE_ARGS=""
if [ -d "$PROJECT_ROOT/src/lib/arch/zx48k/stdlib" ]; then
    INCLUDE_ARGS="-I $PROJECT_ROOT/src/lib/arch/zx48k/stdlib"
fi

cd "$TEST_DIR"

for bi_file in *.bi; do
    test_name="${bi_file%.bi}"
    out_file="${test_name}.out"

    err_file="${test_name}.err"

    if [ -f "$out_file" ]; then
        # Normal test: compare stdout
        actual=$(mktemp /tmp/zxbpp_test_XXXXXX)

        if "$ZXBPP" "$bi_file" -o "$actual" -e /dev/null $INCLUDE_ARGS 2>/dev/null; then
            if diff -u \
                <(sed 's/[[:space:]]*$//' "$out_file" | grep -v '^$') \
                <(sed "s|${PROJECT_ROOT}|/zxbasic|g" "$actual" | sed 's/[[:space:]]*$//' | grep -v '^$') \
                > /dev/null 2>&1; then
                PASS=$((PASS + 1))
            else
                FAIL=$((FAIL + 1))
                ERRORS="${ERRORS}FAIL: ${test_name}\n"
                echo "--- FAIL: ${test_name} ---"
                diff -u \
                    <(sed 's/[[:space:]]*$//' "$out_file" | grep -v '^$') \
                    <(sed "s|${PROJECT_ROOT}|/zxbasic|g" "$actual" | sed 's/[[:space:]]*$//' | grep -v '^$') \
                    || true
                echo ""
            fi
        else
            FAIL=$((FAIL + 1))
            ERRORS="${ERRORS}FAIL: ${test_name} (zxbpp returned error)\n"
        fi

        rm -f "$actual"

    elif [ -f "$err_file" ]; then
        # Error test: expect non-zero exit and matching stderr
        actual_err=$(mktemp /tmp/zxbpp_test_err_XXXXXX)

        if "$ZXBPP" "$bi_file" -e /dev/null $INCLUDE_ARGS 2>"$actual_err" >/dev/null; then
            FAIL=$((FAIL + 1))
            ERRORS="${ERRORS}FAIL: ${test_name} (expected error, got success)\n"
        else
            PASS=$((PASS + 1))
        fi

        rm -f "$actual_err"

    else
        SKIP=$((SKIP + 1))
    fi
done

echo "=============================="
echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
echo "=============================="

if [ -n "$ERRORS" ]; then
    echo ""
    echo "Failed tests:"
    echo -e "$ERRORS"
fi

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

exit 0
