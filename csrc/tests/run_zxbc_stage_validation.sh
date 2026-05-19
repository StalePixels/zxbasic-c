#!/usr/bin/env bash
# run_zxbc_stage_validation.sh — WIRED-IN multi-stage byte-identical meter.
#
# WHY THIS EXISTS (user, 2026-05-19): the prior meters measured stages in
# ISOLATION — `test-zxbc-codegen` compared codegen ASM *text* and could
# read all-green while the assembled binary diverged massively, because
# nothing gated the *composition* until S7.3a. That is the substantive
# metrics oversight that let the port "get this far and be so divergent".
# This meter makes the oversight structurally impossible: byte-identical
# is a GATED pipeline and each stage is reported with its gate.
#
#   Stage 1  Codegen ASM fidelity   C `zxbc --output-format=asm`  ==  Python, byte-for-byte (path-normalised)
#   Stage 2  Assembly fidelity      assemble THAT identical ASM:  C zxbasm-bin == Python zxbasm-bin
#            GATED on Stage 1: a fixture not S1-EQUAL is S2-BLOCKED —
#            Stage 2 can NEVER pass if Stage 1 fails.
#   Stage 3  Meaningful/end-to-end  default `zxbc -o out.bin`: exit code + binary
#            GATED on Stage 2: not-S2-EQUAL is S3-BLOCKED.
#
# PORT-COMPLETE (this corpus) IFF, modulo only justified skips:
#   S1_DIVERGE=S1_C_ERROR=0 AND S2_DIVERGE=S2_C_ERROR=0 AND S3_DIVERGE=S3_C_ERROR=0.
#
# Diagnostic + gate meter (exit 0 always; verdict is the staged table).
# bash 3.2-safe (macOS /bin/bash): scalar counters, no associative arrays.
# Usage: run_zxbc_stage_validation.sh <zxbc-c> <zxbasm-c> <test-dir>
set -uo pipefail
ZXBC_C="${1:?usage: $0 <zxbc-c> <zxbasm-c> <test-dir>}"
ZXBASM_C="${2:?usage: $0 <zxbc-c> <zxbasm-c> <test-dir>}"
TEST_DIR="${3:?usage: $0 <zxbc-c> <zxbasm-c> <test-dir>}"
PY=/opt/homebrew/bin/python3.12
[ -x "$PY" ] || { echo "ERROR: required interpreter $PY not present." >&2; exit 2; }
ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")
ZXBASM_C=$(cd "$(dirname "$ZXBASM_C")" && pwd)/$(basename "$ZXBASM_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
ROOT="$TEST_DIR"; while [ "$ROOT" != "/" ]; do [ -d "$ROOT/src/lib" ] && break; ROOT=$(dirname "$ROOT"); done
ROOT=$(cd "$ROOT" && pwd -P)
BUGS="$ROOT/csrc/tests/zxbc_python_bugs.txt"
SC=$(mktemp -d); trap 'rm -rf "$SC"' EXIT
norm() { sed "s#$ROOT/##g"; }
is_bug() { [ -f "$BUGS" ] && grep -vE '^[[:space:]]*(#|$)' "$BUGS" | grep -qxF "$1"; }
pyrun() { ( cd "$1" && "$PY" -c "import sys;sys.path.insert(0,'$ROOT');from src.$2.$2 import $3 as e;sys.argv=['$2']+$4;sys.exit(e() or 0)" ); }

CORPUS=0; SKIP_PY=0; SKIP_BUG=0
S1_EQUAL=0; S1_DIVERGE=0; S1_C_ERROR=0
S2_EQUAL=0; S2_DIVERGE=0; S2_C_ERROR=0; S2_BLOCKED=0
S3_EQUAL=0; S3_DIVERGE=0; S3_C_ERROR=0; S3_BLOCKED=0

for bas in "$TEST_DIR"/*.bas; do
  [ -e "$bas" ] || continue
  stem=$(basename "$bas" .bas); CORPUS=$((CORPUS+1))
  if is_bug "$stem"; then SKIP_BUG=$((SKIP_BUG+1)); continue; fi
  d="$SC/$stem"; mkdir -p "$d"

  # ---------- Stage 1: codegen ASM fidelity ----------
  ( cd "$d" && "$ZXBC_C" --output-format=asm -o cA.asm "$bas" ) >/dev/null 2>"$d/cA.e"; cAr=$?
  pyrun "$d" zxbc main "['--output-format=asm','-o','pA.asm','$bas']" >/dev/null 2>"$d/pA.e"; pAr=$?
  if [ "$pAr" -ne 0 ] || [ ! -s "$d/pA.asm" ]; then
    SKIP_PY=$((SKIP_PY+1)); S2_BLOCKED=$((S2_BLOCKED+1)); S3_BLOCKED=$((S3_BLOCKED+1)); rm -rf "$d"; continue
  fi
  if [ "$cAr" -ne 0 ] || [ ! -s "$d/cA.asm" ]; then
    S1_C_ERROR=$((S1_C_ERROR+1)); S2_BLOCKED=$((S2_BLOCKED+1)); S3_BLOCKED=$((S3_BLOCKED+1))
    echo "S1-C-ERROR $stem :: $(grep -iE 'error' "$d/cA.e" | tail -1 | cut -c1-90)"; rm -rf "$d"; continue
  fi
  if ! cmp -s <(norm <"$d/cA.asm") <(norm <"$d/pA.asm"); then
    S1_DIVERGE=$((S1_DIVERGE+1)); S2_BLOCKED=$((S2_BLOCKED+1)); S3_BLOCKED=$((S3_BLOCKED+1))
    echo "S1-DIVERGE $stem :: C $(wc -l <"$d/cA.asm" | tr -d ' ')L/$(wc -c <"$d/cA.asm" | tr -d ' ')B vs Py $(wc -l <"$d/pA.asm" | tr -d ' ')L/$(wc -c <"$d/pA.asm" | tr -d ' ')B :: $(diff <(norm <"$d/cA.asm") <(norm <"$d/pA.asm") | grep -E '^[<>]' | head -1 | cut -c1-50)"
    rm -rf "$d"; continue
  fi
  S1_EQUAL=$((S1_EQUAL+1))

  # ---------- Stage 2: assembly fidelity (GATED on S1-EQUAL) ----------
  cp "$d/pA.asm" "$d/ref.asm"
  ( cd "$d" && "$ZXBASM_C" -o cB.bin ref.asm ) >/dev/null 2>"$d/cB.e"; cBr=$?
  pyrun "$d" zxbasm main "['-o','pB.bin','ref.asm']" >/dev/null 2>"$d/pB.e"; pBr=$?
  if [ "$pBr" -ne 0 ] || [ ! -s "$d/pB.bin" ]; then
    SKIP_PY=$((SKIP_PY+1)); S3_BLOCKED=$((S3_BLOCKED+1))
    echo "S2-SKIP-PY $stem :: Python zxbasm did not assemble Python's own asm (pBr=$pBr)"; rm -rf "$d"; continue
  fi
  if [ "$cBr" -ne 0 ] || [ ! -s "$d/cB.bin" ]; then
    S2_C_ERROR=$((S2_C_ERROR+1)); S3_BLOCKED=$((S3_BLOCKED+1))
    echo "S2-C-ERROR $stem :: S1 identical BUT C zxbasm cannot assemble it :: $(grep -iE 'error' "$d/cB.e" | tail -1 | cut -c1-90)"; rm -rf "$d"; continue
  fi
  if ! cmp -s "$d/cB.bin" "$d/pB.bin"; then
    S2_DIVERGE=$((S2_DIVERGE+1)); S3_BLOCKED=$((S3_BLOCKED+1))
    echo "S2-DIVERGE $stem :: S1 identical BUT C-asm-bin != Py-asm-bin ($(wc -c <"$d/cB.bin" | tr -d ' ')B vs $(wc -c <"$d/pB.bin" | tr -d ' ')B)"; rm -rf "$d"; continue
  fi
  S2_EQUAL=$((S2_EQUAL+1))

  # ---------- Stage 3: meaningful / end-to-end (GATED on S2-EQUAL) ----------
  cd1="$d/c3"; pd1="$d/p3"; mkdir -p "$cd1" "$pd1"
  ( cd "$cd1" && "$ZXBC_C" -o out.bin "$bas" ) >/dev/null 2>"$cd1/e"; c3r=$?
  pyrun "$pd1" zxbc main "['-o','out.bin','$bas']" >/dev/null 2>"$pd1/e"; p3r=$?
  if [ "$p3r" -ne 0 ] || [ ! -s "$pd1/out.bin" ]; then
    SKIP_PY=$((SKIP_PY+1)); echo "S3-SKIP-PY $stem :: Python end-to-end produced no out.bin (p3r=$p3r)"; rm -rf "$d"; continue
  fi
  if [ "$c3r" -ne 0 ] || [ ! -s "$cd1/out.bin" ]; then
    S3_C_ERROR=$((S3_C_ERROR+1)); echo "S3-C-ERROR $stem :: S1+S2 identical BUT C end-to-end failed (c3r=$c3r) :: $(grep -iE 'error' "$cd1/e" | tail -1 | cut -c1-80)"; rm -rf "$d"; continue
  fi
  if [ "$c3r" -ne "$p3r" ] || ! cmp -s "$cd1/out.bin" "$pd1/out.bin"; then
    S3_DIVERGE=$((S3_DIVERGE+1)); echo "S3-DIVERGE $stem :: S1+S2 identical BUT end-to-end differs (rc C=$c3r Py=$p3r; bin $(wc -c <"$cd1/out.bin" | tr -d ' ')B vs $(wc -c <"$pd1/out.bin" | tr -d ' ')B) — exit/pipeline-orchestration"; rm -rf "$d"; continue
  fi
  S3_EQUAL=$((S3_EQUAL+1)); rm -rf "$d"
done

echo "==================== ZXBC STAGE VALIDATION ===================="
printf '  corpus %d   skip-python %d   skip-known-bug %d\n' "$CORPUS" "$SKIP_PY" "$SKIP_BUG"
echo "  -- Stage 1 (codegen ASM == Python, byte-for-byte) --"
printf '     S1-EQUAL %d   S1-DIVERGE %d   S1-C-ERROR %d\n' "$S1_EQUAL" "$S1_DIVERGE" "$S1_C_ERROR"
echo "  -- Stage 2 (assemble that ASM: C-bin == Python-bin) — GATED on S1-EQUAL --"
printf '     S2-EQUAL %d   S2-DIVERGE %d   S2-C-ERROR %d   S2-BLOCKED-by-S1 %d\n' "$S2_EQUAL" "$S2_DIVERGE" "$S2_C_ERROR" "$S2_BLOCKED"
echo "  -- Stage 3 (end-to-end default pipeline: exit+binary) — GATED on S2-EQUAL --"
printf '     S3-EQUAL %d   S3-DIVERGE %d   S3-C-ERROR %d   S3-BLOCKED-by-S2 %d\n' "$S3_EQUAL" "$S3_DIVERGE" "$S3_C_ERROR" "$S3_BLOCKED"
echo "  --------------------------------------------------------------"
if [ "$S1_DIVERGE" -eq 0 ] && [ "$S1_C_ERROR" -eq 0 ] \
   && [ "$S2_DIVERGE" -eq 0 ] && [ "$S2_C_ERROR" -eq 0 ] \
   && [ "$S3_DIVERGE" -eq 0 ] && [ "$S3_C_ERROR" -eq 0 ]; then
  echo "  VERDICT: ALL STAGES GREEN (byte-identical through the gated pipeline)"
else
  echo "  VERDICT: NOT byte-identical — Stage 1 is the root gate (fix S1 first;"
  echo "           S2/S3 cannot pass under S1 failure — that is the point)."
fi
exit 0
