#!/usr/bin/env bash
# Strict zxbc codegen harness: Python at runtime is the oracle.
#
# Usage: run_zxbc_codegen_tests.sh <c-zxbc-binary> <test-dir>
#
# For each <stem>.bas in the corpus:
#   - sibling <stem>.asm present  => a codegen test (Python emits asm)
#   - no sibling .asm             => error/reject test (SKIP here; the
#                                    parse-only + AST-equiv harnesses
#                                    own the no-codegen-expected cases)
# Both interpreters run `--output-format=asm -o <tmp> <stem>.bas`. The
# two .asm outputs are compared byte-for-byte after the same path
# normalisation the other strict harnesses use (absolute project root
# -> <PROJECT_ROOT>); nothing else is normalised.
#
# Five buckets:
#   PASS           C asm == Python asm (normalised), both rc 0
#   C_NO_CODEGEN   Python emits asm (rc 0); C does not (rc != 0 / no asm)
#   ASM_MISMATCH   both rc 0 and emit asm, but bytes differ
#   FALSE_POS      C emits asm (rc 0) where Python errors (rc != 0)
#   SKIP           no sibling .asm (error/reject test) or py-internal
#
# All-RED by design at the Phase-5 entry baseline: C exits non-zero at
# the codegen handoff, so every .asm-bearing .bas is C_NO_CODEGEN and
# PASS == 0. The bucket transition (C_NO_CODEGEN -> PASS) across the
# S5.x sprints IS the Phase-5 meter. FALSE_POS must stay 0 (a C that
# emits asm where Python rejects is a hard regression, mirroring the
# parse harness's FALSE_POS gate).
#
# Exits non-zero whenever PASS < codegen-test count (i.e. any red
# bucket) — the strict-harness idiom; the gate compares the printed
# counts, not the exit code.

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <c-zxbc-binary> <test-dir>}"

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    echo "       Strict harness will not silently fall back to system python3." >&2
    exit 2
fi

ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

normalise() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); print }
    '
}

is_python_internal_error() {
    grep -qE 'Traceback \(most recent call last\)|ImportError|ModuleNotFoundError' "$1"
}

PASS=0
C_NO_CODEGEN=0
ASM_MISMATCH=0
FALSE_POS=0
SKIP=0
SKIP_PYINTERNAL=0

# One per-run scratch directory. Two reasons this exists and we never
# `cd "$TEST_DIR"`:
#   1. `$TEST_DIR` is the read-only upstream fixture corpus. Both the
#      Python compiler and the C binary fall back to writing a default
#      `<stem>.asm` next to the *current working directory* when the
#      output path is unusable. If we ran them with CWD inside the
#      corpus, that fallback would overwrite the committed fixtures.
#      Compiling from a scratch CWD with an absolute input path keeps
#      any CWD-side artefact in scratch, never in `tests/`.
#   2. The previous code built temp paths with
#      `mktemp /tmp/zxbc_cg_py_XXXXXX.asm`. On BSD/macOS mktemp the
#      `X` run must be the trailing characters; with a `.asm` suffix
#      the template is taken *literally*, so every iteration after the
#      first collided, mktemp printed nothing, the output path became
#      the empty string, `-o ''` made the compiler write its default
#      `<stem>.asm` into the corpus, and every test fell out of the
#      codegen buckets. A single `mktemp -d` plus fixed names inside
#      it is portable and immune to that collision.
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_cg_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

for bas in "$TEST_DIR"/*.bas; do
    [ -e "$bas" ] || continue
    base=$(basename "$bas")
    stem="${base%.bas}"

    # No sibling .asm => Python rejects / no codegen expected: SKIP
    # (owned by the parse-only + AST-equiv harnesses).
    if [ ! -f "$TEST_DIR/${stem}.asm" ]; then
        SKIP=$((SKIP + 1))
        continue
    fi

    py_out="$SCRATCH/py_out.asm"
    c_out="$SCRATCH/c_out.asm"
    py_err="$SCRATCH/py_err"
    c_err="$SCRATCH/c_err"
    rm -f "$py_out" "$c_out" "$py_err" "$c_err"
    py_rc=0
    c_rc=0

    # Run from the scratch dir with an absolute input path so any
    # default-named CWD-side artefact lands in scratch, never in the
    # read-only corpus.
    ( cd "$SCRATCH" && "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc', '--output-format=asm', '-o', '$py_out', '$bas']
sys.exit(entry_point() or 0)
" ) > /dev/null 2> "$py_err" || py_rc=$?

    if is_python_internal_error "$py_err"; then
        SKIP_PYINTERNAL=$((SKIP_PYINTERNAL + 1))
        continue
    fi

    ( cd "$SCRATCH" && "$ZXBC_C" --output-format=asm -o "$c_out" "$bas" ) > /dev/null 2> "$c_err" || c_rc=$?

    py_has_asm=0; [ -s "$py_out" ] && py_has_asm=1
    c_has_asm=0;  [ -s "$c_out" ]  && c_has_asm=1

    if [ "$py_rc" -ne 0 ] || [ "$py_has_asm" -eq 0 ]; then
        # Python did not produce asm for a .asm-bearing fixture: treat
        # as a skip (stale/environant-specific) unless C wrongly did.
        if [ "$c_rc" -eq 0 ] && [ "$c_has_asm" -eq 1 ]; then
            FALSE_POS=$((FALSE_POS + 1))
            echo "--- FALSE_POS: $stem (C emitted asm; Python rc=$py_rc) ---"
        else
            SKIP=$((SKIP + 1))
        fi
    elif [ "$c_rc" -ne 0 ] || [ "$c_has_asm" -eq 0 ]; then
        C_NO_CODEGEN=$((C_NO_CODEGEN + 1))
    else
        pn="$SCRATCH/py_norm.asm"; cn="$SCRATCH/c_norm.asm"
        normalise < "$py_out" > "$pn"
        normalise < "$c_out"  > "$cn"
        if cmp -s "$pn" "$cn"; then
            PASS=$((PASS + 1))
        else
            ASM_MISMATCH=$((ASM_MISMATCH + 1))
            echo "--- ASM_MISMATCH: $stem ---"
        fi
        rm -f "$pn" "$cn"
    fi

    rm -f "$py_out" "$c_out" "$py_err" "$c_err"
done

cleanup
trap - EXIT INT TERM

CODEGEN_TOTAL=$((PASS + C_NO_CODEGEN + ASM_MISMATCH + FALSE_POS))
echo "##zxbc-codegen##"
echo "  PASS:                $PASS"
echo "  C_NO_CODEGEN:        $C_NO_CODEGEN"
echo "  ASM_MISMATCH:        $ASM_MISMATCH"
echo "  FALSE_POS:           $FALSE_POS"
echo "  SKIP (no .asm):      $SKIP"
echo "  SKIP (py-internal):  $SKIP_PYINTERNAL"
echo "  (codegen tests:      $CODEGEN_TOTAL)"

# Strict idiom: non-zero whenever any codegen test is not PASS.
[ "$PASS" -eq "$CODEGEN_TOTAL" ] && [ "$FALSE_POS" -eq 0 ] && exit 0
exit 1
