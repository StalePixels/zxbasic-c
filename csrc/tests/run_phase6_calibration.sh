#!/usr/bin/env bash
# Phase 6 named calibration — the outfmt (.tap) byte-equivalence gate.
#
# Runs the minimal calibration .bas through Python (the oracle) and the
# C zxbc, both asked for --output-format=tap with the SAME -o basename
# (the .tap/.tzx embedded-title linchpin: asmparse.py:1043-1044 ->
# tzx.py:95-99), and asserts WHOLE-FILE raw byte-equality (NO
# normalisation — test.py:179-180). VERIFIER (fails on drift / absence)
# — the Round-0 verify idiom; contrast the measurement harness
# run_zxbc_outfmt_tests.sh.
#
# Encoded red/green transition (no target-polarity flip, no git
# checkout): RED at the Phase-6 entry baseline (C has no container
# generator — codegen.c writes the asm text into the .tap path,
# ignoring output_file_type), GREEN once the C .tap generator
# reproduces Python's tape bytes for this fixture (S6.3). A non-zero
# exit here at the Phase-6 entry is the EXPECTED meter state, not a
# sprint failure.
#
# Usage: run_phase6_calibration.sh <c-zxbc-binary>

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

CALIB="$PROJECT_ROOT/csrc/tests/outfmt_calibration/calib.bas"
if [ ! -f "$CALIB" ]; then
    echo "verify-phase6-calibration: RED — calibration fixture missing: $CALIB" >&2
    exit 2
fi

INCLUDE_PATH="$PROJECT_ROOT/stdlib:$PROJECT_ROOT/runtime"

# Scratch dir with the SAME -o basename for Python and C (the
# embedded-title linchpin). Sequenced over one path; Python's bytes are
# snapshotted before the C run reuses it.
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_cal6_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

out_file="$SCRATCH/calib.tap"
py_snap="$SCRATCH/py_snap.tap"
py_rc=0
c_rc=0

# --- Python oracle (the exact test.py container option set) ---
( cd "$SCRATCH" && "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc', '-O1', '--arch', 'zx48k', '--output-format=tap',
            '$CALIB', '-o', '$out_file', '-a', '-B', '-e', '/dev/null',
            '-I', '$INCLUDE_PATH']
sys.exit(entry_point() or 0)
" ) > /dev/null 2>&1 || py_rc=$?

if [ "$py_rc" -ne 0 ] || [ ! -s "$out_file" ]; then
    echo "verify-phase6-calibration: RED (rc=2) — Python oracle failed to" \
         "emit a .tap for the calibration fixture (env/pin problem, not a" \
         "C-side result)" >&2
    exit 2
fi
cp "$out_file" "$py_snap"
rm -f "$out_file"

# --- C candidate (same -o basename) ---
( cd "$SCRATCH" && "$ZXBC_C" -O1 --arch zx48k --output-format=tap \
    "$CALIB" -o "$out_file" -a -B -e /dev/null -I "$INCLUDE_PATH" ) \
    > /dev/null 2>&1 || c_rc=$?

echo "verify-phase6-calibration: zxbc .tap on $(basename "$CALIB")"

if [ "$c_rc" -ne 0 ] || [ ! -s "$out_file" ]; then
    echo "verify-phase6-calibration: RED (rc=1) — C emitted no .tap" \
         "(outfmt container generator not yet implemented at this commit)" >&2
    exit 1
fi

# WHOLE-FILE raw byte-compare, NO normalisation (test.py:179-180).
if cmp -s "$py_snap" "$out_file"; then
    echo "verify-phase6-calibration: GREEN (C .tap byte-identical to Python)"
    rc=0
else
    echo "verify-phase6-calibration: RED (rc=1) — C .tap diverges from Python" \
         "(C emits asm text into the container path at this baseline)" >&2
    rc=1
fi
exit "$rc"
