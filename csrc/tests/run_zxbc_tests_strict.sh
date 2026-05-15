#!/usr/bin/env bash
# Strict zxbc parse-only harness: four-bucket classification.
#
# Compares C zxbc --parse-only against the cached Python baselines that
# Sprint 5's regen script produced under csrc/tests/zxbc_parse_expected/.
# Closes the existing run_zxbc_tests.sh's exit-code-only loophole by
# also checking that, when both interpreters exit non-zero, their
# normalised stderrs match.
#
# Buckets:
#   PASS             — exit codes match AND, if both non-zero, stderrs match
#   FALSE_POS        — C exits non-zero, Python exits zero
#   FALSE_NEG        — C exits zero, Python exits non-zero
#   STDERR_MISMATCH  — both exit non-zero, but normalised stderrs differ
#
# Skips:
#   SKIP — Python-internal-error    (.err.expected matches a traceback)
#   SKIP — known-Python-bug         (test stem listed in zxbc_python_bugs.txt)
#
# Usage: run_zxbc_tests_strict.sh <c-zxbc-binary> <bas-test-dir> [file.bas ...]

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc-binary> <bas-test-dir> [file.bas ...]}"
TEST_DIR="${2:?Usage: $0 <c-zxbc-binary> <bas-test-dir> [file.bas ...]}"
shift 2

ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

EXPECTED_DIR="$PROJECT_ROOT/csrc/tests/zxbc_parse_expected"
PYTHON_BUGS="$PROJECT_ROOT/csrc/tests/zxbc_python_bugs.txt"

if [ ! -d "$EXPECTED_DIR" ]; then
    echo "ERROR: $EXPECTED_DIR missing — run 'make regenerate-zxbc-baselines' first." >&2
    exit 2
fi

# Stderr normalisation: same policy as zxbpp/zxbasm strict harnesses.
normalise_stderr() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); sub(/[ \t]+$/, ""); print }
    '
}

is_python_internal_error() {
    grep -qE '^Traceback \(most recent call last\):|^[[:space:]]*(KeyError|TypeError|AttributeError|IndexError|RecursionError|AssertionError):' "$1"
}

# Materialise the python-bugs stem list once into a temp file (macOS
# bash 3.2 doesn't support associative arrays, so we use grep -Fxq).
PY_BUGS_FILE=$(mktemp)
trap 'rm -f "$PY_BUGS_FILE"' EXIT
if [ -f "$PYTHON_BUGS" ]; then
    sed 's/#.*//' "$PYTHON_BUGS" | awk '$1 != "" { print $1 }' > "$PY_BUGS_FILE"
fi
is_known_python_bug() {
    grep -Fxq "$1" "$PY_BUGS_FILE"
}

PASS=0
FALSE_POS=0
FALSE_NEG=0
STDERR_MISMATCH=0
SKIP_PY_INTERNAL=0
SKIP_KNOWN_PY_BUG=0
declare -a FALSE_POS_NAMES
declare -a FALSE_NEG_NAMES
declare -a STDERR_MISMATCH_NAMES

cd "$PROJECT_ROOT"

if [ "$#" -gt 0 ]; then
    BAS_FILES=("$@")
else
    BAS_FILES=("$TEST_DIR"/*.bas)
fi

for bas in "${BAS_FILES[@]}"; do
    stem=$(basename "$bas" .bas)
    rc_file="$EXPECTED_DIR/${stem}.rc"
    err_expected="$EXPECTED_DIR/${stem}.err.expected"

    if [ ! -f "$rc_file" ]; then
        echo "SKIP: $stem — no baseline .rc (regen needed?)" >&2
        continue
    fi
    expected_rc=$(cat "$rc_file" | tr -d '[:space:]')

    if is_known_python_bug "$stem"; then
        SKIP_KNOWN_PY_BUG=$((SKIP_KNOWN_PY_BUG + 1))
        continue
    fi

    # Python-internal-error detection on the captured stderr fixture.
    if [ -f "$err_expected" ] && is_python_internal_error "$err_expected"; then
        SKIP_PY_INTERNAL=$((SKIP_PY_INTERNAL + 1))
        continue
    fi

    c_err=$(mktemp)
    c_rc=0
    "$ZXBC_C" --parse-only "$bas" >/dev/null 2> "$c_err" || c_rc=$?

    if [ "$expected_rc" = "0" ] && [ "$c_rc" = "0" ]; then
        PASS=$((PASS + 1))
    elif [ "$expected_rc" = "0" ] && [ "$c_rc" != "0" ]; then
        FALSE_POS=$((FALSE_POS + 1))
        FALSE_POS_NAMES+=("$stem")
    elif [ "$expected_rc" != "0" ] && [ "$c_rc" = "0" ]; then
        FALSE_NEG=$((FALSE_NEG + 1))
        FALSE_NEG_NAMES+=("$stem")
    else
        # Both non-zero. Compare normalised stderrs strictly.
        if [ -f "$err_expected" ]; then
            c_norm=$(mktemp)
            normalise_stderr < "$c_err" > "$c_norm"
            if cmp -s "$err_expected" "$c_norm"; then
                PASS=$((PASS + 1))
            else
                STDERR_MISMATCH=$((STDERR_MISMATCH + 1))
                STDERR_MISMATCH_NAMES+=("$stem")
            fi
            rm -f "$c_norm"
        else
            # Baseline says non-zero but no stderr captured. Treat as STDERR_MISMATCH
            # since we can't verify what Python reported.
            STDERR_MISMATCH=$((STDERR_MISMATCH + 1))
            STDERR_MISMATCH_NAMES+=("$stem")
        fi
    fi

    rm -f "$c_err"
done

TOTAL=$((PASS + FALSE_POS + FALSE_NEG + STDERR_MISMATCH))

echo "=========================================="
echo "zxbc --parse-only strict harness:"
echo "  PASS:                       $PASS"
echo "  FALSE_POS (C err, Py ok):   $FALSE_POS"
echo "  FALSE_NEG (C ok, Py err):   $FALSE_NEG"
echo "  STDERR_MISMATCH:            $STDERR_MISMATCH"
echo "  SKIP (known Python bug):    $SKIP_KNOWN_PY_BUG"
echo "  SKIP (Python internal err): $SKIP_PY_INTERNAL"
echo "  --"
echo "  Total scored:               $TOTAL"
echo "=========================================="

if [ "$FALSE_POS" -gt 0 ]; then
    echo "FALSE_POS:"; printf '  %s\n' "${FALSE_POS_NAMES[@]}"
fi
if [ "$FALSE_NEG" -gt 0 ]; then
    echo "FALSE_NEG (first 20):"; printf '  %s\n' "${FALSE_NEG_NAMES[@]}" | head -20
fi
if [ "$STDERR_MISMATCH" -gt 0 ]; then
    echo "STDERR_MISMATCH (first 20):"; printf '  %s\n' "${STDERR_MISMATCH_NAMES[@]}" | head -20
fi

if [ "$FALSE_POS" -gt 0 ] || [ "$FALSE_NEG" -gt 0 ] || [ "$STDERR_MISMATCH" -gt 0 ]; then
    exit 1
fi
exit 0
