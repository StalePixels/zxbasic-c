#!/usr/bin/env bash
# Strict zxbc END-TO-END full-equivalence harness: Python at runtime is
# the oracle. This is the Phase-7 port-complete meter (plan Phase-7 CR#4)
# — the real end-to-end pipeline, not a stage probe.
#
# Usage: run_zxbc_full_tests.sh <c-zxbc-binary> <test-dir>
#
# For each <stem>.bas in the corpus (the test-dir arg verbatim — corpus
# scoping is the Makefile var's job, NOT this script's; no scope cut is
# baked in here, mirroring run_zxbc_codegen_tests.sh):
#
#   Both interpreters run the DEFAULT end-to-end compile with the SAME
#   argv on the SAME fixture:
#       zxbc -o out.bin <abs .bas>
#   i.e. no --output-format / no --parse-only — the real end-to-end
#   pipeline producing the default `.bin`. Each interpreter runs from its
#   OWN scratch dir with an absolute input path and writes to the SAME
#   relative output name `out.bin` (mirroring the codegen harness's
#   scratch discipline, run_zxbc_codegen_tests.sh:72-119): the corpus is
#   read-only upstream fixtures (CLAUDE.md r3) and a default-named CWD
#   artefact must never touch it. Identical -o basename in SEPARATE
#   scratch dirs means any filename-embedding container header cannot
#   spuriously differ (the run_zxbc_outfmt_tests.sh same-output-filename
#   linchpin, applied here for the default format too).
#
# We compare BOTH the exit code AND every emitted output artefact. The
# default pipeline's artefact is the single `out.bin`; any additional
# default-named artefact a stage drops into the scratch CWD is enumerated
# and compared too (so the "every emitted output artefact" contract is
# real, not just `out.bin`).
#
# Five buckets (mirroring run_zxbc_codegen_tests.sh's five-bucket policy
# exactly):
#   FULL-EQUAL          Both exit 0 (or both the SAME non-zero) AND every
#                       emitted artefact byte-identical after the
#                       established path normalisation (text artefacts
#                       normalised project-root -> <PROJECT_ROOT>; BINARY
#                       artefacts compared RAW with cmp, NO awk anywhere
#                       near them — the S7.2g soundness lesson: BSD awk
#                       truncates at the first invalid multibyte byte and
#                       can collapse different binaries to a spurious
#                       match. Binary is detected by a NUL/non-print
#                       probe and cmp'd raw; only path-bearing text is
#                       normalised).
#   FULL-DIFF           Both ran but the exit code differs, OR an artefact
#                       differs. The real backlog number.
#   SKIP-C-error        Python produced output but C errored / produced
#                       none. Per the plan this is NEVER a legitimate
#                       terminal state — it is the S7.3 backlog driver,
#                       NOT a skip that hides. Reported LOUDLY as its own
#                       prominent count and per-case line.
#   SKIP-Python-error   Python exits non-zero / produces no output for
#                       that fixture (stale / environment-specific), and
#                       C did not wrongly succeed. (Python-internal
#                       traceback subset routed here via the codegen
#                       harness's is_python_internal_error.)
#   SKIP-known-Python-bug
#                       Fixture stem listed in zxbc_python_bugs.txt
#                       (the exact shared file + matching logic the
#                       codegen/outfmt harnesses use).
#
# Exit code: 0 always. This is a MEASUREMENT meter exactly like
# test-zxbc-codegen / test-zxbc-outfmt at their phase entry — the
# GREEN/RED judgement is the printed bucket counts, not the script's
# exit. (verify-phase7-calibration is the separate pass/fail gate.)

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc-binary> <test-dir>}"
TEST_DIR="${2:?Usage: $0 <c-zxbc-binary> <test-dir>}"

PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    echo "       Strict harness will not silently fall back to system python3." >&2
    exit 2
fi

ZXBC_C=$(cd "$(dirname "$ZXBC_C")" && pwd)/$(basename "$ZXBC_C")
TEST_DIR=$(cd "$TEST_DIR" && pwd)
PROJECT_ROOT="$TEST_DIR"
while [ "$PROJECT_ROOT" != "/" ]; do
    [ -d "$PROJECT_ROOT/src/lib" ] && break
    PROJECT_ROOT=$(dirname "$PROJECT_ROOT")
done
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

# Text-artefact path normalisation, identical to the codegen harness
# (run_zxbc_codegen_tests.sh:55-59). ONLY ever applied to artefacts that
# pass the is_binary probe as text — never to binary (S7.2g: BSD awk
# truncates at the first invalid multibyte byte; running it over a binary
# can collapse two different binaries to a spurious byte-match).
normalise() {
    awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); print }
    '
}

# Binary detection: a file is BINARY if it contains a NUL byte or any
# non-print, non-whitespace control byte in its first 8 KiB. The default
# zxbc pipeline emits a raw machine-code image (`data` per file(1)), so
# out.bin is binary and goes down the RAW cmp path; an .asm/.lst-style
# text artefact goes down the normalise path. We deliberately do NOT use
# awk/grep-with-text-assumptions for the probe: `LC_ALL=C tr -d` of the
# printable+whitespace set leaves only the "binary" bytes; non-empty =>
# binary. This is byte-exact and multibyte-agnostic.
is_binary() {
    # 0 (true) => binary; 1 (false) => text.
    LC_ALL=C head -c 8192 "$1" 2>/dev/null \
        | LC_ALL=C tr -d '[:print:][:space:]' \
        | LC_ALL=C grep -q . && return 0
    return 1
}

is_python_internal_error() {
    grep -qE 'Traceback \(most recent call last\)|ImportError|ModuleNotFoundError' "$1"
}

# The shared known-Python-bug SKIP list — exact file + matching logic
# from run_zxbc_outfmt_tests.sh:85-89 (the codegen harness uses the same
# file; this is the ONLY sanctioned "Python is wrong here" mechanism).
BUGS_FILE="$PROJECT_ROOT/csrc/tests/zxbc_python_bugs.txt"
is_known_bug() {
    [ -f "$BUGS_FILE" ] || return 1
    grep -vE '^[[:space:]]*(#|$)' "$BUGS_FILE" 2>/dev/null | grep -qxF "$1"
}

FULL_EQUAL=0
FULL_DIFF=0
SKIP_C_ERROR=0
SKIP_PY_ERROR=0
SKIP_KNOWN_BUG=0

# Per-run scratch root; SEPARATE py/ and c/ subdirs so the SAME relative
# output name `out.bin` cannot have a filename-embedding header collide,
# and so any default-named CWD-side artefact lands in scratch (never the
# read-only corpus). Same mktemp -d discipline as the codegen/outfmt
# harnesses (BSD/macOS trailing-X collision immunity).
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/zxbc_full_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

PY_DIR="$SCRATCH/py"
C_DIR="$SCRATCH/c"

# Compare every emitted artefact between the two scratch dirs. Returns 0
# iff the artefact SETS match AND every artefact is byte-equal (binary
# RAW-cmp, text normalised-cmp). Sets DIFF_REASON on mismatch.
DIFF_REASON=""
compare_artefacts() {
    local f rel pa ca
    # Union of emitted file basenames across both scratch dirs (both are
    # flat — the pipeline emits into CWD; no recursion needed, but find
    # is used so an unexpected nested artefact is still caught).
    local -a py_files c_files
    py_files=()
    c_files=()
    while IFS= read -r f; do py_files+=("${f#"$PY_DIR"/}"); done \
        < <(cd "$PY_DIR" 2>/dev/null && find . -type f | sed 's|^\./||' | sort)
    while IFS= read -r f; do c_files+=("${f#"$C_DIR"/}"); done \
        < <(cd "$C_DIR" 2>/dev/null && find . -type f | sed 's|^\./||' | sort)

    local set_py set_c
    set_py=$(printf '%s\n' "${py_files[@]}" | sort -u)
    set_c=$(printf '%s\n' "${c_files[@]}" | sort -u)
    if [ "$set_py" != "$set_c" ]; then
        DIFF_REASON="artefact set differs (py:[$(echo "$set_py" | tr '\n' ' ')] c:[$(echo "$set_c" | tr '\n' ' ')])"
        return 1
    fi

    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        pa="$PY_DIR/$rel"
        ca="$C_DIR/$rel"
        if is_binary "$pa" || is_binary "$ca"; then
            # RAW byte-compare — NO awk/normalise anywhere near a binary
            # artefact (S7.2g soundness lesson).
            if ! cmp -s "$pa" "$ca"; then
                DIFF_REASON="binary artefact '$rel' differs (raw cmp)"
                return 1
            fi
        else
            # Path-bearing text artefact: normalise project root then cmp.
            local pn cn
            pn="$SCRATCH/.pn"
            cn="$SCRATCH/.cn"
            normalise < "$pa" > "$pn"
            normalise < "$ca" > "$cn"
            if ! cmp -s "$pn" "$cn"; then
                rm -f "$pn" "$cn"
                DIFF_REASON="text artefact '$rel' differs (normalised cmp)"
                return 1
            fi
            rm -f "$pn" "$cn"
        fi
    done <<< "$set_py"
    return 0
}

for bas in "$TEST_DIR"/*.bas; do
    [ -e "$bas" ] || continue
    base=$(basename "$bas")
    stem="${base%.bas}"

    if is_known_bug "$stem"; then
        SKIP_KNOWN_BUG=$((SKIP_KNOWN_BUG + 1))
        echo "SKIP-known-Python-bug $stem"
        continue
    fi

    rm -rf "$PY_DIR" "$C_DIR"
    mkdir -p "$PY_DIR" "$C_DIR"
    py_err="$SCRATCH/py_err"
    c_err="$SCRATCH/c_err"
    py_rc=0
    c_rc=0

    # --- Python oracle: default end-to-end compile, own scratch CWD,
    #     absolute input path, SAME relative -o name. ---
    ( cd "$PY_DIR" && "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
from src.zxbc.zxbc import main as entry_point
sys.argv = ['zxbc', '-o', 'out.bin', '$bas']
sys.exit(entry_point() or 0)
" ) > /dev/null 2> "$py_err" || py_rc=$?

    if is_python_internal_error "$py_err"; then
        SKIP_PY_ERROR=$((SKIP_PY_ERROR + 1))
        echo "SKIP-Python-error $stem (python-internal traceback)"
        continue
    fi

    # --- C candidate: same argv shape, own scratch CWD. ---
    ( cd "$C_DIR" && "$ZXBC_C" -o "out.bin" "$bas" ) > /dev/null 2> "$c_err" || c_rc=$?

    py_has=0; [ -s "$PY_DIR/out.bin" ] && py_has=1
    c_has=0;  [ -s "$C_DIR/out.bin" ]  && c_has=1

    # Python did not produce the end-to-end artefact (rc!=0 or no
    # out.bin): a Python-side skip (reject fixture / stale / env), UNLESS
    # C wrongly succeeded — the hard regression. Mirrors the codegen
    # harness FALSE_POS guard semantics; here a C success where Python
    # rejects is reported as FULL-DIFF (a real divergence, never EQUAL).
    if [ "$py_rc" -ne 0 ] || [ "$py_has" -eq 0 ]; then
        if [ "$c_rc" -eq 0 ] && [ "$c_has" -eq 1 ]; then
            FULL_DIFF=$((FULL_DIFF + 1))
            echo "FULL-DIFF $stem :: C produced out.bin where Python did not (py_rc=$py_rc, c_rc=$c_rc) — false success"
        else
            SKIP_PY_ERROR=$((SKIP_PY_ERROR + 1))
            echo "SKIP-Python-error $stem (py_rc=$py_rc, no Python artefact)"
        fi
        continue
    fi

    # Python produced output. C must too — else SKIP-C-error (loud; the
    # S7.3 backlog driver, never a legitimate terminal state).
    if [ "$c_rc" -ne 0 ] || [ "$c_has" -eq 0 ]; then
        SKIP_C_ERROR=$((SKIP_C_ERROR + 1))
        echo "--- SKIP-C-error: $stem (Python produced out.bin; C errored c_rc=$c_rc / no artefact) ---"
        continue
    fi

    # Both ran and both produced out.bin. Compare exit code FIRST, then
    # every emitted artefact.
    if [ "$py_rc" -ne "$c_rc" ]; then
        FULL_DIFF=$((FULL_DIFF + 1))
        echo "FULL-DIFF $stem :: exit code differs (py_rc=$py_rc, c_rc=$c_rc)"
        continue
    fi

    if compare_artefacts; then
        FULL_EQUAL=$((FULL_EQUAL + 1))
        echo "FULL-EQUAL $stem"
    else
        FULL_DIFF=$((FULL_DIFF + 1))
        echo "FULL-DIFF $stem :: $DIFF_REASON"
    fi
done

cleanup
trap - EXIT INT TERM

echo "=== run_zxbc_full_tests === FULL-EQUAL: $FULL_EQUAL / FULL-DIFF: $FULL_DIFF / SKIP-C-error: $SKIP_C_ERROR / SKIP-Python-error: $SKIP_PY_ERROR / SKIP-known-Python-bug: $SKIP_KNOWN_BUG"

# Measurement meter: exit 0 always (like test-zxbc-codegen /
# test-zxbc-outfmt — the bucket counts are the verdict, not this exit;
# verify-phase7-calibration is the pass/fail gate).
exit 0
