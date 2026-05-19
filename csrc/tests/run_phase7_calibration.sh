#!/usr/bin/env bash
# Phase 7 named calibration — the END-TO-END full-equivalence gate.
#
# Runs ONE existing functional fixture through the full default pipeline
# (no --output-format / no --parse-only — the real end-to-end compile
# producing the default `.bin`) on Python (the oracle) and the C zxbc,
# from SEPARATE scratch dirs with the SAME relative -o basename, and
# asserts WHOLE-FILE RAW byte-equality of the emitted `.bin` plus equal
# exit code. VERIFIER (fails on drift / absence) — the Round-0 verify
# idiom; contrast the measurement harness run_zxbc_full_tests.sh.
#
# Fixture: csrc/tests/codegen_calibration/calib.bas
#   ("DIM a AS UBYTE / LET a = 1 + 2") — a deliberately minimal but REAL
#   program (typed DIM + constant-fold arithmetic LET), NOT an empty
#   stub. It exercises the full front-to-back pipeline (parse → semantic
#   → codegen → assemble → binary image). Empirically FULL-EQUAL
#   end-to-end at this commit: C `.bin` is byte-identical to Python's and
#   both exit 0. It is the named meter-GREEN anchor for Phase 7. (S7.1/
#   S7.2 left the pipeline byte-faithful for simple typed programs;
#   this is the green that asserts that did not regress.)
#
# The byte-compare is RAW cmp on the binary image — NO awk/normalisation
# anywhere near it (the S7.2g soundness lesson: BSD awk truncates at the
# first invalid multibyte byte and can collapse different binaries to a
# spurious match). The default `.bin` embeds no filename and no
# timestamp, so the same -o basename in separate scratch dirs is
# sufficient and no normalisation is needed or possible.
#
# Usage: run_phase7_calibration.sh <c-zxbc-binary>

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc-binary>}"
ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT="$SCRIPT_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    exit 2
fi

CALIB="$PROJECT_ROOT/csrc/tests/codegen_calibration/calib.bas"
if [ ! -f "$CALIB" ]; then
    echo "verify-phase7-calibration: RED — calibration fixture missing: $CALIB" >&2
    exit 2
fi

# Separate scratch CWDs, SAME relative -o basename (the filename-embed
# linchpin, applied to the default format too). Same mktemp -d
# discipline as the other harnesses.
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_cal7_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

PY_DIR="$SCRATCH/py"
C_DIR="$SCRATCH/c"
mkdir -p "$PY_DIR" "$C_DIR"
py_rc=0
c_rc=0

# --- Python oracle: default end-to-end compile. ---
( cd "$PY_DIR" && "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc', '-o', 'out.bin', '$CALIB']
sys.exit(entry_point() or 0)
" ) > /dev/null 2>&1 || py_rc=$?

if [ "$py_rc" -ne 0 ] || [ ! -s "$PY_DIR/out.bin" ]; then
    echo "verify-phase7-calibration: RED (rc=2) — Python oracle failed to" \
         "emit a .bin for the calibration fixture (env/pin problem, not a" \
         "C-side result)" >&2
    exit 2
fi

# --- C candidate: same argv shape, own scratch CWD. ---
( cd "$C_DIR" && "$ZXBC_C" -o "out.bin" "$CALIB" ) > /dev/null 2>&1 || c_rc=$?

echo "verify-phase7-calibration: zxbc end-to-end (default .bin) on $(basename "$CALIB")"

if [ "$c_rc" -ne 0 ] || [ ! -s "$C_DIR/out.bin" ]; then
    echo "verify-phase7-calibration: RED (rc=1) — C produced no .bin" \
         "(end-to-end pipeline regressed for the calibration fixture)" >&2
    exit 1
fi

if [ "$py_rc" -ne "$c_rc" ]; then
    echo "verify-phase7-calibration: RED (rc=1) — exit code differs" \
         "(py_rc=$py_rc, c_rc=$c_rc)" >&2
    exit 1
fi

# WHOLE-FILE RAW byte-compare — NO normalisation (S7.2g soundness).
if cmp -s "$PY_DIR/out.bin" "$C_DIR/out.bin"; then
    echo "verify-phase7-calibration: GREEN (C .bin byte-identical to Python, rc=$c_rc)"
    rc=0
else
    echo "verify-phase7-calibration: RED (rc=1) — C .bin diverges from Python" \
         "end-to-end (the full pipeline regressed for a simple typed program)" >&2
    rc=1
fi
exit "$rc"
