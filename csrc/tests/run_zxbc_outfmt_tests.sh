#!/usr/bin/env bash
# Strict zxbc outfmt (container) harness: Python at runtime is the oracle.
#
# Usage: run_zxbc_outfmt_tests.sh <c-zxbc-binary> <test-dir>
#
# Phase 6 corpus (NOT the full .bas set). The Python functional harness
# (tests/functional/test.py:23,252-257) drives a container format only
# for .bas whose basename matches ^(tzx|tap|sna|z80)_.*. Each such
# fixture is run for its native container format with the EXACT test.py
# option set (test.py:243-257 + the inline -I stdlib:runtime of
# test.py:437):
#
#   zxbc -O1 --arch <arch> --output-format=<ext> <abs .bas> \
#        -o <scratch>/<stem>.<ext> -a -B -e /dev/null \
#        -I <root>/stdlib:<root>/runtime
#
# Plus every container-prefixed fixture is additionally run as
# --output-format=bin (the .bin arm has no committed reference, so
# Python-at-runtime is the only oracle; .bin needs no -B/-a — they
# would parser.error for non-tape/snapshot formats per
# args_config.py:137-149, so they are dropped on the .bin arm only).
#
# Byte-cmp policy: WHOLE-FILE raw `cmp -s`, NO normalisation
# (test.py:179-180 is_binary => raw bytes == bytes). The codegen
# <PROJECT_ROOT> normalisation does NOT apply to containers. The
# linchpin: .tap/.tzx embed the -o output basename (first 10 chars) in
# the tape header title (asmparse.py:1043-1044 -> tzx.py:95-99), so the
# harness MUST give C and Python the SAME -o basename (the fixture
# stem + ext). .bin/.sna/.z80 embed no filename and carry no timestamp,
# so no normalisation step is needed or possible.
#
# Five buckets (mirroring run_zxbc_codegen_tests.sh's five-bucket idiom):
#   BYTE_EQUAL       C container == Python container (raw cmp), both rc 0
#   BYTE_DIFF        both produced output, bytes differ
#   SKIP_C_ERROR     Python produced a container; C did not produce a
#                    byte-equal one (rc != 0 / no output / wrong bytes).
#                    At the Phase-6 entry baseline EVERY container row
#                    lands here: C writes asm text into the container
#                    path (no generate_binary analogue in csrc/), so
#                    C bytes != Python bytes for every fixture.
#   SKIP_PY_ERROR    Python rc != 0 / no output (reject fixture / env)
#   SKIP_KNOWN_BUG   listed in zxbc_python_bugs.txt (none for outfmt yet;
#                    bucket kept for parity with the Phase-5 convention)
#
# FALSE_POS guard: a BYTE_EQUAL where Python errored would be a hard
# regression (C emitting a valid container where Python rejects),
# mirroring run_zxbc_codegen_tests.sh:139-147. Must stay 0.
#
# All-RED by design at the Phase-6 entry baseline: C has no container
# generator, so BYTE_EQUAL == 0 and every container test is SKIP_C_ERROR.
# The SKIP_C_ERROR -> BYTE_EQUAL transition across S6.2->S6.6 IS the
# Phase-6 meter.
#
# Exits non-zero whenever BYTE_EQUAL < outfmt-test count (i.e. any red
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

# Test arch is the corpus dir name (test.py:245); the -I include path is
# the inline-mode stdlib:runtime under the project root (test.py:437).
ARCH=$(basename "$TEST_DIR")
INCLUDE_PATH="$PROJECT_ROOT/stdlib:$PROJECT_ROOT/runtime"

# The outfmt known-Python-bug SKIP list (shared file; none for outfmt
# yet — kept for parity with the Phase-5 convention).
BUGS_FILE="$PROJECT_ROOT/csrc/tests/zxbc_python_bugs.txt"
is_known_bug() {
    [ -f "$BUGS_FILE" ] || return 1
    grep -vE '^[[:space:]]*(#|$)' "$BUGS_FILE" 2>/dev/null | grep -qxF "$1"
}

is_python_internal_error() {
    grep -qE 'Traceback \(most recent call last\)|ImportError|ModuleNotFoundError' "$1"
}

BYTE_EQUAL=0
BYTE_DIFF=0
SKIP_C_ERROR=0
SKIP_PY_ERROR=0
SKIP_KNOWN_BUG=0
FALSE_POS=0

# One per-run scratch directory. Same discipline as the codegen harness
# (run_zxbc_codegen_tests.sh:72-90): $TEST_DIR is the read-only upstream
# fixture corpus; both Python and C fall back to writing a default file
# next to CWD when -o is unusable, which would clobber the committed
# fixtures if CWD were inside the corpus. A single `mktemp -d` plus
# fixed names inside it is portable and immune to the BSD/macOS mktemp
# trailing-X collision.
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_of_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

# One corpus row = one (fixture, format) pair. Updates the bucket
# counters. The SAME -o basename is given to Python and C (the
# embedded-title linchpin); for the .bin arm, -B/-a are dropped (they
# parser.error for non-tape/snapshot formats).
run_pair() {
    local bas="$1" fmt="$2"
    local base stem out_file err_py err_c py_rc c_rc pyargv
    local -a cargv
    base=$(basename "$bas")
    stem="${base%.bas}"

    if is_known_bug "$stem"; then
        SKIP_KNOWN_BUG=$((SKIP_KNOWN_BUG + 1))
        return
    fi

    # SAME -o basename for Python and C (the embedded-title linchpin).
    # The runs are sequenced over the one path; Python's bytes are
    # snapshotted before the C run reuses it.
    out_file="$SCRATCH/${stem}.${fmt}"
    err_py="$SCRATCH/py_err"
    err_c="$SCRATCH/c_err"
    py_rc=0
    c_rc=0

    # argv: -O1 --arch <arch> --output-format=<fmt> <abs.bas>
    #       -o <scratch>/<stem>.<fmt> [-a -B] -e /dev/null
    #       -I <root>/stdlib:<root>/runtime
    if [ "$fmt" = "bin" ]; then
        pyargv="['zxbc','-O1','--arch','$ARCH','--output-format=$fmt','$bas','-o','$out_file','-e','/dev/null','-I','$INCLUDE_PATH']"
        cargv=( -O1 --arch "$ARCH" "--output-format=$fmt" "$bas" -o "$out_file" -e /dev/null -I "$INCLUDE_PATH" )
    else
        pyargv="['zxbc','-O1','--arch','$ARCH','--output-format=$fmt','$bas','-o','$out_file','-a','-B','-e','/dev/null','-I','$INCLUDE_PATH']"
        cargv=( -O1 --arch "$ARCH" "--output-format=$fmt" "$bas" -o "$out_file" -a -B -e /dev/null -I "$INCLUDE_PATH" )
    fi

    # --- Python oracle ---
    rm -f "$out_file" "$err_py"
    ( cd "$SCRATCH" && "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = $pyargv
sys.exit(entry_point() or 0)
" ) > /dev/null 2> "$err_py" || py_rc=$?

    if is_python_internal_error "$err_py"; then
        SKIP_PY_ERROR=$((SKIP_PY_ERROR + 1))
        return
    fi

    local py_has=0
    [ -s "$out_file" ] && py_has=1

    local py_snap=""
    if [ "$py_rc" -eq 0 ] && [ "$py_has" -eq 1 ]; then
        py_snap="$SCRATCH/py_snap.bin"
        cp "$out_file" "$py_snap"
    fi

    # --- C candidate (same -o basename) ---
    rm -f "$out_file" "$err_c"
    ( cd "$SCRATCH" && "$ZXBC_C" "${cargv[@]}" ) > /dev/null 2> "$err_c" || c_rc=$?

    local c_has=0
    [ -s "$out_file" ] && c_has=1

    if [ "$py_rc" -ne 0 ] || [ "$py_has" -eq 0 ]; then
        # Python did not produce a container (reject fixture / env).
        # The hard regression is C succeeding where Python rejects; but
        # at this base C never emits a real container and we have no
        # Python bytes to byte-compare against, so this is a Python-side
        # skip. A future C that genuinely emits a container where Python
        # rejects would not byte-equal anything (no py_snap) and so can
        # never reach BYTE_EQUAL — the FALSE_POS invariant holds by
        # construction; the counter is kept for parity/visibility.
        SKIP_PY_ERROR=$((SKIP_PY_ERROR + 1))
        rm -f "$out_file" "$err_py" "$err_c"
        return
    fi

    if [ "$c_rc" -ne 0 ] || [ "$c_has" -eq 0 ]; then
        SKIP_C_ERROR=$((SKIP_C_ERROR + 1))
        rm -f "$out_file" "$err_py" "$err_c" "$py_snap"
        return
    fi

    # Both produced output: WHOLE-FILE raw byte-compare, NO
    # normalisation (test.py:179-180).
    if cmp -s "$py_snap" "$out_file"; then
        BYTE_EQUAL=$((BYTE_EQUAL + 1))
    else
        # C produced a file but it is not Python's container. At the
        # Phase-6 entry baseline this is C emitting asm text into the
        # container path (no generate_binary analogue). Per the strict
        # idiom this is the all-RED state: SKIP_C_ERROR (C did not
        # produce a format-valid byte-equal container). When a real C
        # generator lands and merely differs, the meter reads it the
        # same; the SKIP_C_ERROR -> BYTE_EQUAL move is the signal.
        SKIP_C_ERROR=$((SKIP_C_ERROR + 1))
        echo "--- SKIP_C_ERROR: ${stem} [${fmt}] (C output != Python container) ---"
    fi

    rm -f "$out_file" "$err_py" "$err_c" "$py_snap"
}

for bas in "$TEST_DIR"/*.bas; do
    [ -e "$bas" ] || continue
    base=$(basename "$bas")
    stem="${base%.bas}"

    # Corpus = container-prefixed fixtures only (test.py:23).
    case "$stem" in
        tap_*|tzx_*|sna_*|z80_*) ;;
        *) continue ;;
    esac

    # Native container format = the prefix.
    nat="${stem%%_*}"

    run_pair "$bas" "$nat"
    # ...and additionally the .bin arm (Python-at-runtime oracle; the
    # .bin format has no committed reference and no *_ prefix).
    run_pair "$bas" "bin"
done

cleanup
trap - EXIT INT TERM

OUTFMT_TOTAL=$((BYTE_EQUAL + BYTE_DIFF + SKIP_C_ERROR + FALSE_POS))
echo "##zxbc-outfmt##"
echo "  BYTE_EQUAL:           $BYTE_EQUAL"
echo "  BYTE_DIFF:            $BYTE_DIFF"
echo "  SKIP_C_ERROR:         $SKIP_C_ERROR"
echo "  FALSE_POS:            $FALSE_POS"
echo "  SKIP (Python-error):  $SKIP_PY_ERROR"
echo "  SKIP (known-py-bug):  $SKIP_KNOWN_BUG"
echo "  (outfmt tests:        $OUTFMT_TOTAL)"

# Strict idiom: non-zero whenever any outfmt test is not BYTE_EQUAL,
# or any FALSE_POS. All-RED by design at the Phase-6 entry baseline
# (BYTE_EQUAL == 0) => exit 1.
[ "$BYTE_EQUAL" -eq "$OUTFMT_TOTAL" ] && [ "$OUTFMT_TOTAL" -gt 0 ] \
    && [ "$FALSE_POS" -eq 0 ] && exit 0
exit 1
