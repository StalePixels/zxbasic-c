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

    # Skip tests without expected output (error tests)
    if [ ! -f "$out_file" ]; then
        SKIP=$((SKIP + 1))
        continue
    fi

    # Create temp file for actual output
    actual=$(mktemp /tmp/zxbpp_test_XXXXXX)

    # Run zxbpp with include paths
    if "$ZXBPP" "$bi_file" -o "$actual" -e /dev/null $INCLUDE_ARGS 2>/dev/null; then
        # Normalize paths in both expected and actual output:
        # Replace project-root paths with /zxbasic and vice versa
        if diff -u \
            <(sed 's/[[:space:]]*$//' "$out_file" | grep -v '^$') \
            <(sed "s|${PROJECT_ROOT}|/zxbasic|g" "$actual" | sed 's/[[:space:]]*$//' | grep -v '^$') \
            > /dev/null 2>&1; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            ERRORS="${ERRORS}FAIL: ${test_name}\n"
            # Show diff for debugging
            echo "--- FAIL: ${test_name} ---"
            diff -u \
                <(sed 's/[[:space:]]*$//' "$out_file" | grep -v '^$') \
                <(sed "s|${PROJECT_ROOT}|/zxbasic|g" "$actual" | sed 's/[[:space:]]*$//' | grep -v '^$') \
                || true
            echo ""
        fi
    else
        # zxbpp returned error — check if this is expected (no .out = error test)
        # Since we already filtered for .out existence, this is an unexpected failure
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}FAIL: ${test_name} (zxbpp returned error)\n"
    fi

    rm -f "$actual"
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
