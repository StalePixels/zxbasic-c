#!/usr/bin/env bash
# run_corpus.sh — Python-vs-C parity meter over REAL released ZX BASIC programs.
#
# WHY THIS EXISTS
#   The inherited tests/functional/ corpus and the hand-authored codegen_probes
#   are byte-truth only for codepaths they happen to exercise. Real shipped
#   programs (games, demos, engines from zxbasic.readthedocs.io/released_programs)
#   stress the compiler the way actual authors did: deep includes, inline asm,
#   incbin, banking, large programs. This meter compiles each through BOTH the
#   Python reference oracle and the C port and compares — the same drop-in-
#   replacement contract used by csrc/tests/codegen_probes/run_probes.sh:
#
#     1. exit code           (end-to-end run; must match)
#     2. stderr  — path-normalised (warnings + semantic errors live here)
#     3. stage-1 ASM         (--output-format=asm, path-normalised cmp)
#     4. end-to-end binary   (-o out.bin, RAW cmp — never normalise binary)
#
#   Judged by FIRST divergence (exit, stderr, asm, bin). All four match => EQUAL.
#
#   TIERS (derived, not stored — never goes stale):
#     BINARY-EQUAL    : EQUAL *and* Python produced a real binary  => full
#                       end-to-end byte parity on a self-contained program.
#     FRONTEND-EQUAL  : EQUAL but no binary — both Py and C stop at the same
#                       point (e.g. a missing incbin artifact or external lib),
#                       identically. Still proves preprocess/parse/codegen-to-asm
#                       parity. These are the flyby-fix candidates: supply the
#                       missing piece in fixups/<id>/ to promote to BINARY-EQUAL.
#
#   A divergence (DIFF-*) is a real C-port finding: C does not match Python.
#
#   This is a DIAGNOSTIC meter: it always exits 0. The divergences ARE the
#   finding.
#
# Usage:
#   ./run_corpus.sh                 # every staged program
#   ./run_corpus.sh <id> [<id>...]  # only the named programs
#   ./run_corpus.sh --diff <id>     # one program, dumping the first divergence
#
# bash 3.2-safe (macOS /bin/bash): scalar counters only.

set -uo pipefail

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
MANIFEST="$SELF_DIR/manifest.tsv"
WORK="$SELF_DIR/work"

SHOW_DIFF=0
if [ "${1:-}" = "--diff" ]; then SHOW_DIFF=1; shift; fi
WANT=" $* "

# ---- project root (walk up for src/lib) ----
ROOT="$SELF_DIR"
while [ "$ROOT" != "/" ]; do [ -d "$ROOT/src/lib" ] && break; ROOT=$(dirname "$ROOT"); done
[ -d "$ROOT/src/lib" ] || { echo "ERROR: project root (src/lib) not found above $SELF_DIR" >&2; exit 2; }
ROOT=$(cd "$ROOT" && pwd -P)

# ---- interpreters ----
PY="${PY:-/opt/homebrew/bin/python3.12}"
if [ ! -x "$PY" ]; then
    for c in python3.12 python3.11 python3; do command -v "$c" >/dev/null 2>&1 && { PY=$(command -v "$c"); break; }; done
fi
[ -x "$PY" ] || { echo "ERROR: no python3.12+ oracle found." >&2; exit 2; }
# Pin Python hash seed: upstream Python is hash-seed nondeterministic at -O3
# (while.bas byte-flip; see README + zxbc_python_bugs.txt). Seed 0 makes the
# oracle stable and byte-matches C, so the corpus meter measures a real delta.
export PYTHONHASHSEED=0
ZXBC_C="$ROOT/csrc/build/bin/zxbc"
[ -x "$ZXBC_C" ] || { echo "ERROR: C zxbc not built at $ZXBC_C (cmake --build csrc/build)." >&2; exit 2; }

[ -f "$MANIFEST" ] || { echo "ERROR: no manifest.tsv at $MANIFEST" >&2; exit 2; }

# ---- normalisation: strip absolute roots from TEXT streams only ----
norm_text() {  # arg: a CWD whose absolute form should also be scrubbed
    awk -v r1="$ROOT" -v r2="$1" '
        { gsub(r1,"<ROOT>"); gsub(r2,"<CWD>"); print }'
}
is_py_internal() { grep -qE 'Traceback \(most recent call last\)|ImportError|ModuleNotFoundError' "$1"; }

pyrun() {  # <cwd> <python-list-argv>
    ( cd "$1" && "$PY" -c "
import sys
sys.path.insert(0, '$ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc'] + $2
sys.exit(entry_point() or 0)
" )
}

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_corpus_XXXXXX")
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

T=0; BIN_EQ=0; FE_EQ=0; D_EXIT=0; D_STDERR=0; D_ASM=0; D_BIN=0; SKIP_PY=0; SKIP_NS=0

# build a python-list literal from a flag string + fixed args
pylist() { # args... -> ['a','b',...]
    local out="["; local first=1
    for a in "$@"; do [ $first -eq 1 ] && first=0 || out="$out,"; out="$out'$a'"; done
    echo "$out]"
}

echo "================== RELEASED-PROGRAM CORPUS :: parity meter =================="
echo "  oracle : $PY"
echo "  c-port : $ZXBC_C"
echo "  --------------------------------------------------------------"

while IFS=$'\t' read -r id name origin archive sha atype entry flags notes; do
    case "$id" in ''|'#'*) continue ;; esac
    if [ -n "$*" ] && [ "${WANT/ $id /}" = "$WANT" ]; then continue; fi
    [ "$entry" = "NONE" ] || [ -z "$entry" ] && continue   # libraries / no entry

    T=$((T+1))
    dir="$WORK/$id/$(dirname "$entry")"
    base="$(basename "$entry")"
    if [ ! -f "$WORK/$id/$entry" ]; then
        SKIP_NS=$((SKIP_NS+1)); echo "SKIP-NOSTAGE      $id :: not staged (run ./fetch.sh $id)"; continue
    fi
    dir=$(cd "$dir" && pwd)

    # extra codegen flags shared by both stages (from manifest; default -O2)
    [ -z "$flags" ] && flags="-O2"
    # shellcheck disable=SC2206
    fa=($flags)

    d="$SCRATCH/$id"; mkdir -p "$d"
    py_asm="$d/py.asm"; cy_asm="$d/cy.asm"; py_bin="$d/py.bin"; cy_bin="$d/cy.bin"
    py_ae="$d/py.asm.err"; cy_ae="$d/cy.asm.err"; py_be="$d/py.bin.err"; cy_be="$d/cy.bin.err"
    py_arc=0; cy_arc=0; py_brc=0; cy_brc=0

    # ---- stage-1 asm ----
    pyrun "$dir" "$(pylist "${fa[@]}" --output-format=asm -o "$py_asm" "$base")" >/dev/null 2>"$py_ae" || py_arc=$?
    ( cd "$dir" && "$ZXBC_C" "${fa[@]}" --output-format=asm -o "$cy_asm" "$base" ) >/dev/null 2>"$cy_ae" || cy_arc=$?
    if is_py_internal "$py_ae"; then SKIP_PY=$((SKIP_PY+1)); echo "SKIP-PY-ERROR     $id :: Python internal error (asm stage)"; continue; fi

    # ---- end-to-end binary ----
    pyrun "$dir" "$(pylist "${fa[@]}" -o "$py_bin" "$base")" >/dev/null 2>"$py_be" || py_brc=$?
    ( cd "$dir" && "$ZXBC_C" "${fa[@]}" -o "$cy_bin" "$base" ) >/dev/null 2>"$cy_be" || cy_brc=$?
    if is_py_internal "$py_be"; then SKIP_PY=$((SKIP_PY+1)); echo "SKIP-PY-ERROR     $id :: Python internal error (bin stage)"; continue; fi

    # (1) exit (end-to-end)
    if [ "$py_brc" -ne "$cy_brc" ]; then
        D_EXIT=$((D_EXIT+1)); echo "DIFF-EXIT         $id :: Py=$py_brc C=$cy_brc"
        echo "    Py: $(norm_text "$dir" <"$py_be" | grep -iE 'error|warning' | head -1 | cut -c1-110)"
        echo "    C : $(norm_text "$dir" <"$cy_be" | grep -iE 'error|warning' | head -1 | cut -c1-110)"
        continue
    fi
    # (2) stderr
    norm_text "$dir" <"$py_be" >"$d/py.be.n"; norm_text "$dir" <"$cy_be" >"$d/cy.be.n"
    if ! cmp -s "$d/py.be.n" "$d/cy.be.n"; then
        D_STDERR=$((D_STDERR+1)); echo "DIFF-STDERR       $id :: exit match ($py_brc), normalised stderr differs"
        [ $SHOW_DIFF -eq 1 ] && diff "$d/py.be.n" "$d/cy.be.n" | sed 's/^/      /' | head -20
        continue
    fi
    # (3) stage-1 asm
    pa=0; [ -s "$py_asm" ] && pa=1; ca=0; [ -s "$cy_asm" ] && ca=1
    if [ "$pa" -ne "$ca" ]; then
        D_ASM=$((D_ASM+1)); echo "DIFF-ASM          $id :: asm presence differs (Py=$pa rc=$py_arc  C=$ca rc=$cy_arc)"; continue
    fi
    if [ "$pa" -eq 1 ]; then
        norm_text "$dir" <"$py_asm" >"$d/py.an"; norm_text "$dir" <"$cy_asm" >"$d/cy.an"
        if ! cmp -s "$d/py.an" "$d/cy.an"; then
            D_ASM=$((D_ASM+1)); echo "DIFF-ASM          $id :: stage-1 asm differs (Py $(wc -l <"$d/py.an"|tr -d ' ')L vs C $(wc -l <"$d/cy.an"|tr -d ' ')L)"
            [ $SHOW_DIFF -eq 1 ] && diff "$d/py.an" "$d/cy.an" | grep -E '^[<>]' | head -20 | sed 's/^/      /'
            continue
        fi
    fi
    # (4) binary (raw)
    pb=0; [ -s "$py_bin" ] && pb=1; cb=0; [ -s "$cy_bin" ] && cb=1
    if [ "$pb" -ne "$cb" ]; then
        D_BIN=$((D_BIN+1)); echo "DIFF-BIN          $id :: binary presence differs (Py=$pb C=$cb)"; continue
    fi
    if [ "$pb" -eq 1 ] && ! cmp -s "$py_bin" "$cy_bin"; then
        D_BIN=$((D_BIN+1)); echo "DIFF-BIN          $id :: binary differs (Py $(wc -c <"$py_bin"|tr -d ' ')B vs C $(wc -c <"$cy_bin"|tr -d ' ')B) :: $(cmp "$py_bin" "$cy_bin" 2>&1|head -1)"; continue
    fi

    # EQUAL — derive tier from whether a real binary was produced
    if [ "$pb" -eq 1 ]; then
        BIN_EQ=$((BIN_EQ+1)); echo "BINARY-EQUAL      $id ($(wc -c <"$py_bin"|tr -d ' ')B)"
    else
        FE_EQ=$((FE_EQ+1))
        why=$(norm_text "$dir" <"$py_be" | grep -iE 'error' | head -1 | cut -c1-90)
        echo "FRONTEND-EQUAL    $id :: parity, no binary${why:+ — both stop at: $why}"
    fi
done < "$MANIFEST"

echo "  --------------------------------------------------------------"
echo "  SUMMARY"
printf '    programs tested     %d\n' "$T"
printf '    BINARY-EQUAL        %d   (full end-to-end byte parity)\n' "$BIN_EQ"
printf '    FRONTEND-EQUAL      %d   (parse/codegen parity; flyby-fix candidates)\n' "$FE_EQ"
printf '    DIFF-EXIT           %d\n' "$D_EXIT"
printf '    DIFF-STDERR         %d\n' "$D_STDERR"
printf '    DIFF-ASM            %d\n' "$D_ASM"
printf '    DIFF-BIN            %d\n' "$D_BIN"
printf '    SKIP-PY-ERROR       %d\n' "$SKIP_PY"
printf '    SKIP-NOSTAGE        %d\n' "$SKIP_NS"
echo "============================================================================"
exit 0
