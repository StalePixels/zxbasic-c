#!/usr/bin/env bash
# Regenerate zxbc parse-only baselines from Python ground truth.
#
# Pin-update-only invocation policy: this script is invoked manually
# WHEN THE UPSTREAM PYTHON PIN MOVES. It is NOT a dependency of any test
# target. Casual or scheduled regeneration is forbidden — baseline drift
# would silently shift the spec the C port is being measured against.
#
# Outputs (under csrc/tests/zxbc_parse_expected/):
#   <stem>.rc           — Python --parse-only exit code (0 or non-zero)
#   <stem>.err.expected — normalised Python stderr, only for non-zero exits
#   REGEN_STAMP         — Python interpreter version + path, upstream pin
#                         commit hash, regen date. Audit trail.
#
# The Python-health-check gate runs Python's own functional test suite
# first (test_prepro/test_asm/test_basic). If Python's tests don't pass,
# the script aborts: a broken Python is not a trustworthy spec.
#
# Usage:  csrc/tests/regen_zxbc_baselines.sh [--skip-pytest]
#         The --skip-pytest flag is for development-time iteration only;
#         do NOT use it for the spec-establishing regen at a new pin.

set -uo pipefail

SKIP_PYTEST=0
if [ "${1:-}" = "--skip-pytest" ]; then
    SKIP_PYTEST=1
fi

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: $PYTHON not present. Pin-update regen requires the canonical interpreter." >&2
    exit 2
fi

PROJECT_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)
TEST_DIR="$PROJECT_ROOT/tests/functional/arch/zx48k"
EXPECTED_DIR="$PROJECT_ROOT/csrc/tests/zxbc_parse_expected"
STAMP="$EXPECTED_DIR/REGEN_STAMP"

cd "$PROJECT_ROOT"

if [ "$SKIP_PYTEST" -eq 0 ]; then
    echo "Python-health-check gate: pytest tests/functional/test_prepro.py test_asm.py test_basic.py"
    if ! "$PYTHON" -m pytest -q \
            tests/functional/test_prepro.py \
            tests/functional/test_asm.py \
            tests/functional/test_basic.py; then
        echo "ABORT: Python's own functional tests failed at the held pin." >&2
        echo "       A broken Python is not a trustworthy spec — refusing to regenerate." >&2
        exit 3
    fi
    echo
fi

PIN_HASH=$(git rev-parse HEAD 2>/dev/null || echo unknown)
PYTHON_VER=$("$PYTHON" --version 2>&1 | sed 's/^Python //')
REGEN_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Stderr normalisation: project-root subst (real + logical) + trailing-
# whitespace strip. Same policy as the zxbpp/zxbasm strict harnesses.
normalise_stderr() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); sub(/[ \t]+$/, ""); print }
    '
}

mkdir -p "$EXPECTED_DIR"

echo "Regenerating baselines for $(ls "$TEST_DIR"/*.bas | wc -l | tr -d ' ') .bas files…"

PASS_CNT=0; ERR_CNT=0
for bas in "$TEST_DIR"/*.bas; do
    stem=$(basename "$bas" .bas)
    rc_file="$EXPECTED_DIR/${stem}.rc"
    err_file="$EXPECTED_DIR/${stem}.err.expected"
    err_tmp=$(mktemp)
    rc=0

    "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src import zxbc
sys.argv = ['zxbc', '--parse-only', '$bas']
try:
    rc = zxbc.main() or 0
except SystemExit as e:
    rc = e.code or 0
sys.exit(rc)
" >/dev/null 2> "$err_tmp" || rc=$?

    echo "$rc" > "$rc_file"
    if [ "$rc" -ne 0 ]; then
        normalise_stderr < "$err_tmp" > "$err_file"
        ERR_CNT=$((ERR_CNT + 1))
    else
        rm -f "$err_file"
        PASS_CNT=$((PASS_CNT + 1))
    fi
    rm -f "$err_tmp"
done

cat > "$STAMP" <<EOF
# zxbc parse-only baseline regeneration audit stamp
python:        $PYTHON
python_ver:    $PYTHON_VER
upstream_pin:  $PIN_HASH
regen_date:    $REGEN_DATE
basc_files:    $((PASS_CNT + ERR_CNT))
exit_zero:     $PASS_CNT
exit_nonzero:  $ERR_CNT
EOF

echo
echo "Regeneration complete:"
cat "$STAMP"
