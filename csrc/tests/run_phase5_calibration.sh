#!/usr/bin/env bash
# Phase 5 named calibration — the codegen byte-equivalence gate.
#
# Runs the minimal calibration .bas through Python (the oracle) and the
# C zxbc, normalises both .asm the same way the strict codegen harness
# does (project root -> <PROJECT_ROOT>), and asserts byte-equality.
# VERIFIER (fails on drift / absence) — the Round-0 verify idiom;
# contrast the measurement harness run_zxbc_codegen_tests.sh.
#
# Encoded red/green transition (no target-polarity flip, no git
# checkout): RED at the Phase-5 entry baseline (C exits non-zero at the
# codegen handoff, emits no asm), GREEN once the C translator/emitter
# reproduces Python's asm for this fixture. A non-zero exit here at the
# Phase-5 entry is the EXPECTED meter state, not a sprint failure.
#
# Usage: run_phase5_calibration.sh <c-zxbc-binary>

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc-binary>}"
ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT="$SCRIPT_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    exit 2
fi

CALIB="$PROJECT_ROOT/csrc/tests/codegen_calibration/calib.bas"
if [ ! -f "$CALIB" ]; then
    echo "verify-phase5-calibration: RED — calibration fixture missing: $CALIB" >&2
    exit 2
fi

normalise() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); print }
    '
}

cd "$PROJECT_ROOT"

py_out=$(mktemp /tmp/zxbc_cal_py_XXXXXX.asm)
c_out=$(mktemp /tmp/zxbc_cal_c_XXXXXX.asm)
py_rc=0
c_rc=0

"$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc', '--output-format=asm', '-o', '$py_out', '$CALIB']
sys.exit(entry_point() or 0)
" > /dev/null 2>&1 || py_rc=$?

if [ "$py_rc" -ne 0 ] || [ ! -s "$py_out" ]; then
    echo "verify-phase5-calibration: RED (rc=2) — Python oracle failed to" \
         "emit asm for the calibration fixture (env/pin problem, not a" \
         "C-side result)" >&2
    rm -f "$py_out" "$c_out"
    exit 2
fi

"$ZXBC_C" --output-format=asm -o "$c_out" "$CALIB" > /dev/null 2>&1 || c_rc=$?

echo "verify-phase5-calibration: zxbc codegen on $(basename "$CALIB")"

if [ "$c_rc" -ne 0 ] || [ ! -s "$c_out" ]; then
    echo "verify-phase5-calibration: RED (rc=1) — C emitted no asm" \
         "(codegen not yet implemented at this commit)" >&2
    rm -f "$py_out" "$c_out"
    exit 1
fi

pn=$(mktemp); cn=$(mktemp)
normalise < "$py_out" > "$pn"
normalise < "$c_out"  > "$cn"
if cmp -s "$pn" "$cn"; then
    echo "verify-phase5-calibration: GREEN (C asm byte-identical to Python)"
    rc=0
else
    echo "verify-phase5-calibration: RED (rc=1) — C asm diverges from Python" >&2
    rc=1
fi
rm -f "$py_out" "$c_out" "$pn" "$cn"
exit "$rc"
