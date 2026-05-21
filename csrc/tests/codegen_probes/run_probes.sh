#!/usr/bin/env bash
# run_probes.sh — additive codegen-probe meter for the zxbasic-c C port.
#
# WHY THIS EXISTS
#   The inherited tests/functional/ corpus is fixed and silent on codepaths
#   it never exercises. These probes are NEW, hand-authored .bas fixtures
#   that deliberately drive codegen codepaths the corpus does not — starting
#   with the full typecast cross-product (string<->numeric, byref mismatch,
#   CAST-with-string, etc.). For each fixture this runner compares the FULL
#   meaningful contract between the Python reference oracle and the C port:
#
#     1. exit code            (must match)
#     2. stderr   — path-normalised, compared (this is where W*-warnings and
#                   typecast ERRORS surface; the staged meter does NOT check
#                   stderr, so divergence here is invisible to it)
#     3. Stage-1 ASM          (--output-format=asm, path-normalised cmp)
#     4. end-to-end binary    (default `-o out.bin`, RAW cmp — never normalise
#                              binary; awk/sed over NUL-bearing data is unsound)
#
#   Buckets (one per fixture):
#     PROBE-EQUAL        all four parts match
#     PROBE-DIFF-EXIT    exit codes differ
#     PROBE-DIFF-STDERR  exit codes match but normalised stderr differs
#     PROBE-DIFF-ASM     exit/stderr match but stage-1 asm differs
#     PROBE-DIFF-BIN     exit/stderr/asm match but end-to-end binary differs
#     SKIP-PY-ERROR      Python raised an internal error (Traceback/ImportError)
#
#   Precedence note: a fixture is classified by the FIRST contract part (in the
#   order exit, stderr, asm, bin) that diverges, so the bucket localises the
#   earliest divergence. Cases where Python legitimately rejects (non-zero exit,
#   no asm/bin) are still compared: a matching C rejection with matching stderr
#   is PROBE-EQUAL; a divergence is bucketed accordingly. That is the point —
#   the typecast ERROR path (string<->numeric) is a meaningful-stderr contract.
#
#   This is a DIAGNOSTIC meter: it always exits 0. The divergences ARE the
#   finding; the parent re-runs and adjudicates each (verify-don't-adopt).
#
# Usage: run_probes.sh <category-subdir>          (e.g. run_probes.sh typecast)
#        run_probes.sh /abs/path/to/category-dir  (absolute also accepted)
#
# bash 3.2-safe (macOS /bin/bash): scalar counters only, no associative arrays.

set -uo pipefail

CAT_ARG="${1:?usage: $0 <category-subdir>   (e.g. typecast)}"

# ---- locate self / project root (walk up for src/lib, the corpus marker) ----
SELF_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT="$SELF_DIR"
while [ "$ROOT" != "/" ]; do
    [ -d "$ROOT/src/lib" ] && break
    ROOT=$(dirname "$ROOT")
done
if [ ! -d "$ROOT/src/lib" ]; then
    echo "ERROR: could not locate project root (no src/lib above $SELF_DIR)." >&2
    exit 2
fi
ROOT=$(cd "$ROOT" && pwd -P)

# ---- category dir: accept a bare subdir name OR an absolute path ----
if [ -d "$CAT_ARG" ]; then
    CAT_DIR=$(cd "$CAT_ARG" && pwd)
elif [ -d "$SELF_DIR/$CAT_ARG" ]; then
    CAT_DIR=$(cd "$SELF_DIR/$CAT_ARG" && pwd)
else
    echo "ERROR: category dir '$CAT_ARG' not found (looked at it directly and under $SELF_DIR/)." >&2
    exit 2
fi

# ---- interpreters: pinned Python oracle + the built C binaries ----
PY=/opt/homebrew/bin/python3.12
[ -x "$PY" ] || { echo "ERROR: required interpreter $PY not present (no silent fallback to system python3)." >&2; exit 2; }
ZXBC_C="$ROOT/csrc/build/zxbc/zxbc"
[ -x "$ZXBC_C" ] || { echo "ERROR: C zxbc not built at $ZXBC_C — build it (cmake --build csrc/build --target zxbc) first." >&2; exit 2; }

# ---- normalisation: strip the absolute project root from TEXT streams only ----
# Two spellings of the root (symlink-resolved and as-walked) are both replaced.
ROOT_REAL=$(cd "$ROOT" && pwd -P)
norm_text() {
    awk -v r1="$ROOT_REAL" -v r2="$ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); print }
    '
}

is_py_internal() {
    grep -qE 'Traceback \(most recent call last\)|ImportError|ModuleNotFoundError' "$1"
}

# Run the Python oracle from a given scratch CWD with an explicit argv list.
pyrun() {
    local cwd="$1"; shift
    local argv="$1"   # a python-list literal, e.g. ['--output-format=asm','-o','x.asm','/abs/in.bas']
    ( cd "$cwd" && "$PY" -c "
import sys
sys.path.insert(0, '$ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc'] + $argv
sys.exit(entry_point() or 0)
" )
}

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_probe_XXXXXX")
[ -n "$SCRATCH" ] && [ -d "$SCRATCH" ] || { echo "ERROR: could not create scratch dir." >&2; exit 2; }
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

TOTAL=0
EQUAL=0
D_EXIT=0
D_STDERR=0
D_ASM=0
D_BIN=0
SKIP_PY=0

echo "==================== CODEGEN PROBES :: $(basename "$CAT_DIR") ===================="
echo "  oracle : $PY"
echo "  c-port : $ZXBC_C"
echo "  dir    : ${CAT_DIR#$ROOT/}"
echo "  --------------------------------------------------------------"

for bas in "$CAT_DIR"/*.bas; do
    [ -e "$bas" ] || continue
    stem=$(basename "$bas" .bas)
    TOTAL=$((TOTAL + 1))

    d="$SCRATCH/$stem"; mkdir -p "$d/cy_asm" "$d/py_asm" "$d/cy_bin" "$d/py_bin"

    # ---------- Stage-1 ASM (each interpreter in its own scratch CWD) ----------
    py_asm="$d/py_asm/out.asm"; py_asm_e="$d/py_asm/err"
    cy_asm="$d/cy_asm/out.asm"; cy_asm_e="$d/cy_asm/err"
    py_asm_rc=0; cy_asm_rc=0
    pyrun "$d/py_asm" "['--output-format=asm','-o','$py_asm','$bas']" >/dev/null 2>"$py_asm_e" || py_asm_rc=$?
    ( cd "$d/cy_asm" && "$ZXBC_C" --output-format=asm -o "$cy_asm" "$bas" ) >/dev/null 2>"$cy_asm_e" || cy_asm_rc=$?

    if is_py_internal "$py_asm_e"; then
        SKIP_PY=$((SKIP_PY + 1))
        echo "SKIP-PY-ERROR     $stem :: Python internal error (asm stage)"
        rm -rf "$d"; continue
    fi

    # ---------- end-to-end binary (default pipeline -o out.bin) ----------
    py_bin="$d/py_bin/out.bin"; py_bin_e="$d/py_bin/err"
    cy_bin="$d/cy_bin/out.bin"; cy_bin_e="$d/cy_bin/err"
    py_bin_rc=0; cy_bin_rc=0
    pyrun "$d/py_bin" "['-o','$py_bin','$bas']" >/dev/null 2>"$py_bin_e" || py_bin_rc=$?
    ( cd "$d/cy_bin" && "$ZXBC_C" -o "$cy_bin" "$bas" ) >/dev/null 2>"$cy_bin_e" || cy_bin_rc=$?

    if is_py_internal "$py_bin_e"; then
        SKIP_PY=$((SKIP_PY + 1))
        echo "SKIP-PY-ERROR     $stem :: Python internal error (bin stage)"
        rm -rf "$d"; continue
    fi

    # ---- The meaningful contract is judged on the default (end-to-end) run:
    #      exit code + stderr. The asm-stage run additionally checks codegen-text
    #      fidelity. Order of judgement: exit, stderr, asm, bin. ----

    # (1) exit code — compare the end-to-end exit codes.
    if [ "$py_bin_rc" -ne "$cy_bin_rc" ]; then
        D_EXIT=$((D_EXIT + 1))
        echo "PROBE-DIFF-EXIT   $stem :: exit Py=$py_bin_rc  C=$cy_bin_rc"
        echo "    Py stderr: $(norm_text <"$py_bin_e" | grep -iE 'error|warning' | head -1 | cut -c1-110)"
        echo "    C  stderr: $(norm_text <"$cy_bin_e" | grep -iE 'error|warning' | head -1 | cut -c1-110)"
        rm -rf "$d"; continue
    fi

    # (2) stderr — path-normalised, compared (warnings + typecast errors live here).
    norm_text <"$py_bin_e" >"$d/py_bin.en"
    norm_text <"$cy_bin_e" >"$d/cy_bin.en"
    if ! cmp -s "$d/py_bin.en" "$d/cy_bin.en"; then
        D_STDERR=$((D_STDERR + 1))
        echo "PROBE-DIFF-STDERR $stem :: exit match ($py_bin_rc) but normalised stderr differs"
        # show every differing line, prefixed P< (python-only) / C> (c-only)
        diff "$d/py_bin.en" "$d/cy_bin.en" | grep -E '^[<>]' | while IFS= read -r ln; do
            case "$ln" in
                "<"*) echo "    P${ln#<}";;
                ">"*) echo "    C${ln#>}";;
            esac
        done
        rm -rf "$d"; continue
    fi

    # (3) Stage-1 ASM — path-normalised cmp (codegen-text fidelity).
    py_has_asm=0; [ -s "$py_asm" ] && py_has_asm=1
    cy_has_asm=0; [ -s "$cy_asm" ] && cy_has_asm=1
    if [ "$py_has_asm" -ne "$cy_has_asm" ]; then
        D_ASM=$((D_ASM + 1))
        echo "PROBE-DIFF-ASM    $stem :: asm presence differs (Py asm=$py_has_asm rc=$py_asm_rc  C asm=$cy_has_asm rc=$cy_asm_rc)"
        rm -rf "$d"; continue
    fi
    if [ "$py_has_asm" -eq 1 ]; then
        norm_text <"$py_asm" >"$d/py.an"
        norm_text <"$cy_asm" >"$d/cy.an"
        if ! cmp -s "$d/py.an" "$d/cy.an"; then
            D_ASM=$((D_ASM + 1))
            echo "PROBE-DIFF-ASM    $stem :: stage-1 asm differs (Py $(wc -l <"$d/py.an" | tr -d ' ')L vs C $(wc -l <"$d/cy.an" | tr -d ' ')L)"
            diff "$d/py.an" "$d/cy.an" | grep -E '^[<>]' | head -8 | while IFS= read -r ln; do
                case "$ln" in
                    "<"*) echo "    P${ln#<}";;
                    ">"*) echo "    C${ln#>}";;
                esac
            done
            rm -rf "$d"; continue
        fi
    fi

    # (4) end-to-end binary — RAW cmp (never normalise binary).
    py_has_bin=0; [ -s "$py_bin" ] && py_has_bin=1
    cy_has_bin=0; [ -s "$cy_bin" ] && cy_has_bin=1
    if [ "$py_has_bin" -ne "$cy_has_bin" ]; then
        D_BIN=$((D_BIN + 1))
        echo "PROBE-DIFF-BIN    $stem :: binary presence differs (Py bin=$py_has_bin  C bin=$cy_has_bin)"
        rm -rf "$d"; continue
    fi
    if [ "$py_has_bin" -eq 1 ]; then
        if ! cmp -s "$py_bin" "$cy_bin"; then
            D_BIN=$((D_BIN + 1))
            firstdiff=$(cmp "$py_bin" "$cy_bin" 2>&1 | head -1)
            echo "PROBE-DIFF-BIN    $stem :: end-to-end binary differs (Py $(wc -c <"$py_bin" | tr -d ' ')B vs C $(wc -c <"$cy_bin" | tr -d ' ')B) :: $firstdiff"
            rm -rf "$d"; continue
        fi
    fi

    EQUAL=$((EQUAL + 1))
    echo "PROBE-EQUAL       $stem"
    rm -rf "$d"
done

cleanup; trap - EXIT INT TERM

echo "  --------------------------------------------------------------"
echo "  SUMMARY ($(basename "$CAT_DIR"))"
printf '    fixtures            %d\n' "$TOTAL"
printf '    PROBE-EQUAL         %d\n' "$EQUAL"
printf '    PROBE-DIFF-EXIT     %d\n' "$D_EXIT"
printf '    PROBE-DIFF-STDERR   %d\n' "$D_STDERR"
printf '    PROBE-DIFF-ASM      %d\n' "$D_ASM"
printf '    PROBE-DIFF-BIN      %d\n' "$D_BIN"
printf '    SKIP-PY-ERROR       %d\n' "$SKIP_PY"
echo "=============================================================="
# Diagnostic meter: the divergences are the finding, not the exit code.
exit 0
