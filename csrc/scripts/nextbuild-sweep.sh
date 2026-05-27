#!/usr/bin/env bash
# Local-only NextBuild Sources sweep — NOT a CI test. Depends on a sibling
# `_ref/NextBuild/` checkout (override location with NEXTBUILD_DIR).
#
# For each top-level .bas program under _ref/NextBuild/Sources/, compiles it
# with BOTH:
#   1. our C zxbc (csrc/build/zxbc/zxbc)            via nextbuild-c.py
#   2. pinned upstream Python (zxbasic-c/src/)      via nextbuild-c.py + NEXTBUILD_BACKEND=oracle
# and compares the resulting binaries.
#
# NOT: a comparison against NextBuild's bundled zxbasic/ (v1.17.1, ~4 years
# stale). That comparison is meaningless for "is our C port faithful to
# current upstream?" — it just reports the drift between v1.17.1 and
# current upstream's added peephole passes. See
# docs/plans/zxbasic-c/research/stage-02-nextbuild-sources-resweep.md
# for the live picture of what's genuinely divergent vs current upstream.
#
# Usage:
#   ./csrc/scripts/nextbuild-sweep.sh                       # full sweep
#   ./csrc/scripts/nextbuild-sweep.sh -k TilemapScroll      # filter by path substring
#
# Output: per-program MATCH / DIVERGE-BIN / C-CRASH / PY-CRASH / BOTH-FAIL
# bucket, then a summary line at the end.
#
# NOT wired into `make test` or CI — purely a local stress-test driver.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ZXBASIC_C_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NEXTBUILD_DIR="${NEXTBUILD_DIR:-$ZXBASIC_C_DIR/../_ref/NextBuild}"
NEXTBUILD_DIR="$(cd "$NEXTBUILD_DIR" 2>/dev/null && pwd)" || {
    echo "ERROR: NextBuild checkout not found at ${NEXTBUILD_DIR:-}" >&2
    echo "       Set \$NEXTBUILD_DIR or symlink _ref/NextBuild." >&2
    exit 2
}
SOURCES_DIR="$NEXTBUILD_DIR/Sources"

. "$ZXBASIC_C_DIR/csrc/tests/_find_python312.sh"
DRIVER="$SCRIPT_DIR/nextbuild-c.py"

FILTER="${1-}"
case "$FILTER" in -k) FILTER="${2-}";; esac

run_backend() {
    # $1 = c|oracle, $2 = bas-file. Writes binary next to .bas (nextbuild-c.py
    # default behaviour). Returns the path it wrote, or empty on no-output.
    local backend="$1" bas="$2"
    local base="${bas%.bas}"
    rm -f "$base.bin" "$base.tap" "$base.nex"
    NEXTBUILD_DIR="$NEXTBUILD_DIR" NEXTBUILD_BACKEND="$backend" \
        "$PYTHON" "$DRIVER" "$bas" >/dev/null 2>&1
    local rc=$?
    # Prefer .bin; fall back to .tap or .nex (post-compile chrome may add wrappers).
    for ext in .bin .tap .nex; do
        if [ -f "$base$ext" ]; then echo "$base$ext"; return $rc; fi
    done
    echo ""
    return $rc
}

PASS=0; DIVERGE=0; C_CRASH=0; PY_CRASH=0; BOTH_FAIL=0; TOTAL=0
declare -a BUCKETS=()

while IFS= read -r bas; do
    rel="${bas#$SOURCES_DIR/}"
    if [ -n "$FILTER" ] && ! echo "$rel" | grep -qF "$FILTER"; then continue; fi
    # Skip library-style .bas (Nextlibs/) — they're #include sources not programs.
    case "$rel" in Nextlibs/*) continue;; esac
    TOTAL=$((TOTAL + 1))

    py_out=$(run_backend oracle "$bas"); py_rc=$?
    py_bin="$py_out"
    [ -n "$py_bin" ] && { py_size=$(wc -c < "$py_bin" | tr -d ' '); cp "$py_bin" /tmp/sweep_py.bin; } || py_size=0

    c_out=$(run_backend c "$bas"); c_rc=$?
    c_bin="$c_out"
    [ -n "$c_bin" ] && { c_size=$(wc -c < "$c_bin" | tr -d ' '); cp "$c_bin" /tmp/sweep_c.bin; } || c_size=0

    if [ -z "$py_bin" ] && [ -z "$c_bin" ]; then
        BUCKETS+=("BOTH-FAIL    $rel  py_rc=$py_rc c_rc=$c_rc")
        BOTH_FAIL=$((BOTH_FAIL + 1))
    elif [ -z "$c_bin" ]; then
        BUCKETS+=("C-CRASH      $rel  py=$py_size c_rc=$c_rc")
        C_CRASH=$((C_CRASH + 1))
    elif [ -z "$py_bin" ]; then
        BUCKETS+=("PY-CRASH     $rel  c=$c_size py_rc=$py_rc")
        PY_CRASH=$((PY_CRASH + 1))
    elif cmp -s /tmp/sweep_py.bin /tmp/sweep_c.bin; then
        BUCKETS+=("MATCH        $rel  $c_size bytes")
        PASS=$((PASS + 1))
    else
        delta=$((c_size - py_size))
        first=$(cmp -l /tmp/sweep_py.bin /tmp/sweep_c.bin 2>/dev/null | head -1 | awk '{print $1}')
        BUCKETS+=("DIVERGE-BIN  $rel  py=$py_size c=$c_size Δ=$delta first-diff-byte=$first")
        DIVERGE=$((DIVERGE + 1))
    fi
done < <(find "$SOURCES_DIR" -name "*.bas" -type f | sort)

printf '%s\n' "${BUCKETS[@]}"
echo "=========================================="
printf 'NextBuild sweep (oracle = pinned upstream Python at src/):\n'
printf '  MATCH:       %d\n' "$PASS"
printf '  DIVERGE-BIN: %d\n' "$DIVERGE"
printf '  C-CRASH:     %d\n' "$C_CRASH"
printf '  PY-CRASH:    %d\n' "$PY_CRASH"
printf '  BOTH-FAIL:   %d\n' "$BOTH_FAIL"
printf '  TOTAL:       %d\n' "$TOTAL"
echo "=========================================="

# Exit code = number of genuine divergences (DIVERGE-BIN + C-CRASH).
# BOTH-FAIL doesn't count — those are corpus hygiene (missing assets etc.),
# not port bugs. PY-CRASH wouldn't count either (would be an upstream bug).
exit $((DIVERGE + C_CRASH))
