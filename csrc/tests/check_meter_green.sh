#!/usr/bin/env bash
# check_meter_green.sh — gate-wrapper for the exit-0-always measurement
# harnesses (zxbc-full, zxbc-stages, zxbc-omatrix, codegen_probes).
#
# Each of those harnesses prints a bucket summary and exits 0 regardless
# of divergences (so the buckets are the verdict, not the exit code).
# The `make test` umbrella needs an exit code to act on, so this wrapper:
#
#   1. Streams the harness output to stdout (so the user still sees it).
#   2. Tees it to a temp log.
#   3. Greps the log for any non-zero divergence/FAIL bucket.
#   4. Exits 0 if every divergence bucket is zero, non-zero otherwise.
#
# Usage:
#   check_meter_green.sh <label> <kind> -- <cmd...>
#
#   <label>   short name printed on the PASS/FAIL line (e.g. "zxbc-full")
#   <kind>    one of: full | stages | omatrix | probes
#             — selects which divergence buckets to check
#   <cmd...>  the harness invocation
#
# Exit codes:
#   0  meter green (zero divergences)
#   1  meter red (one or more divergence bucket non-zero, OR harness
#      didn't print the expected summary line at all)
#   2  bad usage

set -u

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <label> <kind> -- <cmd...>" >&2
    echo "  kind: full | stages | omatrix-zx48k | omatrix-zxnext | probes" >&2
    exit 2
fi
LABEL="$1"
KIND="$2"
if [ "$3" != "--" ]; then
    echo "$0: expected '--' separator after kind, got: $3" >&2
    exit 2
fi
shift 3

LOG=$(mktemp -t "check_meter_${LABEL}.XXXXXX.log")
trap 'rm -f "$LOG"' EXIT INT TERM

"$@" 2>&1 | tee "$LOG"
# tee preserves the harness's exit code via the pipefail-friendly idiom:
PIPESTATUS_HARNESS="${PIPESTATUS[0]:-0}"

# If the harness itself errored (not just a measurement print + exit 0),
# that's RED regardless of bucket counts.
if [ "$PIPESTATUS_HARNESS" -ne 0 ]; then
    echo
    echo "[check_meter_green] ${LABEL}: RED — harness exited ${PIPESTATUS_HARNESS}" >&2
    exit 1
fi

# Per-kind verdict regex: a single grep pattern that matches the *bad*
# state. If any line of the harness output matches, the meter is RED.
case "$KIND" in
    full)
        # run_zxbc_full_tests.sh: "FULL-EQUAL: N / FULL-DIFF: D / SKIP-C-error: E / ..."
        # RED if FULL-DIFF != 0 OR SKIP-C-error != 0.
        if grep -qE 'FULL-DIFF: [1-9]|SKIP-C-error: [1-9]' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — non-zero FULL-DIFF or SKIP-C-error" >&2
            exit 1
        fi
        if ! grep -qE 'FULL-EQUAL: [0-9]+' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — no FULL-EQUAL summary line found" >&2
            exit 1
        fi
        ;;
    stages)
        # run_zxbc_stage_validation.sh: prints "VERDICT: ALL STAGES GREEN"
        # on success and S{1,2,3}-DIVERGE / S{1,2,3}-C-ERROR per-stage.
        if ! grep -q 'VERDICT: ALL STAGES GREEN' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — VERDICT line missing or not GREEN" >&2
            exit 1
        fi
        ;;
    omatrix-zx48k)
        # run_zxbc_omatrix.sh: per-O-level rows
        #   "LEVEL    EQUAL  BIN-DIFF   C-ERR   CRASH   SKIP-Python-error"
        #   "-O0   888     0       0       0      ..."
        # For the zx48k corpus we tolerate up to 3 BIN-DIFF per level —
        # the documented chr/chr1/const6 known-Python-bug entries
        # (csrc/tests/zxbc_python_bugs.txt) surface here as BIN-DIFF at
        # -O1/-O2/-O3. Any C-ERR / CRASH is unconditionally RED.
        # Awk parses the 4 -O rows directly.
        BAD=$(awk '
            /^-O[0-9] / {
                eq=$2; diff=$3; cerr=$4; crash=$5;
                if (diff+0 > 3 || cerr+0 > 0 || crash+0 > 0) {
                    printf("%s diff=%s cerr=%s crash=%s\n", $1, $3, $4, $5);
                }
            }
        ' "$LOG")
        if [ -n "$BAD" ]; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — omatrix non-green rows:" >&2
            printf '  %s\n' "$BAD" >&2
            exit 1
        fi
        if ! grep -qE '^-O0 ' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — no -O0 summary row found" >&2
            exit 1
        fi
        ;;
    omatrix-zxnext)
        # Same harness, zxnext corpus. zxnext is byte-clean across the
        # whole -O matrix (no documented Python-bug SKIPs here), so the
        # bar is zero divergence at every level.
        BAD=$(awk '
            /^-O[0-9] / {
                eq=$2; diff=$3; cerr=$4; crash=$5;
                if (diff+0 > 0 || cerr+0 > 0 || crash+0 > 0) {
                    printf("%s diff=%s cerr=%s crash=%s\n", $1, $3, $4, $5);
                }
            }
        ' "$LOG")
        if [ -n "$BAD" ]; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — omatrix-zxnext non-green rows:" >&2
            printf '  %s\n' "$BAD" >&2
            exit 1
        fi
        if ! grep -qE '^-O0 ' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — no -O0 summary row found" >&2
            exit 1
        fi
        ;;
    probes)
        # codegen_probes/run_probes.sh: per-category summary —
        #     PROBE-DIFF-EXIT / PROBE-DIFF-STDERR / PROBE-DIFF-ASM / PROBE-DIFF-BIN
        # RED if any of those counters is non-zero, anywhere.
        if grep -qE 'PROBE-DIFF-(EXIT|STDERR|ASM|BIN) +[1-9]' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — non-zero PROBE-DIFF-* bucket" >&2
            exit 1
        fi
        if ! grep -q 'PROBE-EQUAL ' "$LOG"; then
            echo
            echo "[check_meter_green] ${LABEL}: RED — no PROBE-EQUAL summary line" >&2
            exit 1
        fi
        ;;
    *)
        echo "$0: unknown kind '$KIND' (expected full|stages|omatrix-zx48k|omatrix-zxnext|probes)" >&2
        exit 2
        ;;
esac

echo
echo "[check_meter_green] ${LABEL}: GREEN"
exit 0
