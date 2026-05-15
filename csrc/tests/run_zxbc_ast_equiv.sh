#!/usr/bin/env bash
# AST-equivalence harness — Sprint 9 / Phase 5 part C.
#
# For each .bas:
#   1. Run csrc/tests/dump_python_ast.py → py_ast.json
#   2. Run csrc/build/zxbc-ast-dump/zxbc-ast-dump → c_ast.json
#   3. Run csrc/tests/diff_ast_json.py → EQUAL or DIFF: <count> sites
#
# Five buckets:
#   EQUAL                        — both dumps succeed, diff reports EQUAL
#   DIFF                         — both dumps succeed, diff reports DIFF
#   SKIP — Python parse error    — Python dump exits non-zero
#   SKIP — C parse error         — C dump exits non-zero
#   SKIP — known Python bug      — stem in csrc/tests/zxbc_python_bugs.txt
#
# Round 0 honest finding (recorded in phase5-aggregate-counts.md): the
# C and Python AST shapes diverge on essentially every program because
# Python optimises declarations out and the two ports use different
# tag namings for compound nodes (Python: bare PRINT; C: PRINT >
# PRINT_ITEM). The aggregate EQUAL count is expected to be very low.
# The harness's value is the per-test DIFF site count + the calibration
# differential, not the EQUAL count itself.
#
# Usage: run_zxbc_ast_equiv.sh <c-ast-dump-binary> <py-dump-py> <diff-py> <bas-test-dir> [file.bas ...]

set -uo pipefail

ZXBC_DUMP="${1:?Usage: $0 <c-ast-dump-binary> <py-dump-py> <diff-py> <bas-test-dir> [file.bas ...]}"
PY_DUMP="${2:?Usage: $0 <c-ast-dump-binary> <py-dump-py> <diff-py> <bas-test-dir> [file.bas ...]}"
DIFF_PY="${3:?Usage: $0 <c-ast-dump-binary> <py-dump-py> <diff-py> <bas-test-dir> [file.bas ...]}"
TEST_DIR="${4:?Usage: $0 <c-ast-dump-binary> <py-dump-py> <diff-py> <bas-test-dir> [file.bas ...]}"
shift 4

ZXBC_DUMP=$(cd "$(dirname "$ZXBC_DUMP")" && pwd)/$(basename "$ZXBC_DUMP")
PY_DUMP=$(cd "$(dirname "$PY_DUMP")" && pwd)/$(basename "$PY_DUMP")
DIFF_PY=$(cd "$(dirname "$DIFF_PY")" && pwd)/$(basename "$DIFF_PY")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done

PYTHON="$PROJECT_ROOT/.venv/bin/python"
[ -x "$PYTHON" ] || PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: no python3.12 found at .venv/bin/python or /opt/homebrew/bin/python3.12" >&2
    exit 2
fi

PYTHON_BUGS="$PROJECT_ROOT/csrc/tests/zxbc_python_bugs.txt"
PY_BUGS_FILE=$(mktemp)
trap 'rm -f "$PY_BUGS_FILE"' EXIT
if [ -f "$PYTHON_BUGS" ]; then
    sed 's/#.*//' "$PYTHON_BUGS" | awk '$1 != "" { print $1 }' > "$PY_BUGS_FILE"
fi
is_known_python_bug() { grep -Fxq "$1" "$PY_BUGS_FILE"; }

EQUAL=0
DIFF=0
SKIP_PY_PARSE=0
SKIP_C_PARSE=0
SKIP_KNOWN_PY_BUG=0
SUM_SITES=0
declare -a EQUAL_NAMES
declare -a DIFF_SITE_LINES   # "stem:count"
declare -a SKIP_PY_NAMES
declare -a SKIP_C_NAMES

cd "$PROJECT_ROOT"

if [ "$#" -gt 0 ]; then
    BAS_FILES=("$@")
else
    BAS_FILES=("$TEST_DIR"/*.bas)
fi

for bas in "${BAS_FILES[@]}"; do
    stem=$(basename "$bas" .bas)

    if is_known_python_bug "$stem"; then
        SKIP_KNOWN_PY_BUG=$((SKIP_KNOWN_PY_BUG + 1))
        continue
    fi

    py_json=$(mktemp); c_json=$(mktemp)

    if ! "$PYTHON" "$PY_DUMP" "$bas" > "$py_json" 2>/dev/null; then
        SKIP_PY_PARSE=$((SKIP_PY_PARSE + 1))
        SKIP_PY_NAMES+=("$stem")
        rm -f "$py_json" "$c_json"
        continue
    fi
    if ! "$ZXBC_DUMP" "$bas" > "$c_json" 2>/dev/null; then
        SKIP_C_PARSE=$((SKIP_C_PARSE + 1))
        SKIP_C_NAMES+=("$stem")
        rm -f "$py_json" "$c_json"
        continue
    fi

    diff_out=$("$PYTHON" "$DIFF_PY" "$py_json" "$c_json")
    summary=$(printf '%s\n' "$diff_out" | tail -1)
    if [ "$summary" = "EQUAL" ]; then
        EQUAL=$((EQUAL + 1))
        EQUAL_NAMES+=("$stem")
    else
        # Summary form: "DIFF: <count> sites"
        sites=$(printf '%s\n' "$summary" | awk '/^DIFF: / { print $2 }')
        [ -z "$sites" ] && sites=0
        DIFF=$((DIFF + 1))
        SUM_SITES=$((SUM_SITES + sites))
        DIFF_SITE_LINES+=("${stem}:${sites}")
    fi

    rm -f "$py_json" "$c_json"
done

TOTAL=$((EQUAL + DIFF + SKIP_PY_PARSE + SKIP_C_PARSE + SKIP_KNOWN_PY_BUG))
AVG_SITES=0
[ "$DIFF" -gt 0 ] && AVG_SITES=$((SUM_SITES / DIFF))

echo "=========================================="
echo "zxbc AST-equivalence harness:"
echo "  EQUAL:                       $EQUAL"
echo "  DIFF:                        $DIFF"
echo "  SKIP (Python parse error):   $SKIP_PY_PARSE"
echo "  SKIP (C parse error):        $SKIP_C_PARSE"
echo "  SKIP (known Python bug):     $SKIP_KNOWN_PY_BUG"
echo "  --"
echo "  Total:                       $TOTAL"
echo "  Sum of DIFF sites:           $SUM_SITES"
echo "  Avg sites per DIFF:          $AVG_SITES"
echo "=========================================="

if [ "${ZXBC_AST_EQUIV_VERBOSE:-0}" = "1" ]; then
    [ "$EQUAL" -gt 0 ] && { echo "EQUAL tests (first 20):"; printf '  %s\n' "${EQUAL_NAMES[@]}" | head -20; }
    [ "$DIFF" -gt 0 ]  && { echo "DIFF site counts (first 20):"; printf '  %s\n' "${DIFF_SITE_LINES[@]}" | head -20; }
fi

# Exit zero — this is a measurement, not a verifier. SKIP-Python-parse
# is informational (Phase 4's domain); SKIP-C-parse highlights the C
# port's parse-failure cases (Round 1 backlog).
exit 0
