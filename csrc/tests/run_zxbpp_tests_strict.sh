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
# Sprint 3 (Phase 2) adds the error-test branch: per .bi with sibling .err
# and no .out, compare normalised stderrs strictly. Python's live stderr
# is the oracle; the .err fixture is checked as a sanity-reference (mismatch
# flagged as stale for Phase 6 reconciliation, no regen here).

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

# Stderr normalisation: path subst (as above) plus trailing-whitespace strip
# per line. Empty lines and line ordering are preserved. Per plan §2.
normalise_stderr() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); sub(/[ \t]+$/, ""); print }
    '
}

STALE_ERR_LOG="$PROJECT_ROOT/csrc/tests/phase2-stale-err-fixtures.log"
: > "$STALE_ERR_LOG"

is_python_internal_error() {
    grep -qE '^Traceback \(most recent call last\):|^[[:space:]]*(KeyError|TypeError|AttributeError|IndexError|RecursionError|AssertionError):' "$1"
}

PASS=0
FAIL=0
ERR_PASS=0
ERR_FAIL=0
SKIP_HELPER=0
SKIP_PYINTERNAL=0
declare -a FAIL_NAMES
declare -a ERR_FAIL_NAMES

cd "$TEST_DIR"

for bi in *.bi; do
    name=${bi%.bi}
    case "$name" in
        once|once_base|other_arch|init_dot|spectrum)
            SKIP_HELPER=$((SKIP_HELPER + 1)); continue
            ;;
    esac
    is_error_test=0
    if [ -f "${name}.err" ] && [ ! -f "${name}.out" ]; then
        is_error_test=1
    fi

    py_out=$(mktemp); py_err=$(mktemp)
    c_out=$(mktemp);  c_err=$(mktemp)
    py_rc=0; c_rc=0

    "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbpp.zxbpp import entry_point
sys.argv = ['zxbpp', '$bi']
sys.exit(entry_point() or 0)
" > "$py_out" 2> "$py_err" || py_rc=$?

    # Note: no -e /dev/null here — the strict harness needs C's actual
    # stderr to compare against Python's. The fuzzy harness suppressed
    # stderr and only checked exit code, which is exactly the loophole
    # this rebuild closes (test-infra-trust.md:24).
    "$ZXBPP_C" "$bi" $INCLUDE_ARGS > "$c_out" 2> "$c_err" || c_rc=$?

    if is_python_internal_error "$py_err"; then
        SKIP_PYINTERNAL=$((SKIP_PYINTERNAL + 1))
        echo "$name :: $(head -1 "$py_err")" >> "$PYTHON_INTERNAL_LOG"
        rm -f "$py_out" "$py_err" "$c_out" "$c_err"
        continue
    fi

    if [ "$is_error_test" -eq 1 ]; then
        # Error test: both must exit non-zero AND normalised stderrs match.
        py_err_norm=$(mktemp); c_err_norm=$(mktemp)
        normalise_stderr < "$py_err" > "$py_err_norm"
        normalise_stderr < "$c_err"  > "$c_err_norm"

        # Sanity-reference: flag stale .err fixture if Python's live
        # stderr no longer matches the static .err. The static fixture
        # is bare (no path:line prefix), so compare on the message tail.
        py_tail=$(tail -1 "$py_err_norm" | sed -E 's/^[^:]*:[0-9]+: error: //')
        fixture_msg=$(head -1 "${name}.err" | awk '{ sub(/[ \t]+$/, ""); print }')
        if [ "$py_tail" != "$fixture_msg" ]; then
            echo "$name :: py='$py_tail' vs fixture='$fixture_msg'" >> "$STALE_ERR_LOG"
        fi

        if [ "$py_rc" -eq 0 ] || [ "$c_rc" -eq 0 ]; then
            ERR_FAIL=$((ERR_FAIL + 1))
            ERR_FAIL_NAMES+=("$name")
            echo "--- ERR-FAIL: $name (exit codes: py=$py_rc c=$c_rc — at least one zero) ---"
        elif cmp -s "$py_err_norm" "$c_err_norm"; then
            ERR_PASS=$((ERR_PASS + 1))
        else
            ERR_FAIL=$((ERR_FAIL + 1))
            ERR_FAIL_NAMES+=("$name")
            echo "--- ERR-FAIL: $name (stderr mismatch) ---"
            diff -u "$py_err_norm" "$c_err_norm"
            echo
        fi

        rm -f "$py_out" "$py_err" "$c_out" "$c_err" "$py_err_norm" "$c_err_norm"
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
echo "  Success-test PASS:    $PASS"
echo "  Success-test FAIL:    $FAIL"
echo "  Error-test PASS:      $ERR_PASS"
echo "  Error-test FAIL:      $ERR_FAIL"
echo "  SKIP (helper):        $SKIP_HELPER"
echo "  SKIP (py-internal):   $SKIP_PYINTERNAL"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "Failed success tests:"
    printf '  %s\n' "${FAIL_NAMES[@]}"
fi
if [ "$ERR_FAIL" -gt 0 ]; then
    echo "Failed error tests:"
    printf '  %s\n' "${ERR_FAIL_NAMES[@]}"
fi

[ -s "$STALE_ERR_LOG" ] && echo "Stale .err fixtures (recorded for Phase 6): $(wc -l < "$STALE_ERR_LOG")"

if [ "$FAIL" -gt 0 ] || [ "$ERR_FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
