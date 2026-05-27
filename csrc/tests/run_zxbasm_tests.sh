#!/usr/bin/env bash
# Strict zxbasm harness — Round 0 Phase 3.
#
# Closes the 0-byte cmp loophole in run_zxbasm_tests.sh: that harness
# used `cmp -s` of the assembler's binary output against the static
# .bin fixture, which silently passes whenever both files are 0-byte
# (asmerror0 in particular). The strict harness instead classifies
# each .asm by sibling fixtures into three buckets:
#
#   .err present, .bin absent (or both)        → error test
#       PASS iff Python and C exit codes agree AND normalised stderrs
#       strict-cmp identical (Phase 2 policy: PROJECT_ROOT substitution
#       + trailing-whitespace strip; preserve empty lines and ordering).
#       When .err exists, any sibling .bin is ignored.
#
#   .bin present, .err absent                  → success test
#       PASS iff zxbasm produces a binary that strict-cmps the .bin
#       fixture. Same as the existing harness for this branch — kept,
#       not loophole-bearing.
#
#   neither .err nor .bin                      → SKIP (missing fixture)
#       Logged to phase3-asm-missing-fixtures.log so Sprint 10 can
#       resolve every entry to either a regen-eligible test (produce
#       .bin/.err) or a documented exclusion in zxbasm_excluded.txt.
#
# Hard-coded python3.12; no fallback to system python3.
#
# Usage: run_zxbasm_tests_strict.sh <zxbasm-binary> <test-dir>
#
# Exit code: 0 if PASS+ERR_PASS == total-non-skip; 1 otherwise.

set -uo pipefail

ZXBASM_C="${1:?Usage: $0 <zxbasm-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <zxbasm-binary> <test-dir>}"

. "$(dirname "$0")/_find_python312.sh"

ZXBASM_C=$(cd "$(dirname "$ZXBASM_C")" && pwd)/$(basename "$ZXBASM_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

MISSING_LOG="$PROJECT_ROOT/csrc/tests/phase3-asm-missing-fixtures.log"
PYTHON_INTERNAL_LOG="$PROJECT_ROOT/csrc/tests/phase3-python-internal-skips.log"
EXCLUSIONS="$PROJECT_ROOT/csrc/tests/zxbasm_excluded.txt"
: > "$MISSING_LOG"
: > "$PYTHON_INTERNAL_LOG"

normalise_stderr() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); sub(/[ \t]+$/, ""); print }
    '
}

is_python_internal_error() {
    grep -qE '^Traceback \(most recent call last\):|^[[:space:]]*(KeyError|TypeError|AttributeError|IndexError|RecursionError|AssertionError):' "$1"
}

is_excluded() {
    [ -f "$EXCLUSIONS" ] && grep -qE "^$1[[:space:]]*#" "$EXCLUSIONS"
}

PASS=0
FAIL=0
ERR_PASS=0
ERR_FAIL=0
SKIP_MISSING=0
SKIP_EXCLUDED=0
SKIP_PYINTERNAL=0
declare -a FAIL_NAMES
declare -a ERR_FAIL_NAMES

cd "$TEST_DIR"

for asm in *.asm; do
    name=${asm%.asm}

    if is_excluded "$name"; then
        SKIP_EXCLUDED=$((SKIP_EXCLUDED + 1))
        continue
    fi

    has_err=0; has_bin=0
    [ -f "${name}.err" ] && has_err=1
    [ -f "${name}.bin" ] && has_bin=1

    if [ "$has_err" -eq 0 ] && [ "$has_bin" -eq 0 ]; then
        SKIP_MISSING=$((SKIP_MISSING + 1))
        echo "$name" >> "$MISSING_LOG"
        continue
    fi

    py_out_bin=$(mktemp); c_out_bin=$(mktemp)
    py_err=$(mktemp); c_err=$(mktemp)
    py_rc=0; c_rc=0

    OPTS_EXTRA=""
    if [[ "$name" == zxnext_* ]]; then
        # Existing fuzzy harness uses --zxnext for ZX Next opcode tests;
        # upstream's test_asm.py uses "-O=-N" which the C port's argparse
        # does not split into -O + -N. --zxnext is portable across both.
        OPTS_EXTRA="--zxnext"
    fi

    "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbasm import zxbasm
sys.argv = ['zxbasm', '-o', '$py_out_bin'] + ('$OPTS_EXTRA'.split() if '$OPTS_EXTRA' else []) + ['$asm']
sys.exit(zxbasm.main() or 0)
" 2> "$py_err" >/dev/null || py_rc=$?

    "$ZXBASM_C" -o "$c_out_bin" $OPTS_EXTRA "$asm" 2> "$c_err" >/dev/null || c_rc=$?

    if is_python_internal_error "$py_err"; then
        SKIP_PYINTERNAL=$((SKIP_PYINTERNAL + 1))
        echo "$name :: $(head -1 "$py_err")" >> "$PYTHON_INTERNAL_LOG"
        rm -f "$py_out_bin" "$c_out_bin" "$py_err" "$c_err"
        continue
    fi

    if [ "$has_err" -eq 1 ]; then
        # Error test: stderr must strict-cmp identical after normalisation.
        # Sibling .bin (if any) is ignored — .err presence is authoritative.
        py_err_norm=$(mktemp); c_err_norm=$(mktemp)
        normalise_stderr < "$py_err" > "$py_err_norm"
        normalise_stderr < "$c_err"  > "$c_err_norm"

        if [ "$py_rc" -ne "$c_rc" ]; then
            ERR_FAIL=$((ERR_FAIL + 1))
            ERR_FAIL_NAMES+=("$name (exit codes diverge: py=$py_rc c=$c_rc)")
            echo "--- ERR-FAIL: $name (exit codes py=$py_rc c=$c_rc) ---"
        elif cmp -s "$py_err_norm" "$c_err_norm"; then
            ERR_PASS=$((ERR_PASS + 1))
        else
            ERR_FAIL=$((ERR_FAIL + 1))
            ERR_FAIL_NAMES+=("$name")
            echo "--- ERR-FAIL: $name (stderr mismatch) ---"
            diff -u "$py_err_norm" "$c_err_norm"
            echo
        fi

        rm -f "$py_out_bin" "$c_out_bin" "$py_err" "$c_err" "$py_err_norm" "$c_err_norm"
        continue
    fi

    # Success test: strict cmp of binary output against .bin fixture.
    if [ "$c_rc" -ne 0 ]; then
        FAIL=$((FAIL + 1))
        FAIL_NAMES+=("$name (C zxbasm exited $c_rc)")
        echo "--- FAIL: $name (C zxbasm exited $c_rc) ---"
        cat "$c_err" | head -5
        echo
    elif cmp -s "$c_out_bin" "${name}.bin"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAIL_NAMES+=("$name")
        echo "--- FAIL: $name (binary mismatch) ---"
        if command -v xxd >/dev/null 2>&1; then
            echo "Expected (${name}.bin):"; xxd "${name}.bin" | head -5
            echo "Got:";                    xxd "$c_out_bin"   | head -5
        fi
        echo
    fi

    rm -f "$py_out_bin" "$c_out_bin" "$py_err" "$c_err"
done

echo "=========================================="
echo "zxbasm strict harness:"
echo "  Success-test PASS:        $PASS"
echo "  Success-test FAIL:        $FAIL"
echo "  Error-test PASS:          $ERR_PASS"
echo "  Error-test FAIL:          $ERR_FAIL"
echo "  SKIP (missing fixture):   $SKIP_MISSING"
echo "  SKIP (excluded):          $SKIP_EXCLUDED"
echo "  SKIP (py-internal):       $SKIP_PYINTERNAL"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "Failed success tests:"
    printf '  %s\n' "${FAIL_NAMES[@]}"
fi
if [ "$ERR_FAIL" -gt 0 ]; then
    echo "Failed error tests:"
    printf '  %s\n' "${ERR_FAIL_NAMES[@]}"
fi

if [ "$SKIP_MISSING" -gt 0 ]; then
    echo "Missing-fixture .asm files (logged to phase3-asm-missing-fixtures.log): $SKIP_MISSING"
fi

if [ "$FAIL" -gt 0 ] || [ "$ERR_FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
