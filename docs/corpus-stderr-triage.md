# Corpus DIFF-STDERR triage — divergence class map

Scope: the 14 `DIFF-STDERR` rows in the released-program corpus
(`csrc/tests/released_corpus/`) as of 2026-06-12. Both compilers reach the
same exit code on every row; only their stderr (path-normalised) diverges.
This document classifies the 14 rows into root-cause classes, gives a minimal
repro for each, attributes the Python and C source locations, and recommends a
disposition against the maintainer's IDE-highlighting bar (a divergence MATTERS
when it would mislead a human or tooling parsing compiler output — wrong file,
wrong line, an error/warning actually present in one and absent in the other).

This is analysis only. No compiler source was changed. Repro fixtures live under
`csrc/tests/codegen_probes/_triage/<class-slug>/` (the `_` prefix keeps them out
of `PROBE_CATEGORIES`, so no meter runs them).

## Row → class summary

| Row | flags (abbrev) | Class(es) | Verdict |
|-----|----------------|-----------|---------|
| 3-reyes-magos | -O2 | parser-recovery-cascade | MATTERS |
| abydos | -O2 | parser-recovery-cascade | MATTERS |
| ad-lunam | -O3 … | data-label-in-sub | FIXED |
| ad-lunam-plus | -O3 … | include-lineno-zero | DEFERRED-PYTHON-SIDE |
| berksman | -O2 | parser-recovery-cascade | MATTERS |
| breakspace | -O2 … | include-lineno-zero | DEFERRED-PYTHON-SIDE |
| knights-demons-dx | -O3 … | include-lineno-zero (1st) + crlf-continuation (2nd) | DEFERRED-PYTHON-SIDE / MATTERS |
| looking-for-csscgc2012 | -O2 | parser-recovery-cascade | MATTERS |
| maritrini | -O2 | include-filename-attribution (W150) + diagnostic-ordering | MATTERS / SHELVE |
| pixel-quest | -O3 … | include-lineno-zero | DEFERRED-PYTHON-SIDE |
| pixel-quest-2000 | -O3 … | include-lineno-zero | DEFERRED-PYTHON-SIDE |
| souls | -O2 | parser-recovery-cascade | MATTERS |
| zen | -O2 … | include-lineno-zero (1st) + w520-whitespace-quirk (2nd) | DEFERRED-PYTHON-SIDE |
| zen-ii | -O2 … | include-lineno-zero (1st) + w520-whitespace-quirk (2nd) | DEFERRED-PYTHON-SIDE |

The 14 rows collapse into **6 classes**. Three rows (knights-demons-dx, zen,
zen-ii) carry a second divergence behind the first-divergence the meter buckets;
those secondaries are listed under their own classes. maritrini carries three
sub-divergences (one MATTERS, two ordering).

DEFERRED-PYTHON-SIDE rows (root cause is a Python/PLY artifact, propose no C
change): **ad-lunam-plus, breakspace, knights-demons-dx (first divergence only),
pixel-quest, pixel-quest-2000, zen (first divergence), zen-ii (first
divergence)** — the include-lineno-zero class; plus **zen, zen-ii** again for the
w520-whitespace-quirk secondary.

---

## CLASS 1 — include-lineno-zero  *(DEFERRED-PYTHON-SIDE)*

**Member rows (first divergence):** ad-lunam-plus, breakspace, knights-demons-dx,
pixel-quest, pixel-quest-2000, zen, zen-ii.

**Repro:** `csrc/tests/codegen_probes/_triage/include-lineno-zero/inc0.bas`

```basic
DIM x AS UBYTE
#include "nonexistent_lib.bas"
x = 1
```

**Divergence (`-O2`):**

```
PY:  inc0.bas:0: error: file 'nonexistent_lib.bas' not found
C :  inc0.bas:2: error: file 'nonexistent_lib.bas' not found
```

**Root cause.** Quote-form `#include "…"` (non-`once`) parses via
`src/zxbpp/zxbpp.py:413` `p_include_macro : include : INCLUDE include_modifier
expr` (the `"…"` string reduces through `expr : STRING`,
src/zxbpp/zxbpp.py:733). The not-found error is raised with `p.lineno(3)`
(zxbpp.py:430), but `p[3]` here is the **`expr` non-terminal**, and PLY's
`p.lineno(n)` returns **0** for a non-terminal unless lineno is explicitly
propagated up the reduction — which this grammar does not do. So Python emits
`:0:` for every quote-form include that is not found. The error text itself comes
from `search_filename` (zxbpp.py:204). The C port computes the real source line
(correct in isolation), so it diverges.

C location: `csrc/zxbpp/preproc.c` include-resolution path (the `file '…' not
found` emission) carries the directive's actual line.

**Verdict: DEFERRED to upstream-resync phase.** The line-0 form is a genuine PLY
defect on the *Python* side; the C output (real line number) is the *more*
correct one for an IDE. Matching Python here would mean deliberately zeroing a
correct line number — regressing IDE highlighting to reproduce an upstream bug.
Classify, attribute, defer; propose no C change. (Angle-bracket `#include <…>`
goes through `p_include_fname`/FILENAME with a real token lineno and would not
show this; all seven member rows use the quote form.)

**Effort to "fix" (i.e. match Python): herculean and ill-advised** — would
require emulating PLY non-terminal lineno propagation just for this token.
**Regression risk: high** (zeroing a correct line). Recommend leaving until the
upstream-resync phase decides whether to carry Python's quirk or diverge by
design.

---

## CLASS 2 — parser-recovery-cascade  *(MATTERS)*

**Member rows:** souls, berksman, looking-for-csscgc2012, 3-reyes-magos, abydos.

**Repro:** `csrc/tests/codegen_probes/_triage/parser-recovery-cascade/recovery.bas`

```basic
SUB prota()
   DIM a AS UBYTE
   IF a=1 THEN a=2
   a=3
   END IF
END SUB
```

**Divergence (`-O2`):**

```
PY:  recovery.bas:5: error: Syntax Error. Unexpected token 'IF' <IF>
     recovery.bas:5: error: Unexpected token 'END'. Expected 'END FUNCTION' or 'END SUB' instead.
C :  recovery.bas:5: error: Syntax Error. Unexpected token 'IF' <IF>
```

**Root cause.** When a syntax error occurs inside a SUB/FUNCTION body and the
parser is in error-recovery, PLY can reduce the **error production**
`function_declaration : function_header program_co END error`
(`src/zxbc/zxbparser.py:3007` `p_function_error`), which emits
`"Unexpected token 'END'. Expected 'END FUNCTION' or 'END SUB' instead."`
(zxbparser.py:3010-3013) at `p.lineno(3)`. The C parser
(`csrc/zxbc/parser.c`) uses hand-written recursive-descent recovery, which does
not carry PLY's `error`-token grammar productions, so this secondary error is not
emitted and the C resync consumes a different span of tokens.

The same root drives the two **"Too many errors" line-drift** rows
(3-reyes-magos, abydos): there every *visible* error line is byte-identical
between Python and C — only the final `Too many errors. Giving up!` line differs.
`src/api/errmsg.py:48-55` replaces the message of the `(max_syntax_errors+1)`-th
error call but keeps **that call's lineno**. Because the two recoveries consume
tokens differently, the error that trips the cap fires at a different source
position:

```
3-reyes-magos:  PY  TRM68.bas.txt:1501: error: Too many errors. Giving up!
                C   TRM68.bas.txt:1425: error: Too many errors. Giving up!
abydos:         PY  NOST120E.bas:3309: …    C  NOST120E.bas:3307: …
```

For 3-reyes-magos the divergence is concrete: after the last visible IF error at
1425, Python's recovery skips forward to the next malformed single-line-IF at
1501 before raising the cap-tripping error; C raises its (suppressed) cap-tripping
error still at 1425.

**Verdict: MATTERS.** Python reports an error that C omits (and vice-versa the cap
line drifts) — a human or IDE parsing the stream gets a different error set /
position. This is real-program-realistic (all five rows are old single-line-`IF`
dialect that both versions correctly reject; the divergence is purely in the
*recovery cascade*).

**Effort: herculean.** Byte-matching PLY's LALR error-token recovery from a
hand-written recursive-descent parser is the hardest item here — it would touch
the whole statement/recovery loop in `csrc/zxbc/parser.c` and require replicating
which `error`-token productions PLY reduces and how far each consumes.
**Regression risk: high** — recovery changes ripple across every malformed input.
A pragmatic partial fix is to add the single `p_function_error`-equivalent
production (emit the "Expected END FUNCTION/SUB" error when recovery hits a stray
`END` inside a function), which covers souls/berksman/looking-for; the
cap-line-drift rows (3-reyes-magos, abydos) would need fuller recovery parity.
Recommend last, and only if the maintainer wants error-stream parity on
deliberately-malformed legacy sources.

---

## CLASS 3 — crlf-continuation  *(FIXED)*

**Status: FIXED** (commit `fix(zxbpp): CRLF-tolerant line-continuation`). The
three zxbpp pre-tokenisation join loops (`preproc_file` top-level,
`preproc_string`, and the include loop) now detect the `\\`/`_` continuation
marker one byte before an optional trailing CR and preserve that CR in the join,
mirroring Python's `r"[\\_]\r?\n"`. The first-`#define` blank-line emission was
also made CR-aware (Python's `program : define NEWLINE`, zxbpp.py:326, emits the
NEWLINE token value verbatim, so a CRLF source yields `\r\n`). Probe
`preprocessor/define_crlf_line_continuation.bas` (committed with CRLF via the
repo-root `.gitattributes` `*.bas -crlf`) was RED (`PROBE-DIFF-EXIT`, Py=0 C=1)
before the fix, PROBE-EQUAL after. knights-demons-dx's two `illegal preprocessor
character '\'` stderr lines are gone from its `--diff`; only its deferred
include-lineno-zero first divergence (CLASS 1) remains, keeping the row
DIFF-STDERR by design. Verified: the C zxbc/zxbasm lexers already tolerate CRLF
(forward-scan, not look-one-back), so no change was needed there. The underscore
`_`-CRLF *hole* is closed too (lines now join), but a separate, pre-existing
`_`-rendering divergence (Python strips the `_`, C keeps it) remains and is
identical on LF — out of scope for this CRLF class.

**Member rows:** knights-demons-dx (secondary divergence, behind its
include-lineno-zero first divergence).

**Repro:** `csrc/tests/codegen_probes/_triage/crlf-continuation/crlf_define.bas`
— **this file is committed with CRLF line endings** (`.gitattributes` has
`*.bas -crlf`, so git preserves the bytes). The bug only reproduces with CRLF.

```basic
#define switchMusic() \⏎(CRLF)
    asm                   \⏎(CRLF)
        call 54721        \⏎(CRLF)
    end asm⏎(CRLF)
switchMusic()⏎(CRLF)
```

**Divergence (`-O2`):**

```
PY:  (clean — no output)
C :  crlf_define.bas:2: error: illegal preprocessor character '\'
     crlf_define.bas:3: error: illegal preprocessor character '\'
```

**Root cause.** A `\` line-continuation followed by `\r\n`. Python's lexer
continuation rules use `r"[\\_]\r?\n"` (e.g. `src/zxbpp/zxbpplex.py:100`
`t_asm_CONTINUE`, and the INITIAL/define/defargs CONTINUE rules), which
explicitly tolerate an optional `\r` before `\n`. The C preprocessor's
line-joining loops check the **last character before `\n`** for `\\`
(`csrc/zxbpp/preproc.c:2818-2840` top-level loop; `:1532` include loop), but with
CRLF the last char is `\r`, not `\\`, so the continuation is not recognised; the
trailing `\` survives into `process_line`, which then reports `illegal
preprocessor character '\'`. The `\r` is only stripped on the *non*-continued
path (`preproc.c:2847`, `:1545`), i.e. after the wrong branch is taken.

**Verdict: MATTERS.** C emits spurious errors (wrong file/line attribution of a
non-error) that Python does not — and CRLF source is common (the knights-demons-dx
release ships CRLF). An IDE would flag a valid continuation as an error.

**Effort: small.** In both join loops, test for `\\` allowing a single optional
trailing `\r` before the line boundary (mirror the Python `\r?` in the
continuation predicate) — i.e. strip/ignore a trailing `\r` *before* the
backslash check rather than after. **Regression risk: low** — narrowly scoped to
the end-of-line continuation predicate; the underscore-continuation branch has the
same shape and would want the same treatment.

---

## CLASS 4 — data-label-in-sub  *(FIXED)*

**Status: FIXED** (commit `fix(zxbc): DATA-in-sub label collision cascade`).
The C `p_data` reduce
action (`case 157` in `csrc/zxbc/parser.c` — the live table-driven path, NOT the
parallel `parse_statement` BTOK_DATA branch at :4628, which is dead code for the
DATA construct under the table-driven parser) now mirrors Python's
order-of-operations exactly: `make_label(DATA_PTR_CURRENT)` runs FIRST
(zxbparser.py:1734), with the `declare_label` collision check
(symboltable.py:592-595) applied to the `symboltable_access_label` result (the
same check `label_define` already performs for `<label>:` sites), THEN the
`p[2] is None` early-out (:1738), THEN the `FUNCTION_LEVEL` guard (:1742). For
consecutive in-SUB DATA the guard returns without advancing `DATA_PTR_CURRENT`,
so the 2nd+ DATA re-declares the same auto-label and now emits
`Label '.DATA.__DATA__N' already used at <file>:<line>` BEFORE the
`DATA not allowed within Functions nor Subs` error — byte-identical to Python.
Probe `errors/err_data_label_in_sub.bas` RED (`PROBE-DIFF-STDERR`) before, GREEN
after. ad-lunam promoted DIFF-STDERR → FRONTEND-EQUAL (this was its only
divergence). Top-level DATA and the DATA-bearing BINARY-EQUAL rows (fourspriter)
verified unchanged.

**Original triage C-location note:** the triage placed the C site at
`csrc/zxbc/parser.c:4631-4643`. That was imprecise — that `parse_statement`
branch is dead code for DATA; the live reduce action is `case 157`. The fix
landed there. (Sibling check: the only other `declare_label` callers are the
label-definition productions, already covered by `label_define`'s collision
check; `check_and_make_label` callers — RESTORE/GOTO/GOSUB — are references, not
declarations, and correctly skip the check. p_data was the sole divergent site.)

**Member rows:** ad-lunam.

**Repro:** `csrc/tests/codegen_probes/_triage/data-label-in-sub/data_in_sub.bas`

```basic
SUB foo()
   DATA 1, 2, 3
   DATA 4, 5, 6
END SUB
```

**Divergence (`-O2`):**

```
PY:  data_in_sub.bas:2: error: DATA not allowed within Functions nor Subs
     data_in_sub.bas:3: error: Label '.DATA.__DATA__0' already used at data_in_sub.bas:2
     data_in_sub.bas:3: error: DATA not allowed within Functions nor Subs
C :  data_in_sub.bas:2: error: DATA not allowed within Functions nor Subs
     data_in_sub.bas:3: error: DATA not allowed within Functions nor Subs
```

(In ad-lunam this is `.DATA.__DATA__47`, three times, between the four
`DATA not allowed` errors.)

**Root cause.** Python's `p_data` (`src/zxbc/zxbparser.py:1732`) calls
`make_label(gl.DATA_PTR_CURRENT, …)` at **line 1734 — before** the
`if gl.FUNCTION_LEVEL: error("DATA not allowed…")` guard at line 1742.
`make_label` (zxbparser.py:452) routes to `SYMBOL_TABLE.declare_label`, which
detects an already-declared label and raises `Label '…' already used at …`
(`src/api/symboltable/symboltable.py:594`). Because `DATA_PTR_CURRENT` is the
same value for consecutive in-SUB DATA statements, the second and later DATA
statements collide and emit the extra error *in addition to* the
DATA-not-allowed error.

The C port (`csrc/zxbc/parser.c:4631-4643`) creates the per-DATA label with
`symboltable_access_label` (an **access**, not a declare), which does not run the
duplicate-detection path, so the "already used" cascade is absent.

**Verdict: MATTERS.** Python reports errors C does not. The errors are odd
(they're a side-effect of label creation running before the function-level
guard), but they are real diagnostic output an IDE/log parser would see — and the
maintainer's rule is parity, not "the prettier diagnostic wins".

**Effort: small–medium.** Route the in-`p_data` label creation through the
declare/collision path (the C analogue of `declare_label`) so the duplicate is
detected, matching the Python order-of-operations (create label, *then* guard on
function level). **Regression risk: medium** — touches DATA label registration,
which feeds the `.__DATA__` block; must ensure the *valid* (top-level) DATA path
is unaffected and only the in-SUB collision diagnostic is added. Verify against
the existing BINARY-EQUAL rows with DATA blocks (fourspriter et al.).

---

## CLASS 5 — include-filename-attribution (W150)  *(FIXED — commit pending)*

**Status: FIXED.** Resolved by attributing the W150 "is never used" emits to
the symbol's stored declaration filename (mirroring the W170/W190 fix
c59df290c). Probe `warnings/w150_include_filename_attribution` (+ companion
`_w150_included_unused_var.bi`) RED-verified before the fix, GREEN after.
maritrini's W150 wrong-file line (`plScore`) is gone from its `--diff`; only the
two documented ordering-only sub-divergences (W190 visit-order, undefined-label
order) remain — both SHELVED (every file+line correct, only sequence differs).

**Correction to the C-location attribution below.** The original triage placed
the C site in "`csrc/api/` / the symbol-table walk". That was imprecise: the
module-level W150 is emitted from the **var-translation** phase
(`csrc/zxbc/var_translator.c` — `vt_visit_vardecl` and `vt_visit_arraydecl`,
mirroring Python `src/arch/z80/visitor/var_translator.py:29`/`:50`), NOT the
symbol-table. There is also a **sibling site** the triage did not mention: the
optimizer LET-prune path (`csrc/zxbc/passes/optimizer.c` `opt_visit_let` /
`opt_visit_letarray`, mirroring Python `src/api/optimize.py:323`/`:345`), which
emits W150 for an assigned-but-never-read variable and was diverging the same
way. maritrini's `plScore` (assigned at engine.bas:196, declared at
engine.bas:69) is hit by BOTH paths; Python emits the warning twice but its
`errmsg.msg_output` dedup cache (errmsg.py:30, keyed on the fully-formatted
message) collapses the two identical `engine.bas:69` strings to one. Before the
fix both C sites used `cs->current_file` and produced identical
`maritrini.bas:69` strings (so C's emission also collapsed to one wrong line);
fixing only var_translator made the two C strings DIFFER, surfacing a second
line — so **both** sites had to be fixed for parity. (Python's `visit_LETSUBSTR`,
optimize.py:362, passes no `fname` and is left on `current_file` to match.)

**Member rows:** maritrini (one of three sub-divergences).

**Repro:** `csrc/tests/codegen_probes/_triage/include-filename-attribution/`
(entry `main.bas`, which `#include once "inc.bas"`).

```basic
; inc.bas
DIM unusedVar AS UINTEGER
; main.bas
#include once "inc.bas"
DIM used AS UBYTE
used = 1
```

**Divergence (`-O2`):**

```
PY:  main.bas:2: warning: [W150] Variable 'used' is never used
     inc.bas:1: warning: [W150] Variable 'unusedVar' is never used
C :  main.bas:2: warning: [W150] Variable 'used' is never used
     main.bas:1: warning: [W150] Variable 'unusedVar' is never used      ← wrong file
```

(In maritrini: PY `engine.bas:69` vs C `maritrini.bas:69` for variable
`plScore`.)

**Root cause.** For a module-level `DIM` in an *included* file that is never used,
W150 is emitted via `warning_not_used` (`src/api/errmsg.py:154`), which takes an
explicit `fname`. Python carries the *defining file* (the included file) as the
warning's filename; the C port emits the **main file's** name with the included
file's line number — same line, wrong file. This is the same family as the
already-fixed W170/W190 include-filename-attribution work (noted in the corpus
README, probe `warnings/w170_w190_include_filename_attribution.bas`), but on the
W150 path for module-scope variables, which was not covered by that fix.

C location: the W150 `never used` emission for module-level symbols in
`csrc/api/` / the symbol-table walk that reports unused variables — it needs to
pass the symbol's own `fname` rather than the current/main file.

**Verdict: MATTERS — strongly.** This is the maintainer's exact worry: an IDE
keys highlighting off the reported file+line, and C names the *wrong file*. The
line number happens to coincide (69), which makes it worse — the highlight lands
on an unrelated line of `maritrini.bas`.

**Effort: small.** Mirror the existing W170/W190 fix for the W150 module-variable
path: attribute the warning to the symbol's defining file. **Regression risk:
low** — the W170/W190 precedent shows the symbol already knows its origin file;
this extends the same attribution to one more warning code.

---

## CLASS 6 — w520-whitespace-quirk  *(DEFERRED-PYTHON-SIDE)*

**Member rows:** zen, zen-ii (secondary divergence, behind include-lineno-zero).

**Repro:** `csrc/tests/codegen_probes/_triage/w520-whitespace-quirk/w520.bas`

```basic
#define BOARD(r,c)  (1+(r)+(c))
DIM x AS UBYTE
x = BOARD(1,2)
```

(Note the **two** spaces after `)`.)

**Divergence (`-O2`):**

```
PY:  w520.bas:1: warning: [W520] missing whitespace after macro name
     w520.bas:2: warning: [W150] Variable 'x' is never used
C :  w520.bas:2: warning: [W150] Variable 'x' is never used
```

**Root cause — a Python bug.** For a function-like macro, after `)` the lexer
enters the `defexpr` state and the `SEPARATOR` rule `r"[ \t]+"`
(`src/zxbpp/zxbpplex.py:371`) greedily captures *all* the trailing whitespace as a
single token, so `defs[0]` is e.g. `"  "` (two spaces). The W520 guard in
`p_define` (`src/zxbpp/zxbpp.py:552-556`) tests
`isinstance(defs[0], str) and defs[0] in " \t"` — but `in` is **substring
membership**, and `"  "` (two spaces) is *not* a substring of `" \t"`
(space+tab), so the `else` branch fires and emits W520 "missing whitespace after
macro name" — *even though whitespace is present*. Confirmed behaviour:

| after `)` | Python W520? |
|-----------|--------------|
| 1 space   | no  (`" "` IS a substring of `" \t"`) |
| 2 spaces  | **yes** (spurious) |
| 3 spaces  | **yes** (spurious) |
| 1 tab     | no |

The C preprocessor (`csrc/zxbpp/preproc.c:888-891`) correctly gates W520 on the
char after `)` being genuinely non-whitespace, so it does not fire on
`BOARD(r,c)  (…)`. C is the *correct* one.

**Verdict: DEFERRED to upstream-resync phase.** Matching Python means porting a
substring-membership bug whose warning text ("missing whitespace") is the
opposite of what the source contains. Classify, attribute, defer; propose no C
change. (The *legitimate* W520 cases — object-like `#define FOO=1`, function-like
`#define A(r,c)(1)` with truly no whitespace — already agree between Python and C;
only the 2+-whitespace spurious case diverges.)

**Effort to match Python: small but wrong** (replicate the buggy membership
test). **Regression risk of matching: introduces a knowingly-incorrect warning.**
Leave for the upstream-resync decision.

---

## Recommended fix order (across classes)

Ordered by value-per-risk against the IDE-highlighting bar:

1. **CLASS 5 — include-filename-attribution (W150)** — ✅ **FIXED.** small,
   low-risk, high payoff (wrong *file* attributed). Extended the existing
   W170/W190 fix to the var-translator VARDECL/ARRAYDECL and optimizer
   LET/LETARRAY W150 emit sites. Was done first, as recommended.
2. **CLASS 3 — crlf-continuation** — ✅ **FIXED.** small, low-risk; cleared the
   spurious `illegal preprocessor character '\'` errors on common CRLF sources
   (knights-demons-dx). Narrow predicate change in the three zxbpp join loops
   plus a CR-aware first-`#define` blank.
3. **CLASS 4 — data-label-in-sub** — ✅ **FIXED.** small–medium; routed the
   `case 157` p_data label creation through the declare/collision path (mirroring
   `label_define`) and reordered make_label before the FUNCTION_LEVEL guard to
   match Python. DATA-bearing BINARY-EQUAL rows verified unchanged.
4. **CLASS 2 — parser-recovery-cascade** — herculean; do last and only if
   error-stream parity on malformed legacy dialect is wanted. A partial
   `p_function_error`-equivalent covers souls/berksman/looking-for; full parity
   (incl. the 3-reyes-magos/abydos cap-line drift) is a large recovery rework.
5. **CLASS 1 — include-lineno-zero** and **CLASS 6 — w520-whitespace-quirk** —
   *do not fix C.* DEFERRED-PYTHON-SIDE; both are upstream PLY/Python defects and
   the C output is the more correct one. Revisit only at the upstream-resync
   phase to decide whether to deliberately carry the upstream quirk.

## Rows that will still DIFF after the MATTERS classes are fixed

Three rows carry a DEFERRED-PYTHON-SIDE divergence as their **first** divergence,
so even after fixing CLASSes 3/4/5/(2) the meter will still bucket them
DIFF-STDERR until the upstream-resync decision lands: **zen, zen-ii** (line-0
include, then the spurious W520), **ad-lunam-plus, breakspace, pixel-quest,
pixel-quest-2000** (line-0 include), and **knights-demons-dx** (line-0 include
first; its crlf-continuation secondary is the fixable part). This is expected:
they are upstream-side findings, not C-port bugs.

## Open questions for the maintainer

1. **include-lineno-zero (CLASS 1):** carry the upstream PLY line-0 quirk for
   byte-parity, or keep C's correct line number and accept the DIFF until
   upstream is patched? Seven rows hinge on this single call.
2. **parser-recovery-cascade (CLASS 2):** is error-stream parity on
   deliberately-malformed legacy-dialect sources worth a recovery rework, or is
   "same exit code, same *valid* diagnostics" sufficient and the cascade
   divergence acceptable? If partial is OK, the `p_function_error` production
   alone closes souls/berksman/looking-for.
3. **maritrini diagnostic-ordering (sub-class, no fixture):** see below.

## Note — maritrini diagnostic-ordering (sub-divergence, not separately reproduced)

Beyond the W150 file-attribution bug (CLASS 5), maritrini has two **ordering-only**
divergences — same diagnostic set, different emission order:

- **W190 "should return a value":** PY emits
  `drawInventory` (inventory.bas:70), `normalizeNumber` (engine.bas:572),
  `collision` (engine.bas:499); C emits drawInventory, *collision*,
  *normalizeNumber* (last two swapped). Root: the emission order follows the AST
  traversal in `UnreachableCodeVisitor.visit_FUNCTION`
  (`src/api/optimize.py:95-105`); the C optimizer pass walks functions in a
  different order.
- **Undefined GLOBAL label errors** (bin stage; maritrini exits 5): same three
  labels (`.MUSICDATA`, `.BEEPOLA_NEXTNOTE`, `._splashScreen.text`), different
  order between PY and C — a backend undefined-label-resolution ordering
  difference.

I did **not** produce a clean minimal fixture for the ordering sub-class. Genuine
effort was spent; it resists minimisation because the order only diverges across a
*set* of functions/labels spread over multiple included files, and a 2–3 function
toy did not reproduce the specific swap (the divergence depends on the multi-file
symbol-registration order). Recommend **SHELVE** for the ordering pieces: for the
IDE-highlighting bar, every individual line and file is correct — only the
sequence differs, which does not mislead highlighting. If the maintainer wants
full stderr byte-parity, this needs a dedicated dig into the optimizer
function-visit order and the backend label-resolution order, which is
medium-effort, medium-risk, and low IDE payoff. The W150 file-attribution piece
(CLASS 5) is the part of maritrini that genuinely MATTERS.
