#!/usr/bin/env bash
#
# run_zxbasm_tests.sh — Run zxbasm assembler tests
#
# Usage: run_zxbasm_tests.sh <zxbasm_binary> <test_dir>
#
# For each .asm file with a matching .bin in the test directory,
# assembles the .asm file and compares binary output against .bin.
# Files starting with "zxnext_" get the --zxnext flag.

set -euo pipefail

ZXBASM="${1:?Usage: $0 <zxbasm_binary> <test_dir>}"
TEST_DIR="${2:?Usage: $0 <zxbasm_binary> <test_dir>}"

# Resolve paths
ZXBASM="$(cd "$(dirname "$ZXBASM")" && pwd)/$(basename "$ZXBASM")"
TEST_DIR="$(cd "$TEST_DIR" && pwd)"

PASS=0
FAIL=0
SKIP=0
ERROR=0
TOTAL=0
FAILED_TESTS=""

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

for asm_file in "$TEST_DIR"/*.asm; do
    [ -f "$asm_file" ] || continue

    base=$(basename "$asm_file" .asm)
    expected="$TEST_DIR/${base}.bin"

    # Skip tests without expected output (error tests)
    if [ ! -f "$expected" ]; then
        SKIP=$((SKIP + 1))
        continue
    fi

    TOTAL=$((TOTAL + 1))
    actual="$TMPDIR/${base}.bin"

    # Build command
    OPTS="-d -e /dev/null -o $actual"
    if [[ "$base" == zxnext_* ]]; then
        OPTS="$OPTS --zxnext"
    fi

    # Run assembler
    if "$ZXBASM" $OPTS "$asm_file" </dev/null >/dev/null 2>&1; then
        # Compare binary output
        if cmp -s "$actual" "$expected"; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            FAILED_TESTS="$FAILED_TESTS  FAIL: $base (binary mismatch)\n"
            if command -v xxd >/dev/null 2>&1; then
                echo "--- FAIL: $base ---"
                echo "Expected (${expected}):"
                xxd "$expected" | head -5
                echo "Got (${actual}):"
                xxd "$actual" | head -5
                echo ""
            fi
        fi
    else
        # Assembler returned error but we expected success
        if [ -f "$actual" ] && cmp -s "$actual" "$expected"; then
            PASS=$((PASS + 1))
        else
            ERROR=$((ERROR + 1))
            FAILED_TESTS="$FAILED_TESTS  ERROR: $base (assembler failed)\n"
        fi
    fi
done

echo "========================================="
echo "zxbasm test results:"
echo "  PASS:    $PASS / $TOTAL"
echo "  FAIL:    $FAIL"
echo "  ERROR:   $ERROR"
echo "  SKIP:    $SKIP (no expected .bin)"
echo "========================================="

if [ -n "$FAILED_TESTS" ]; then
    echo ""
    echo "Failed tests:"
    echo -e "$FAILED_TESTS"
fi

if [ $FAIL -gt 0 ] || [ $ERROR -gt 0 ]; then
    exit 1
fi
exit 0
