#!/usr/bin/env bash
# Mechanised C-vs-Python CLI parity harness for sprint S7.2a–f.
#
# Usage: run_cmdline_parity.sh <c-zxbc> <c-zxbpp> <c-zxbasm> <project-root>
#
# WHY THIS EXISTS
# ---------------
# S7.2a–f ported the zxbc/zxbpp/zxbasm flag surface (output-format
# selection, deprecated-flag warnings, tape-append, save-config, the
# argparse-faithful validation/mutex/format gates, --opt-strategy, the
# zxbpp/zxbasm flag-rejection narrowing). Each sub-slice was hand-verified
# against the Python reference once. This harness locks that parity
# against regression end-to-end: it actually invokes BOTH the C binary
# and python3.12 on the same argv + fixture and compares exit code,
# the error-message content, and (for output-producing scenarios) the
# output bytes. It is the complementary byte/exit gate to the existing
# fast struct-level cmdline unit test (CMake `cmdline_value_tests` /
# csrc/tests/test_cmdline.c) — that one stays untouched.
#
# This is a REAL gate, never a do-nothing script (the Round-0 ethos
# recorded at csrc/tests/CMakeLists.txt:86-93 — a test that "passes if
# the binary is replaced with a do-nothing script" is worthless). Point
# the C-binary var at /bin/echo and every case FAILs loudly.
#
# THE EQUIVALENCE CONTRACT (read carefully)
# -----------------------------------------
# For each (entrypoint, scenario, fixture) we run the C binary and the
# Python reference with the SAME argv (passed as a proper array — never
# a single word-split string; an early verifier was bitten by this) on
# the SAME fixture, and assert this triple:
#
#  1. exit code — must be exactly equal.
#
#  2. stderr — we compare the `error:` MESSAGE CONTENT, not the whole
#     stream. Python uses argparse: error paths emit a multi-line
#     `usage: …` preamble then `<prog>: error: <MSG>` and exit 2. getopt
#     fundamentally cannot reproduce the `usage:` block, the exact
#     argparse `unrecognized arguments: <token-list>`, or the -h/--help
#     body — those are a DOCUMENTED CARRIED user-adjudication, NOT this
#     harness's concern and NOT a failure. The faithful, byte-required
#     part is: same exit code, and the same error message — extract from
#     Python the text after the LAST `: error: ` on its final `error:`
#     line, from C the text after the LAST `error: ` on its final
#     `error:` line, and compare those (see py_errmsg/c_errmsg). For
#     success (exit 0) we compare nothing in stderr UNLESS the scenario
#     explicitly produces meaningful stderr — the S7.2b deprecation
#     `WARNING:` lines ARE byte-meaningful and are compared exactly
#     (assert_warning).
#
#     Scenarios where the argparse `unrecognized arguments: <list>` is
#     the only divergence (zxbpp's hard rejection of --version / -D /
#     -I) are EXIT-CODE-ONLY by the contract: both must exit 2, the
#     message text is the carried argparse item and is NOT compared.
#     This harness gates S7.2a–f, not the whole punchlist; the
#     `usage:` preamble, the exact `unrecognized arguments:` token
#     list, the -h/--help body, and the pre-existing zxbpp STDIN
#     `#line "<stdin>"` vs `"(stdin)"` are owner-attributed pre-existing
#     residue and MUST NOT be spurious failures here (no STDIN scenario
#     is included, by design).
#
#  3. output bytes — when the scenario writes an output file (`-o`) or
#     emits stdout, compare it byte-for-byte (cmp) after path
#     normalisation: the absolute project root can appear in #line/asm,
#     so strip $REAL_PROJECT_ROOT/ and $PROJECT_ROOT/ from both sides
#     before cmp (the exact idiom reused from run_zxbpp_tests.sh /
#     run_zxbc_codegen_tests.sh). The TAP/TZX header embeds
#     basename(outputfile)[:10] — so tape scenarios write C and Python
#     to the SAME output filename (in SEPARATE temp dirs) or the headers
#     differ spuriously (this bit an earlier verifier; baked in here via
#     run_in_cwd writing identical relative -o names under cdir/pdir).
#
# Validation/exit-2 cases compare 1+2. Output-producing cases compare
# 1+2+3. Each case prints `PASS <id>` or `FAIL <id> :: <what differed
# verbatim>`; the run ends with `cmdline-parity: N PASS / M FAIL` and
# exits non-zero iff M>0.

set -uo pipefail

ZXBC_C="${1:?Usage: $0 <c-zxbc> <c-zxbpp> <c-zxbasm> <project-root>}"
ZXBPP_C="${2:?Usage: $0 <c-zxbc> <c-zxbpp> <c-zxbasm> <project-root>}"
ZXBASM_C="${3:?Usage: $0 <c-zxbc> <c-zxbpp> <c-zxbasm> <project-root>}"
PROJECT_ROOT_ARG="${4:?Usage: $0 <c-zxbc> <c-zxbpp> <c-zxbasm> <project-root>}"

# Hard-required interpreter — mirror run_zxbpp_tests.sh:25-29. No silent
# fallback to system python3.
PYTHON=/opt/homebrew/bin/python3.12
if [ ! -x "$PYTHON" ]; then
    echo "ERROR: required interpreter $PYTHON not present." >&2
    echo "       Parity harness will not silently fall back to system python3." >&2
    exit 2
fi

# Absolute-path the C binaries (idiom from run_zxbpp_tests.sh:32) so they
# resolve from any CWD. Do NOT require them executable here — the
# do-nothing-script sanity check deliberately points these at /bin/echo.
abspath() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
ZXBC_C=$(abspath "$ZXBC_C")
ZXBPP_C=$(abspath "$ZXBPP_C")
ZXBASM_C=$(abspath "$ZXBASM_C")

PROJECT_ROOT=$(cd "$PROJECT_ROOT_ARG" && pwd)
REAL_PROJECT_ROOT=$(cd "$PROJECT_ROOT" && pwd -P)

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FIX="$SCRIPT_DIR/cmdline_parity"
for f in ifdef.bas p.bas t.asm t.bi blob.bin blob2.bin; do
    if [ ! -f "$FIX/$f" ]; then
        echo "ERROR: missing fixture $FIX/$f" >&2
        exit 2
    fi
done
IFDEF="$FIX/ifdef.bas"
PBAS="$FIX/p.bas"
TASM="$FIX/t.asm"
TBI="$FIX/t.bi"
BLOB="$FIX/blob.bin"
BLOB2="$FIX/blob2.bin"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/cmdline_parity_XXXXXX")
if [ -z "$SCRATCH" ] || [ ! -d "$SCRATCH" ]; then
    echo "ERROR: could not create scratch dir; refusing to run." >&2
    exit 2
fi
cleanup() { rm -rf "$SCRATCH"; }
trap cleanup EXIT INT TERM

# Path normalisation, byte-identical idiom to run_zxbpp_tests.sh:52-56 /
# run_zxbc_codegen_tests.sh:55-59. Strips the absolute project root (both
# the symlink-resolved and the as-passed form) so #line / asm path
# embeddings compare equal between C and Python.
normalise() {
    LC_ALL=C awk -v r1="$REAL_PROJECT_ROOT" -v r2="$PROJECT_ROOT" '
        { gsub(r1, "<PROJECT_ROOT>"); gsub(r2, "<PROJECT_ROOT>"); print }
    '
}

# Extract the error-message CONTENT from a stderr file.
#  Python (argparse): last line matching `: error: ` -> text after the
#                     LAST `: error: ` occurrence.
#  C (getopt port):   last line matching `error: ` -> text after the
#                     LAST `error: ` occurrence.
# Trailing whitespace stripped (the other strict harnesses do the same).
py_errmsg() {
    grep ': error: ' "$1" 2>/dev/null | tail -1 | \
        sed -E 's/^.*: error: //; s/[ \t]+$//'
}
c_errmsg() {
    grep 'error: ' "$1" 2>/dev/null | tail -1 | \
        sed -E 's/^.*error: //; s/[ \t]+$//'
}

PASS=0
FAIL=0
declare -a FAIL_LINES

ok()   { PASS=$((PASS + 1)); echo "PASS $1"; }
bad()  { FAIL=$((FAIL + 1)); FAIL_LINES+=("$1 :: $2"); echo "FAIL $1 :: $2"; }

# run_c <cwd> <outfile-or-->  -- runs the C binary named in $1 of caller;
# helper sets globals C_RC, and writes stdout to $C_OUT, stderr $C_ERR.
# We pass the binary + argv explicitly through a small wrapper so the
# argv is ALWAYS a real array (never word-split — the bug that bit the
# earlier verifier and made --arch bogus look divergent).
run_c() {
    local cwd="$1"; shift
    C_OUT=$(mktemp "$SCRATCH/c_out.XXXXXX")
    C_ERR=$(mktemp "$SCRATCH/c_err.XXXXXX")
    C_RC=0
    ( cd "$cwd" && "$@" ) > "$C_OUT" 2> "$C_ERR" || C_RC=$?
}

# run_py <cwd> <module-spec> <argv0> [args...]  module-spec is one of
# zxbc|zxbpp|zxbasm; argv passed as a real list into sys.argv.
run_py() {
    local cwd="$1"; local kind="$2"; shift 2
    local importline
    case "$kind" in
        zxbc)   importline="from src.zxbc.zxbc import main as e" ;;
        zxbpp)  importline="from src.zxbpp.zxbpp import entry_point as e" ;;
        zxbasm) importline="from src.zxbasm.zxbasm import main as e" ;;
    esac
    # build the python sys.argv list literal safely
    local pyargs=""
    local a
    for a in "$@"; do
        # single-quote-escape each token for the python literal
        local esc=${a//\'/\'\\\'\'}
        pyargs="$pyargs, '$esc'"
    done
    P_OUT=$(mktemp "$SCRATCH/p_out.XXXXXX")
    P_ERR=$(mktemp "$SCRATCH/p_err.XXXXXX")
    P_RC=0
    ( cd "$cwd" && "$PYTHON" -c "
import sys
sys.path.insert(0, '$PROJECT_ROOT')
$importline
sys.argv = ['$kind'$pyargs]
sys.exit(e() or 0)
" ) > "$P_OUT" 2> "$P_ERR" || P_RC=$?
}

# --- comparison primitives -------------------------------------------

assert_rc() {
    # $1 id ; uses C_RC / P_RC
    if [ "$C_RC" -ne "$P_RC" ]; then
        return 1
    fi
    return 0
}

# byte-compare two files; sets DIFF_TXT on mismatch.
#
# SOUNDNESS NOTE (a real bug found + fixed in this harness): the awk
# path-normaliser MUST NOT see binary content. macOS/BSD awk aborts a
# record at the first invalid multibyte sequence ("towc: multibyte
# conversion failure") and truncates output there — so two DIFFERENT
# .tap/.tzx/.sna/.bin files both collapse to identical garbage and a
# normalised cmp would spuriously PASS. LC_ALL=C does not fix macOS awk
# here. A project-root path string also cannot meaningfully appear
# inside a binary tape/snapshot/bin container, so there is nothing to
# normalise there anyway. Therefore: detect binary (NUL byte present)
# and cmp it RAW byte-for-byte with no normalisation; only text outputs
# (.asm / .out / stdout / INI — where #line / asm path embeddings live)
# go through the project-root strip.
# Binary iff the file contains any byte outside [:print:]+[:space:].
# LC_ALL=C makes tr operate byte-wise (so high-bit bytes count). This is
# portable on macOS/BSD where `grep`'s binary heuristics and negated
# POSIX classes are unreliable for high-bit bytes.
is_binary() {
    [ -n "$(LC_ALL=C tr -d '[:print:][:space:]' < "$1" 2>/dev/null | head -c1)" ]
}

norm_cmp() {
    local pa="$1" pb="$2"
    if is_binary "$pa" || is_binary "$pb"; then
        # raw byte compare — no awk anywhere near binary content
        if cmp -s "$pa" "$pb"; then
            return 0
        fi
        DIFF_TXT="(binary) $(cmp "$pa" "$pb" 2>&1 | head -1)"
        return 1
    fi
    local na nb
    na=$(mktemp "$SCRATCH/na.XXXXXX"); nb=$(mktemp "$SCRATCH/nb.XXXXXX")
    normalise < "$pa" > "$na"
    normalise < "$pb" > "$nb"
    if cmp -s "$na" "$nb"; then
        rm -f "$na" "$nb"; return 0
    fi
    DIFF_TXT=$(cmp "$na" "$nb" 2>&1 | head -1)
    rm -f "$na" "$nb"; return 1
}

clean_run_files() { rm -f "$C_OUT" "$C_ERR" "$P_OUT" "$P_ERR" 2>/dev/null; }

# ---------------------------------------------------------------------
# CASE RUNNERS
# ---------------------------------------------------------------------

# validation/exit-2 case: compare rc + error-message content (items 1+2)
case_err() {
    local id="$1"; local kind="$2"; shift 2
    local cdir="$SCRATCH/c.$id"; local pdir="$SCRATCH/p.$id"
    mkdir -p "$cdir" "$pdir"
    local cbin
    case "$kind" in
        zxbc) cbin="$ZXBC_C" ;; zxbpp) cbin="$ZXBPP_C" ;; zxbasm) cbin="$ZXBASM_C" ;;
    esac
    run_c "$cdir" "$cbin" "$@"
    run_py "$pdir" "$kind" "$@"
    if ! assert_rc; then
        bad "$id" "exit code C=$C_RC Python=$P_RC"
        clean_run_files; return
    fi
    local cm pm
    cm=$(c_errmsg "$C_ERR"); pm=$(py_errmsg "$P_ERR")
    if [ "$cm" != "$pm" ]; then
        bad "$id" "error msg C=[$cm] Python=[$pm]"
        clean_run_files; return
    fi
    ok "$id"
    clean_run_files
}

# exit-code-only case: the error MESSAGE is the carried argparse
# `unrecognized arguments: <token-list>` item — both must exit 2, the
# message text is NOT compared (documented carried user-adjudication).
case_rc_only() {
    local id="$1"; local kind="$2"; local want="$3"; shift 3
    local cdir="$SCRATCH/c.$id"; local pdir="$SCRATCH/p.$id"
    mkdir -p "$cdir" "$pdir"
    local cbin
    case "$kind" in
        zxbc) cbin="$ZXBC_C" ;; zxbpp) cbin="$ZXBPP_C" ;; zxbasm) cbin="$ZXBASM_C" ;;
    esac
    run_c "$cdir" "$cbin" "$@"
    run_py "$pdir" "$kind" "$@"
    if [ "$C_RC" -ne "$P_RC" ]; then
        bad "$id" "exit code C=$C_RC Python=$P_RC (rc-only case; argparse token-list is carried)"
        clean_run_files; return
    fi
    if [ "$C_RC" -ne "$want" ]; then
        bad "$id" "expected exit $want, both gave $C_RC"
        clean_run_files; return
    fi
    ok "$id"
    clean_run_files
}

# success + exact-WARNING-stderr case (S7.2b deprecation lines are
# byte-meaningful). $3 = expected-warning-substring-marker: 'WARN' means
# both stderrs must be byte-identical AND non-empty; 'NOWARN' means both
# stderrs must be empty. rc must match and (unless ignore_out=1) the -o
# output bytes are compared too.
case_warn() {
    local id="$1"; local kind="$2"; local mode="$3"; local ignore_out="$4"
    local outname="$5"; shift 5
    local cdir="$SCRATCH/c.$id"; local pdir="$SCRATCH/p.$id"
    mkdir -p "$cdir" "$pdir"
    local cbin
    case "$kind" in
        zxbc) cbin="$ZXBC_C" ;; zxbpp) cbin="$ZXBPP_C" ;; zxbasm) cbin="$ZXBASM_C" ;;
    esac
    run_c "$cdir" "$cbin" "$@"
    run_py "$pdir" "$kind" "$@"
    if ! assert_rc; then
        bad "$id" "exit code C=$C_RC Python=$P_RC"
        clean_run_files; return
    fi
    # stderr WARNING contract (byte-exact, path-normalised so any
    # embedded path is comparable)
    local ce pe
    ce=$(mktemp "$SCRATCH/ce.XXXXXX"); pe=$(mktemp "$SCRATCH/pe.XXXXXX")
    normalise < "$C_ERR" | sed -E 's/[ \t]+$//' > "$ce"
    normalise < "$P_ERR" | sed -E 's/[ \t]+$//' > "$pe"
    if [ "$mode" = "NOWARN" ]; then
        if [ -s "$ce" ] || [ -s "$pe" ]; then
            bad "$id" "expected empty stderr; C=[$(tr '\n' '|' <"$ce")] Python=[$(tr '\n' '|' <"$pe")]"
            rm -f "$ce" "$pe"; clean_run_files; return
        fi
    else
        if ! cmp -s "$ce" "$pe"; then
            bad "$id" "WARNING stderr differs: C=[$(tr '\n' '|' <"$ce")] Python=[$(tr '\n' '|' <"$pe")]"
            rm -f "$ce" "$pe"; clean_run_files; return
        fi
        if [ ! -s "$ce" ]; then
            bad "$id" "expected a WARNING line, stderr empty"
            rm -f "$ce" "$pe"; clean_run_files; return
        fi
    fi
    rm -f "$ce" "$pe"
    if [ "$ignore_out" != "1" ] && [ -n "$outname" ]; then
        if [ ! -f "$cdir/$outname" ] || [ ! -f "$pdir/$outname" ]; then
            bad "$id" "missing output file $outname (C:$([ -f "$cdir/$outname" ] && echo y || echo n) Py:$([ -f "$pdir/$outname" ] && echo y || echo n))"
            clean_run_files; return
        fi
        if ! norm_cmp "$cdir/$outname" "$pdir/$outname"; then
            bad "$id" "output bytes differ ($outname): $DIFF_TXT"
            clean_run_files; return
        fi
    fi
    ok "$id"
    clean_run_files
}

# success + output-bytes case (no meaningful stderr): compare rc, then
# the -o output file (outname) OR stdout (outname == '@stdout') byte-for
# -byte after path normalisation. Tape scenarios pass the SAME relative
# outname for C and Python (separate dirs) so the TAP/TZX
# basename(outputfile)[:10] header is identical.
case_out() {
    local id="$1"; local kind="$2"; local outname="$3"; shift 3
    local cdir="$SCRATCH/c.$id"; local pdir="$SCRATCH/p.$id"
    mkdir -p "$cdir" "$pdir"
    local cbin
    case "$kind" in
        zxbc) cbin="$ZXBC_C" ;; zxbpp) cbin="$ZXBPP_C" ;; zxbasm) cbin="$ZXBASM_C" ;;
    esac
    run_c "$cdir" "$cbin" "$@"
    run_py "$pdir" "$kind" "$@"
    if ! assert_rc; then
        bad "$id" "exit code C=$C_RC Python=$P_RC (C stderr: $(c_errmsg "$C_ERR"))"
        clean_run_files; return
    fi
    if [ "$C_RC" -ne 0 ]; then
        bad "$id" "expected exit 0, both gave $C_RC"
        clean_run_files; return
    fi
    local cf pf
    if [ "$outname" = "@stdout" ]; then
        cf="$C_OUT"; pf="$P_OUT"
    else
        cf="$cdir/$outname"; pf="$pdir/$outname"
        if [ ! -f "$cf" ] || [ ! -f "$pf" ]; then
            bad "$id" "missing output $outname (C:$([ -f "$cf" ] && echo y || echo n) Py:$([ -f "$pf" ] && echo y || echo n))"
            clean_run_files; return
        fi
    fi
    if ! norm_cmp "$cf" "$pf"; then
        bad "$id" "output bytes differ ($outname): $DIFF_TXT"
        clean_run_files; return
    fi
    ok "$id"
    clean_run_files
}

echo "=================================================="
echo "cmdline parity harness — S7.2a–f C-vs-Python gate"
echo "  C zxbc  : $ZXBC_C"
echo "  C zxbpp : $ZXBPP_C"
echo "  C zxbasm: $ZXBASM_C"
echo "  Python  : $PYTHON"
echo "=================================================="

# ===================================================================
# zxbc
# ===================================================================

# --- S7.2a: -D selects #ifdef branch; asm bytes (path-norm) equal -----
case_out s7.2a-D-FOO       zxbc out.asm -D FOO --output-format=asm -o out.asm "$IFDEF"
case_out s7.2a-no-D        zxbc out.asm        --output-format=asm -o out.asm "$IFDEF"
# Belt-and-braces: -D must actually flip the encoded branch (the two
# above already prove byte-equality with Python; this proves the flag is
# not a no-op by asserting the two C outputs differ).
{
    da="$SCRATCH/da"; db="$SCRATCH/db"; mkdir -p "$da" "$db"
    ( cd "$da" && "$ZXBC_C" -D FOO --output-format=asm -o o.asm "$IFDEF" ) >/dev/null 2>&1
    ( cd "$db" && "$ZXBC_C"        --output-format=asm -o o.asm "$IFDEF" ) >/dev/null 2>&1
    if [ -f "$da/o.asm" ] && [ -f "$db/o.asm" ] && ! cmp -s "$da/o.asm" "$db/o.asm"; then
        ok "s7.2a-D-flips-branch"
    else
        bad "s7.2a-D-flips-branch" "-D FOO produced identical asm to no-D (flag is a no-op?)"
    fi
}

# --- S7.2b: deprecated-flag WARNING lines (byte-meaningful) -----------
case_warn s7.2b-asm         zxbc WARN   1 ""        --asm           -o out.asm "$PBAS"
case_warn s7.2b-tap         zxbc WARN   1 ""        --tap           -o out.tap "$PBAS"
case_warn s7.2b-tzx-T       zxbc WARN   1 ""        -T              -o out.tzx "$PBAS"
# -E (--emit-backend) needs -o or C errs rc=5 while Python rc=0 — a real
# divergence in the *IR-format byte content* path (Phase 5/6 territory),
# NOT what S7.2b gates. S7.2b gates the deprecation WARNING + exit code,
# so we run with -o (rc parity) and ignore_out=1 (do NOT byte-compare
# the IR — that residue is owner-attributed, not this harness's S7.2b
# concern).
case_warn s7.2b-E           zxbc WARN   1 ""        -E              -o out.ir  "$PBAS"
case_warn s7.2b-strict-bool zxbc WARN   1 ""        --parse-only --strict-bool "$PBAS"
case_warn s7.2b-f-tzx-nowarn zxbc NOWARN 0 out.tzx  -f tzx          -o out.tzx "$PBAS"

# --- S7.2c: tape append, save-config, format-gate --------------------
# Tape scenarios: SAME relative -o name (out.tap) under separate dirs so
# the TAP header basename[:10] is identical for C and Python.
case_warn s7.2c-append-binary          zxbc WARN 0 out.tap -t -o out.tap --append-binary "$BLOB"  "$PBAS"
case_warn s7.2c-append-headless-binary zxbc WARN 0 out.tap -t -o out.tap --append-headless-binary "$BLOB2" "$PBAS"
case_out  s7.2c-save-config            zxbc cfg.ini        --save-config cfg.ini -o out.bin "$PBAS"
case_err  s7.2c-format-gate            zxbc                -o out.bin --append-binary "$BLOB" "$PBAS"

# --- S7.2d: validation / mutex / format gates (exit 2 + msg) ---------
case_err s7.2d-arch-bogus    zxbc --arch bogus "$PBAS"
case_err s7.2d-W-ZZZ         zxbc -W ZZZ "$PBAS"
case_err s7.2d-plusW-999     zxbc +W 999 "$PBAS"
case_err s7.2d-W-plusW-100   zxbc -W 100 +W 100 "$PBAS"
case_err s7.2d-t-T-mutex     zxbc -t -T "$PBAS"
case_err s7.2d-parseonly-f   zxbc --parse-only -f tap "$PBAS"
case_err s7.2d-BASIC-bin     zxbc --BASIC -o out.bin "$PBAS"
case_err s7.2d-f-sna         zxbc -f sna -o out.sna "$PBAS"
# valid controls (exit 0)
case_out s7.2d-ctrl-arch     zxbc @stdout --arch zx48k --parse-only "$PBAS"
case_out s7.2d-ctrl-W        zxbc @stdout -W 100 --parse-only "$PBAS"

# --- S7.2f: --opt-strategy size|speed|auto ---------------------------
case_out s7.2f-opt-size  zxbc out.asm --opt-strategy size  --output-format=asm -o out.asm "$PBAS"
case_out s7.2f-opt-speed zxbc out.asm --opt-strategy speed --output-format=asm -o out.asm "$PBAS"
case_out s7.2f-opt-auto  zxbc out.asm --opt-strategy auto  --output-format=asm -o out.asm "$PBAS"

# ===================================================================
# zxbpp  (S7.2d-ii / S7.2e)
# ===================================================================
# --arch bogus: error MESSAGE content matches (Invalid architecture
# 'bogus') -> compare 1+2.
case_err  s7.2e-pp-arch-bogus zxbpp --arch bogus "$TBI"
# --version / -D / -I are hard-rejected: BOTH exit 2 but the message is
# the carried argparse `unrecognized arguments: <token-list>` item
# (getopt cannot reproduce the tokenised list / usage block). Exit-code
# -only by the contract — message NOT compared.
case_rc_only s7.2e-pp-version zxbpp 2 --version "$TBI"
case_rc_only s7.2e-pp-D       zxbpp 2 -D FOO "$TBI"
case_rc_only s7.2e-pp-I       zxbpp 2 -I /tmp "$TBI"
# success: plain (stdout bytes) and -o (output bytes), both path-norm'd
# (#line embeds the path).
case_out  s7.2e-pp-plain  zxbpp @stdout "$TBI"
case_out  s7.2e-pp-o      zxbpp out.out -o out.out "$TBI"

# ===================================================================
# zxbasm  (S7.2d-iii)
# ===================================================================
case_err  s7.2d-asm-t-T-mutex zxbasm -t -T "$TASM"
case_err  s7.2d-asm-B-gate    zxbasm -B "$TASM"
# valid: -t tape output (same -o name) and plain -o bin
case_out  s7.2d-asm-t-tap     zxbasm out.tap -t -o out.tap "$TASM"
case_out  s7.2d-asm-o-bin     zxbasm out.bin -o out.bin "$TASM"

echo "=================================================="
echo "cmdline-parity: $PASS PASS / $FAIL FAIL"
if [ "$FAIL" -gt 0 ]; then
    echo "--- failures ---"
    printf '  %s\n' "${FAIL_LINES[@]}"
fi
echo "=================================================="

cleanup
trap - EXIT INT TERM

[ "$FAIL" -eq 0 ] && exit 0
exit 1
