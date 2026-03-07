#!/bin/bash
#
# Compare Python zxbasm (ground truth) vs C zxbasm output for all test files.
#
# Usage: compare_python_c_asm.sh <c-zxbasm-binary> <test-dir>
#
# Runs both the Python reference assembler and the C port on each .asm file,
# and diffs the binary outputs. This proves the C port is a drop-in
# replacement for the Python original.
#
# Requirements:
#   - Python 3.12+ (auto-detected: python3.12, python3, python)
#   - Project root must contain src/zxbasm/ (Python reference)

set -euo pipefail

ZXBASM_C="${1:?Usage: $0 <c-zxbasm-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <c-zxbasm-binary> <test-dir>}"

# Find Python 3.11+
PYTHON=""
for candidate in python3.12 python3.11 python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        ver=$("$candidate" -c "import sys; print(sys.version_info[:2] >= (3,11))" 2>/dev/null || echo "False")
        if [ "$ver" = "True" ]; then
            PYTHON="$candidate"
            break
        fi
    fi
done
if [ -z "$PYTHON" ]; then
    echo "ERROR: Python 3.11+ not found."
    exit 1
fi

# Normalize paths
ZXBASM_C=$(cd "$(dirname "$ZXBASM_C")" && echo "$(pwd)/$(basename "$ZXBASM_C")")
TEST_DIR=$(cd "$TEST_DIR" && pwd)

# Find project root (where src/lib exists)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    if [ -d "$PROJECT_ROOT/src/lib" ]; then
        break
    fi
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

if [ ! -d "$PROJECT_ROOT/src/zxbasm" ]; then
    echo "ERROR: Cannot find Python reference at $PROJECT_ROOT/src/zxbasm/"
    exit 1
fi

PASS=0
FAIL=0
SKIP=0
ERRORS=""

cd "$TEST_DIR"

for asm_file in *.asm; do
    test_name="${asm_file%.asm}"

    # Only test files that have expected .bin output
    if [ ! -f "${test_name}.bin" ]; then
        SKIP=$((SKIP + 1))
        continue
    fi

    py_out=$(mktemp /tmp/zxbasm_py_XXXXXX.bin)
    c_out=$(mktemp /tmp/zxbasm_c_XXXXXX.bin)
    py_err=$(mktemp /tmp/zxbasm_py_err_XXXXXX)
    c_err=$(mktemp /tmp/zxbasm_c_err_XXXXXX)

    py_rc=0
    c_rc=0

    # Run Python reference
    $PYTHON -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbasm.zxbasm import main as entry_point
sys.argv = ['zxbasm', '-d', '-e', '/dev/null', '-o', '$py_out', '$asm_file']
result = entry_point()
sys.exit(result)
" > /dev/null 2> "$py_err" || py_rc=$?

    # Run C port
    "$ZXBASM_C" -d -e /dev/null -o "$c_out" "$asm_file" > /dev/null 2> "$c_err" || c_rc=$?

    # Compare binary outputs
    if [ "$py_rc" -ne 0 ] && [ "$c_rc" -ne 0 ]; then
        # Both errored — OK
        PASS=$((PASS + 1))
    elif [ "$py_rc" -ne "$c_rc" ]; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}FAIL: ${test_name} (exit code: python=${py_rc} c=${c_rc})\n"
        echo "--- FAIL: ${test_name} (exit code mismatch: py=${py_rc} c=${c_rc}) ---"
    elif diff "$py_out" "$c_out" > /dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}FAIL: ${test_name} (binary mismatch)\n"
        echo "--- FAIL: ${test_name} ---"
        echo "  Python output:"
        xxd "$py_out" | head -5
        echo "  C output:"
        xxd "$c_out" | head -5
        echo ""
    fi

    rm -f "$py_out" "$c_out" "$py_err" "$c_err"
done

echo "=============================="
echo "Python vs C comparison (zxbasm): ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
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
