#!/usr/bin/env bash
# Phase 1 semantic-fidelity meter — S1.1.
#
# Runs each per-construct differential probe over its fixture set and
# reports a per-construct match count. A probe extracts ONE named
# semantic property from Python (live src.zxbc.zxbparser) and C
# (zxbc-ast-dump JSON) and compares; exit 0 = MATCH, 1 = MISMATCH
# (drift), 2 = SKIP (Python/C parse error, or the construct is absent
# from that fixture — not a drift signal).
#
# This is a MEASUREMENT, not a verifier (exit 0 always), mirroring
# run_zxbc_ast_equiv.sh. At S1.1 the corrective constructs (FOR/LET/DIM)
# are expected RED (mismatch) — that is the established meter. They are
# driven to 100% match in S1.2. The Binary regression-guard is expected
# MATCH from day one. verify-phase1-calibration (run_phase1_calibration.sh)
# is the gating target for the named FOR calibration.
#
# Usage: run_semantic_fidelity.sh <c-ast-dump-binary>

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

FIX_DIR="$PROJECT_ROOT/csrc/tests/semantic_fidelity_fixtures"
EQUIV_DIR="$PROJECT_ROOT/csrc/tests/ast_equiv_fixtures"
FOR_CORPUS="$PROJECT_ROOT/tests/functional/arch/zx48k"

run_construct() {
    # $1 label, $2 probe script, $3.. fixtures
    local label="$1" probe="$2"
    shift 2
    local m=0 mm=0 sk=0 n=0
    local f stem rc
    for f in "$@"; do
        [ -f "$f" ] || continue
        n=$((n + 1))
        stem=$(basename "$f" .bas)
        "$PYTHON" "$PROJECT_ROOT/csrc/tests/$probe" "$f" "$ZXBC_DUMP" \
            >/dev/null 2>&1
        rc=$?
        case "$rc" in
            0) m=$((m + 1)) ;;
            1) mm=$((mm + 1)); MISMATCH_NAMES+=("${label}:${stem}") ;;
            *) sk=$((sk + 1)) ;;
        esac
    done
    printf '  %-7s %2d match, %2d mismatch, %2d skip  (of %d)\n' \
        "$label:" "$m" "$mm" "$sk" "$n"
    TOT_M=$((TOT_M + m)); TOT_MM=$((TOT_MM + mm)); TOT_SK=$((TOT_SK + sk))
}

TOT_M=0; TOT_MM=0; TOT_SK=0
declare -a MISMATCH_NAMES

echo "=========================================="
echo "Phase 1 semantic-fidelity meter:"

# FOR: upstream for*.bas corpus + the constructed typed-bounds calibration
#      fixture (plan CR#2). Literal-bound corpus cases parse-fold to 0
#      TYPECAST on both sides (MATCH); the drift manifests on the
#      typed-bounds fixture.
run_construct "FOR" probe_for.py \
    "$FOR_CORPUS"/for*.bas "$EQUIV_DIR/for_typecast_drifted.bas"

run_construct "LET" probe_let.py \
    "$FIX_DIR/let_typecast_drifted.bas"

run_construct "DIM" probe_dim_scalar.py \
    "$FIX_DIR/dim_scalar_placement.bas"

run_construct "BINARY" probe_binary.py \
    "$EQUIV_DIR/binary_expr_faithful.bas"

echo "  --"
printf '  TOTAL   %2d match, %2d mismatch, %2d skip\n' \
    "$TOT_M" "$TOT_MM" "$TOT_SK"
if [ "${#MISMATCH_NAMES[@]}" -gt 0 ]; then
    echo "  Mismatches (drift sites):"
    printf '    %s\n' "${MISMATCH_NAMES[@]}"
fi
echo "=========================================="

# Measurement, not a verifier — the per-construct counts are the meter.
exit 0
