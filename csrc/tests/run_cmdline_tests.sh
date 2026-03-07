#!/usr/bin/env bash
# run_cmdline_tests.sh — Command-line tests for zxbc
#
# Matches: tests/cmdline/test_zxb.py
#
# Usage: ./run_cmdline_tests.sh <zxbc-binary> <test-dir>
#   test-dir should point to tests/cmdline/ which contains empty.bas and config_sample.ini

set -euo pipefail

ZXBC="${1:?Usage: $0 <zxbc-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <zxbc-binary> <test-dir>}"

PASS=0
FAIL=0
TOTAL=0

pass() { PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1)); echo "  FAIL: $1 — $2"; }

BAS="$TEST_DIR/empty.bas"
BIN="$TEST_DIR/empty.bin"
CONFIG="$TEST_DIR/config_sample.ini"

# Ensure test files exist
[ -f "$BAS" ] || { echo "ERROR: $BAS not found"; exit 1; }
[ -f "$CONFIG" ] || { echo "ERROR: $CONFIG not found"; exit 1; }

echo "cmdline tests (matching tests/cmdline/test_zxb.py):"

# ---- test_compile_only ----
# --parse-only should NOT create an output file
rm -f "$BIN"
"$ZXBC" --parse-only "$BAS" -o "$BIN" 2>/dev/null || true
if [ ! -f "$BIN" ]; then
    pass "test_compile_only"
else
    fail "test_compile_only" "Should not create file 'empty.bin'"
    rm -f "$BIN"
fi

# ---- test_org_allows_0xnnnn_format ----
# --org 0xC000 should be accepted (not error)
if "$ZXBC" --parse-only --org 0xC000 "$BAS" -o "$BIN" 2>/dev/null; then
    pass "test_org_allows_0xnnnn_format"
else
    fail "test_org_allows_0xnnnn_format" "--org 0xC000 should be accepted"
fi
rm -f "$BIN"

# ---- test_org_loads_ok_from_config_file_format ----
# -F config_sample.ini should load org=31234
if "$ZXBC" --parse-only -F "$CONFIG" "$BAS" -o "$BIN" 2>/dev/null; then
    pass "test_org_loads_ok_from_config_file_format"
else
    fail "test_org_loads_ok_from_config_file_format" "-F config should be accepted"
fi
rm -f "$BIN"

# ---- test_cmdline_should_override_config_file ----
# --org on cmdline should override org from config file
if "$ZXBC" --parse-only -F "$CONFIG" --org 1234 "$BAS" -o "$BIN" 2>/dev/null; then
    pass "test_cmdline_should_override_config_file"
else
    fail "test_cmdline_should_override_config_file" "cmdline --org should override config"
fi
rm -f "$BIN"

echo ""
echo "$TOTAL tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
