#!/usr/bin/env bash
# Phase 1 named calibration — the FOR typed-bounds gate.
#
# Runs probe_for on the constructed typed-bounds fixture and propagates
# the probe's exit code: 0 == FOR bound-expr TYPECAST property matches
# Python (GREEN), non-zero == drift (RED). This is a VERIFIER (it fails
# on drift, the Round-0 verify idiom) — contrast run_semantic_fidelity.sh
# which is a measurement.
#
# Encoded red/green transition (no target polarity flip, no git
# checkout): RED at the Phase 1 parent baseline (S1.1 — C omits the
# TYPECAST Python inserts), GREEN at Phase 1 HEAD (after the S1.2 fix).
# A non-zero exit here at S1.1 is the EXPECTED meter state, not a
# sprint failure.
#
# Usage: run_phase1_calibration.sh <c-ast-dump-binary>

set -uo pipefail

ZXBC_DUMP="${1:?Usage: $0 <c-ast-dump-binary>}"
ZXBC_DUMP=$(cd "$(dirname "$ZXBC_DUMP")" && pwd)/$(basename "$ZXBC_DUMP")

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT="$SCRIPT_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

PYTHON="$PROJECT_ROOT/.venv/bin/python"
[ -x "$PYTHON" ] || PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: no python at .venv/bin/python or /opt/homebrew/bin/python3.12" >&2
    exit 2
fi

cd "$PROJECT_ROOT"

CALIB="$PROJECT_ROOT/csrc/tests/ast_equiv_fixtures/for_typecast_drifted.bas"

echo "verify-phase1-calibration: probe_for on $(basename "$CALIB")"
"$PYTHON" "$PROJECT_ROOT/csrc/tests/probe_for.py" "$CALIB" "$ZXBC_DUMP"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "verify-phase1-calibration: GREEN (FOR TYPECAST property matches Python)"
else
    echo "verify-phase1-calibration: RED (rc=$rc) — FOR TYPECAST drift present" >&2
fi
exit "$rc"
