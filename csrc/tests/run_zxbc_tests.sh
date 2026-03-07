#!/bin/bash
# run_zxbc_tests.sh — Compare C zxbc exit codes against Python ground truth
#
# Usage:
#   ./csrc/tests/run_zxbc_tests.sh <zxbc-binary> <test-dir> [file.bas ...]
#
# If specific .bas files are listed, only test those.
# Otherwise, test all .bas files in <test-dir>.
#
# Exit codes from Python are cached in csrc/tests/zxbc_expected/*.rc
# (generated once via: see comments in this file)
#
# To regenerate Python baselines:
#   for f in tests/functional/arch/zx48k/*.bas; do
#     bn=$(basename "$f" .bas)
#     python3.12 -c "..." >/dev/null 2>&1; echo $? > csrc/tests/zxbc_expected/${bn}.rc
#   done

BINARY="$1"
TESTDIR="$2"
shift 2

EXPECTED_DIR="$(dirname "$0")/zxbc_expected"

if [ ! -x "$BINARY" ]; then
    echo "Usage: $0 <zxbc-binary> <test-dir> [file.bas ...]" >&2
    exit 2
fi

if [ ! -d "$EXPECTED_DIR" ]; then
    echo "Error: expected exit codes not found at $EXPECTED_DIR" >&2
    echo "Generate them first (see script comments)" >&2
    exit 2
fi

pass=0
fail=0
total=0
false_pos=0
false_neg=0
mismatches=""

# Build file list
if [ $# -gt 0 ]; then
    FILES="$@"
else
    FILES="$TESTDIR"/*.bas
fi

for f in $FILES; do
    bn=$(basename "$f" .bas)
    rc_file="$EXPECTED_DIR/${bn}.rc"

    if [ ! -f "$rc_file" ]; then
        # No Python baseline — skip
        continue
    fi

    expected_rc=$(cat "$rc_file")
    total=$((total + 1))

    "$BINARY" --parse-only "$f" >/dev/null 2>&1
    actual_rc=$?

    # Normalize: anything non-zero is 1
    [ "$actual_rc" != "0" ] && actual_rc=1
    [ "$expected_rc" != "0" ] && expected_rc=1

    if [ "$actual_rc" = "$expected_rc" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        if [ "$actual_rc" = "1" ] && [ "$expected_rc" = "0" ]; then
            false_pos=$((false_pos + 1))
            mismatches="$mismatches  FALSE_POS: $bn (C errors, Python OK)\n"
        else
            false_neg=$((false_neg + 1))
            mismatches="$mismatches  FALSE_NEG: $bn (C OK, Python errors)\n"
        fi
    fi
done

# Summary
echo ""
echo "$total tests: $pass matched, $fail mismatched"
if [ "$false_pos" -gt 0 ]; then
    echo "  $false_pos false positives (C errors, Python OK) — REGRESSIONS"
fi
if [ "$false_neg" -gt 0 ]; then
    echo "  $false_neg false negatives (C OK, Python errors) — need semantic analysis"
fi

if [ -n "$mismatches" ] && [ "$fail" -le 200 ]; then
    echo ""
    echo "Mismatches:"
    printf "$mismatches"
fi

exit $fail
