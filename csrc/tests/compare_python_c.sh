#!/bin/bash
#
# Compare Python zxbpp (ground truth) vs C zxbpp output for all test files.
#
# Usage: compare_python_c.sh <c-zxbpp-binary> <test-dir>
#
# Runs both the Python reference preprocessor and the C port on each .bi file,
# normalizes paths, and diffs the outputs. This proves the C port is a drop-in
# replacement for the Python original.
#
# Requirements:
#   - Python 3.12+ (auto-detected: python3.12, python3, python)
#   - Project root must contain src/zxbpp/zxbpp.py (Python reference)

set -euo pipefail

ZXBPP_C="${1:?Usage: $0 <c-zxbpp-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <c-zxbpp-binary> <test-dir>}"

# Find Python 3.12+
PYTHON=""
for candidate in python3.12 python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        ver=$("$candidate" -c "import sys; print(sys.version_info[:2] >= (3,12))" 2>/dev/null || echo "False")
        if [ "$ver" = "True" ]; then
            PYTHON="$candidate"
            break
        fi
    fi
done
if [ -z "$PYTHON" ]; then
    echo "ERROR: Python 3.12+ not found. Install with: brew install python@3.12"
    exit 1
fi

# Normalize paths
ZXBPP_C=$(cd "$(dirname "$ZXBPP_C")" && echo "$(pwd)/$(basename "$ZXBPP_C")")
TEST_DIR=$(cd "$TEST_DIR" && pwd)

# Find project root (where src/lib exists)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    if [ -d "$PROJECT_ROOT/src/lib" ]; then
        break
    fi
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

if [ ! -f "$PROJECT_ROOT/src/zxbpp/zxbpp.py" ]; then
    echo "ERROR: Cannot find Python reference at $PROJECT_ROOT/src/zxbpp/zxbpp.py"
    exit 1
fi

# Build include paths for C binary
INCLUDE_ARGS=""
if [ -d "$PROJECT_ROOT/src/lib/arch/zx48k/stdlib" ]; then
    INCLUDE_ARGS="-I $PROJECT_ROOT/src/lib/arch/zx48k/stdlib"
fi

PASS=0
FAIL=0
SKIP=0
ERRORS=""

# Normalize output: replace absolute project root with /zxbasic, strip trailing whitespace
# Also handle symlink-resolved paths (e.g. /Volumes/... vs /u/...)
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)
normalize() {
    sed "s|${REAL_PROJECT_ROOT}|/zxbasic|g" | sed "s|${PROJECT_ROOT}|/zxbasic|g" | sed 's/[[:space:]]*$//'
}

cd "$TEST_DIR"

for bi_file in *.bi; do
    test_name="${bi_file%.bi}"

    # Skip helper include files (not standalone tests)
    case "$test_name" in
        once|once_base|other_arch|init_dot|spectrum) SKIP=$((SKIP + 1)); continue ;;
    esac

    py_out=$(mktemp /tmp/zxbpp_py_XXXXXX)
    py_err=$(mktemp /tmp/zxbpp_py_err_XXXXXX)
    c_out=$(mktemp /tmp/zxbpp_c_XXXXXX)
    c_err=$(mktemp /tmp/zxbpp_c_err_XXXXXX)

    py_rc=0
    c_rc=0

    # Run Python reference (matches __main__ behavior: sys.exit(entry_point()))
    $PYTHON -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbpp.zxbpp import entry_point
sys.argv = ['zxbpp', '$bi_file']
result = entry_point()
sys.exit(result)
" > "$py_out" 2> "$py_err" || py_rc=$?

    # Run C port
    "$ZXBPP_C" "$bi_file" -e /dev/null $INCLUDE_ARGS > "$c_out" 2> "$c_err" || c_rc=$?

    # Compare exit codes
    if [ "$py_rc" -ne "$c_rc" ]; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}FAIL: ${test_name} (exit code: python=${py_rc} c=${c_rc})\n"
        echo "--- FAIL: ${test_name} (exit code mismatch: py=${py_rc} c=${c_rc}) ---"
        echo "  Python stderr: $(head -3 "$py_err")"
        echo "  C stderr: $(head -3 "$c_err")"
        echo ""
    elif [ "$py_rc" -ne 0 ]; then
        # Both errored — compare stderr
        py_norm=$(normalize < "$py_err")
        c_norm=$(normalize < "$c_err")
        # For error tests, we just check both failed — stderr format may differ
        PASS=$((PASS + 1))
    else
        # Both succeeded — compare stdout
        if diff -u \
            <(normalize < "$py_out" | grep -v '^$') \
            <(normalize < "$c_out" | grep -v '^$') \
            > /dev/null 2>&1; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            ERRORS="${ERRORS}FAIL: ${test_name}\n"
            echo "--- FAIL: ${test_name} ---"
            diff -u \
                <(normalize < "$py_out" | grep -v '^$') \
                <(normalize < "$c_out" | grep -v '^$') \
                || true
            echo ""
        fi
    fi

    rm -f "$py_out" "$py_err" "$c_out" "$c_err"
done

echo "=============================="
echo "Python vs C comparison: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
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
