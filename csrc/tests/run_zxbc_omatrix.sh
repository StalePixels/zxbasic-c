#!/bin/bash
# zxbc -O-matrix measurement harness (S7.3d-9).
#
# The standing zxbc end-to-end firewall (run_zxbc_full_tests.sh) proves
# byte-for-byte at DEFAULT optimization only. The byte-for-byte contract
# must hold at EVERY optimization level, so this harness sweeps each
# fixture at -O0, -O1, -O2, -O3 and RAW byte-compares the C binary's
# default `.bin` against the Python oracle's, per-level.
#
# Usage:
#   run_zxbc_omatrix.sh <c-zxbc-binary> [<test-dir>]
#
#   <c-zxbc-binary>  path to the built C zxbc (e.g. csrc/build/zxbc/zxbc)
#   <test-dir>       fixture corpus; default tests/functional/arch/zx48k.
#                    Pass tests/functional/arch/zxnext for the Next sweep.
#
# For each <stem>.bas in the corpus, at each of -O0 -O1 -O2 -O3:
#   Both interpreters run the SAME argv on the SAME fixture from their OWN
#   scratch CWD with an absolute input path and the SAME relative output
#   name `out.bin`:
#       zxbc -O<n> -o out.bin <abs .bas>
#   i.e. the real end-to-end default-`.bin` pipeline (no --output-format),
#   exactly the full-tests harness shape, parametrised by -O. Separate
#   py/ and c/ scratch dirs mean an identical -o basename cannot collide
#   in any filename-embedding container header, and a default-named CWD
#   artefact never touches the read-only corpus (CLAUDE.md r3).
#
# Per -O level, five buckets:
#   EQUAL              Both exit 0 AND out.bin byte-identical (RAW cmp).
#   BIN-DIFF           Both exit 0 and both produced out.bin, bytes differ.
#   C-ERR              Python exit 0 + out.bin; C exit != 0 OR no out.bin
#                      (and C did not crash). The silent-drop / port-gap
#                      backlog driver.
#   CRASH              C exit >= 128 (signal). Called out separately from
#                      C-ERR because a SIGSEGV/SIGABRT is a different fix
#                      class from a clean non-zero exit.
#   SKIP-Python-error  Python exit != 0 / no out.bin (reject / stale /
#                      env / python-internal traceback). C is not graded
#                      against a non-existent oracle output.
#
# A C success where Python rejects (Python exit != 0 but C exit 0 with
# out.bin) is reported as BIN-DIFF (a real divergence, never EQUAL or a
# silent skip) — mirroring the full-tests false-success guard.
#
# Constraints honoured (mission spec):
#   * macOS /bin/bash is 3.2: NO `declare -A`, NO `mapfile`. Per-level
#     counters are flat scalar variables ( *_O0 .. *_O3 ), selected by a
#     small case dispatch keyed on the level string. No associative
#     arrays anywhere.
#   * Each compile is watchdogged at 60s via `perl -e 'alarm N; exec'`
#     so a hung port cannot wedge the sweep.
#   * out.bin is BINARY: compared with RAW `cmp` only. No awk / sed / text
#     tooling is ever pointed at the binary (the S7.2g soundness lesson:
#     BSD awk truncates at the first invalid multibyte byte and can
#     collapse two different binaries to a spurious match).
#   * cwd resets between commands in the agent harness, so every path is
#     made absolute up front and the project root is derived from the
#     script's own location (not from $PWD).
#
# This is a REAL gate: a do-nothing C (exits non-zero / emits no out.bin)
# yields ALL C-ERR at every level, never a false EQUAL.
#
# Exit code: 0 always. This is a MEASUREMENT meter, exactly like
# run_zxbc_full_tests.sh — the printed per-level bucket counts are the
# verdict, not the process exit status.

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc-binary> [<test-dir>]}"

# Default corpus is zx48k; derived from this script's location so the cwd
# reset between agent-harness commands cannot misresolve a relative arg.
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_TEST_DIR="$SCRIPT_DIR/../../tests/functional/arch/zx48k"
TEST_DIR="${2:-$DEFAULT_TEST_DIR}"

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    echo "       Strict harness will not silently fall back to system python3." >&2
    exit 2
fi
if [ ! -x "$ZXBC_C" ]; then
    echo "ERROR: C zxbc binary '$ZXBC_C' not present / not executable." >&2
    exit 2
fi
if [ ! -d "$TEST_DIR" ]; then
    echo "ERROR: test dir '$TEST_DIR' not found." >&2
    exit 2
fi

# Absolute-ise the binary and corpus; derive the project root (dir that
# holds src/lib) so the Python oracle import path is correct.
ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
if [ ! -d "$PROJECT_ROOT/src/zxbc" ]; then
    echo "ERROR: cannot locate Python reference src/zxbc/ above $TEST_DIR." >&2
    exit 2
fi

# Per-compile watchdog (60s). perl's alarm + exec replaces the perl
# process image with the target, so the SIGALRM still fires against the
# compile. Returns the compile's own exit status, or 142 (128+SIGALRM)
# on timeout.
WATCHDOG=60
run_watchdogged() {
    perl -e 'alarm shift @ARGV; exec @ARGV or exit 127' "$WATCHDOG" "$@"
}

is_python_internal_error() {
    grep -qE 'Traceback \(most recent call last\)|ImportError|ModuleNotFoundError' "$1"
}

# Optimization levels swept. Space-separated so the for-loop is bash-3.2
# clean (no arrays needed for iteration).
OLEVELS="0 1 2 3"

# Flat per-level scalar counters (NO declare -A). Initialise all.
EQUAL_O0=0;  EQUAL_O1=0;  EQUAL_O2=0;  EQUAL_O3=0
DIFF_O0=0;   DIFF_O1=0;   DIFF_O2=0;   DIFF_O3=0
CERR_O0=0;   CERR_O1=0;   CERR_O2=0;   CERR_O3=0
CRASH_O0=0;  CRASH_O1=0;  CRASH_O2=0;  CRASH_O3=0
SKIP_O0=0;   SKIP_O1=0;   SKIP_O2=0;   SKIP_O3=0

# Per-level newline-delimited stem lists for the failing buckets.
DIFF_LIST_O0="";  DIFF_LIST_O1="";  DIFF_LIST_O2="";  DIFF_LIST_O3=""
CERR_LIST_O0="";  CERR_LIST_O1="";  CERR_LIST_O2="";  CERR_LIST_O3=""
CRASH_LIST_O0=""; CRASH_LIST_O1=""; CRASH_LIST_O2=""; CRASH_LIST_O3=""

# bump <bucket> <olevel>  — increment the right scalar counter.
bump() {
    case "$1/$2" in
        EQUAL/0) EQUAL_O0=$((EQUAL_O0+1));; EQUAL/1) EQUAL_O1=$((EQUAL_O1+1));;
        EQUAL/2) EQUAL_O2=$((EQUAL_O2+1));; EQUAL/3) EQUAL_O3=$((EQUAL_O3+1));;
        DIFF/0)  DIFF_O0=$((DIFF_O0+1));;   DIFF/1)  DIFF_O1=$((DIFF_O1+1));;
        DIFF/2)  DIFF_O2=$((DIFF_O2+1));;   DIFF/3)  DIFF_O3=$((DIFF_O3+1));;
        CERR/0)  CERR_O0=$((CERR_O0+1));;   CERR/1)  CERR_O1=$((CERR_O1+1));;
        CERR/2)  CERR_O2=$((CERR_O2+1));;   CERR/3)  CERR_O3=$((CERR_O3+1));;
        CRASH/0) CRASH_O0=$((CRASH_O0+1));; CRASH/1) CRASH_O1=$((CRASH_O1+1));;
        CRASH/2) CRASH_O2=$((CRASH_O2+1));; CRASH/3) CRASH_O3=$((CRASH_O3+1));;
        SKIP/0)  SKIP_O0=$((SKIP_O0+1));;   SKIP/1)  SKIP_O1=$((SKIP_O1+1));;
        SKIP/2)  SKIP_O2=$((SKIP_O2+1));;   SKIP/3)  SKIP_O3=$((SKIP_O3+1));;
    esac
}

# add_stem <listvar-suffix> <olevel> <stem> — append to the per-level
# list. Done with eval on a constructed var name (bash 3.2 has no
# namerefs); the suffix/level are script-controlled, the stem is a
# fixture filename, so no injection surface.
add_stem() {
    local var="$1_O$2"
    eval "$var=\"\${$var}\$3
\""
}

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_omatrix_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

PY_DIR="$SCRATCH/py"
C_DIR="$SCRATCH/c"

echo "=== run_zxbc_omatrix START ==="
echo "    corpus : $TEST_DIR"
echo "    C zxbc : $ZXBC_C"
echo "    levels : -O0 -O1 -O2 -O3"
echo ""

TOTAL_BAS=0
for bas in "$TEST_DIR"/*.bas; do
    [ -e "$bas" ] || continue
    TOTAL_BAS=$((TOTAL_BAS+1))
    base=$(basename "$bas")
    stem="${base%.bas}"

    for O in $OLEVELS; do
        rm -rf "$PY_DIR" "$C_DIR"
        mkdir -p "$PY_DIR" "$C_DIR"
        py_err="$SCRATCH/py_err"
        c_err="$SCRATCH/c_err"
        py_rc=0
        c_rc=0

        # --- Python oracle ---
        ( cd "$PY_DIR" && run_watchdogged "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc', '-O$O', '-o', 'out.bin', '$bas']
sys.exit(entry_point() or 0)
" ) > /dev/null 2> "$py_err" || py_rc=$?

        py_has=0; [ -s "$PY_DIR/out.bin" ] && py_has=1

        # Python rejected / produced no oracle output for this fixture+level.
        if [ "$py_rc" -ne 0 ] || [ "$py_has" -eq 0 ]; then
            # Run C anyway, only to catch a false-success (C emits where
            # Python rejects). Otherwise it is a Python-side skip.
            ( cd "$C_DIR" && run_watchdogged "$ZXBC_C" -O$O -o "out.bin" "$bas" ) \
                > /dev/null 2> "$c_err" || c_rc=$?
            c_has=0; [ -s "$C_DIR/out.bin" ] && c_has=1
            if [ "$c_rc" -eq 0 ] && [ "$c_has" -eq 1 ]; then
                bump DIFF "$O"
                add_stem DIFF_LIST "$O" "$stem (false-success: C emitted where Python rejected, py_rc=$py_rc)"
            else
                bump SKIP "$O"
            fi
            continue
        fi

        # --- C candidate ---
        ( cd "$C_DIR" && run_watchdogged "$ZXBC_C" -O$O -o "out.bin" "$bas" ) \
            > /dev/null 2> "$c_err" || c_rc=$?
        c_has=0; [ -s "$C_DIR/out.bin" ] && c_has=1

        if [ "$c_rc" -ge 128 ]; then
            bump CRASH "$O"
            add_stem CRASH_LIST "$O" "$stem (c_rc=$c_rc)"
            continue
        fi
        if [ "$c_rc" -ne 0 ] || [ "$c_has" -eq 0 ]; then
            bump CERR "$O"
            add_stem CERR_LIST "$O" "$stem (c_rc=$c_rc)"
            continue
        fi

        # Both produced out.bin: RAW byte-compare (NO text tooling).
        if cmp -s "$PY_DIR/out.bin" "$C_DIR/out.bin"; then
            bump EQUAL "$O"
        else
            bump DIFF "$O"
            add_stem DIFF_LIST "$O" "$stem"
        fi
    done
done

cleanup
trap - EXIT INT TERM

# ---- Per-level summary table -----------------------------------------
echo ""
echo "=== run_zxbc_omatrix SUMMARY ($TOTAL_BAS .bas fixtures x 4 levels) ==="
printf '%-6s %8s %9s %7s %7s %18s\n' "LEVEL" "EQUAL" "BIN-DIFF" "C-ERR" "CRASH" "SKIP-Python-error"
printf '%-6s %8d %9d %7d %7d %18d\n' "-O0" "$EQUAL_O0" "$DIFF_O0" "$CERR_O0" "$CRASH_O0" "$SKIP_O0"
printf '%-6s %8d %9d %7d %7d %18d\n' "-O1" "$EQUAL_O1" "$DIFF_O1" "$CERR_O1" "$CRASH_O1" "$SKIP_O1"
printf '%-6s %8d %9d %7d %7d %18d\n' "-O2" "$EQUAL_O2" "$DIFF_O2" "$CERR_O2" "$CRASH_O2" "$SKIP_O2"
printf '%-6s %8d %9d %7d %7d %18d\n' "-O3" "$EQUAL_O3" "$DIFF_O3" "$CERR_O3" "$CRASH_O3" "$SKIP_O3"

# ---- Per-level failing-stem lists ------------------------------------
print_list() {
    # print_list <label> <list-text>
    local label="$1" text="$2"
    local n
    n=$(printf '%s' "$text" | grep -c .)
    echo ""
    echo "--- $label ($n) ---"
    [ "$n" -gt 0 ] && printf '%s' "$text" | grep . | sed 's/^/    /'
}

print_list "-O0 BIN-DIFF" "$DIFF_LIST_O0"
print_list "-O0 C-ERR"    "$CERR_LIST_O0"
print_list "-O0 CRASH"    "$CRASH_LIST_O0"
print_list "-O1 BIN-DIFF" "$DIFF_LIST_O1"
print_list "-O1 C-ERR"    "$CERR_LIST_O1"
print_list "-O1 CRASH"    "$CRASH_LIST_O1"
print_list "-O2 BIN-DIFF" "$DIFF_LIST_O2"
print_list "-O2 C-ERR"    "$CERR_LIST_O2"
print_list "-O2 CRASH"    "$CRASH_LIST_O2"
print_list "-O3 BIN-DIFF" "$DIFF_LIST_O3"
print_list "-O3 C-ERR"    "$CERR_LIST_O3"
print_list "-O3 CRASH"    "$CRASH_LIST_O3"

echo ""
echo "=== run_zxbc_omatrix END (measurement meter; exit 0 always) ==="
exit 0
