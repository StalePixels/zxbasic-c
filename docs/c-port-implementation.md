# ZX BASIC C Port — Implementation Report

Companion to [`c-port-plan.md`](c-port-plan.md) (the plan) and the
phase plans under [`plans/`](plans/). This document records the
implemented state: a C port of the Boriel ZX BASIC toolchain whose
output is **byte-for-byte identical to the Python original across the
inherited functional corpus** (`tests/functional/`), through the full
compile→assemble→link pipeline, for both the `zx48k` and `zxnext`
targets.

**Scope of that claim (important):** corpus byte-identity is achieved
and independently verified — but it is **not** the same as full
drop-in completeness. A fixed corpus is byte-truth only for the
codepaths it exercises; it is silent on the rest. The
rejection/diagnostic surface (error/warning parity on *invalid* input)
and codepaths the corpus never exercises still carry **known open
divergences** — see *Known residue* below and the verification-probe
series (`csrc/tests/codegen_probes/`). "Port complete" (full drop-in)
is **not** yet reached.

## Status

| Target | Stage 1 (codegen ASM) | Stage 2 (assembled binary) | Stage 3 (end-to-end) |
|--------|----------------------|----------------------------|----------------------|
| **zx48k**  | 895 EQUAL / 0 DIVERGE / 0 C-ERROR | 886 / 0 / 0 | 886 / 0 / 0 |
| **zxnext** | 197 EQUAL / 0 DIVERGE / 0 C-ERROR | 197 / 0 / 0 | 197 / 0 / 0 |

Both targets report **VERDICT: ALL STAGES GREEN** — byte-identical
through the gated pipeline. (Counts exclude fixtures the *Python*
reference itself cannot compile/assemble — `skip-python` — and a small
`skip-known-bug` set; see "Known residue" below.)

These counts are **corpus codegen byte-identity** (the
codegen→assemble→binary path). They are **not** a port-complete claim:
the rejection/diagnostic surface and untested-codepath divergences in
*Known residue* are outside what this table measures.

Cross-component equivalence guards also pass:

| Guard | Result |
|-------|--------|
| `zxbc` parse-equivalence (exit-code parity vs Python `--parse-only`) | 952 PASS / **34 FALSE_NEG** (C accepts what Python rejects) / 47 STDERR_MISMATCH / **0 FALSE_POS** |
| `zxbpp` strict harness | 86 success + 5 error PASS / **0 FAIL** |
| `zxbasm` success (binary) tests | 60 PASS / **0 FAIL** |

`FALSE_POS = 0` is the hard regression gate and holds. The **34
FALSE_NEG** (C accepts programs Python rejects) and **47
STDERR_MISMATCH** (divergent error/warning text) are *not* clean —
they are the rejection/diagnostic-surface residue catalogued under
*Known residue*, required for full drop-in.

## What "byte-identical" means here, and how it is measured

The single source of truth is the **gated stage meter**,
[`csrc/tests/run_zxbc_stage_validation.sh`](../csrc/tests/run_zxbc_stage_validation.sh),
driven by `make test-zxbc-stages` (zx48k) or by pointing the same script
at `tests/functional/arch/zxnext`. It runs Python and C side-by-side on
every fixture and compares, with each stage **gated** on the previous so
a later stage can never paper over an earlier divergence:

1. **Stage 1 — codegen ASM fidelity.** `zxbc --output-format=asm` output
   compared byte-for-byte (path-normalised) against Python. This is the
   core deliverable: the C compiler emits the *same Z80 assembly text*.
2. **Stage 2 — assembly fidelity.** The (now identical) ASM is assembled
   by both C `zxbasm` and Python `zxbasm`; the `.bin` outputs are compared.
   Gated on Stage 1.
3. **Stage 3 — end-to-end.** The default `zxbc -o out.bin` pipeline is run
   end-to-end; exit code and final binary are compared. Gated on Stage 2.

A fixture is **fully byte-identical** only when all three stages are
EQUAL with zero DIVERGE and zero C-ERROR. (This is the per-fixture
pipeline verdict — distinct from whole-port "port complete," which also
requires the rejection/diagnostic surface in *Known residue*.) The
verdict line is mechanical, not a human judgement.

This meter exists specifically because earlier per-stage metrics could
read green in isolation while the *composed* pipeline diverged — measuring
each stage independently let a codegen-text match hide an assembled-binary
mismatch. The gated design makes that structurally impossible.

**The corpus meter is necessary but not sufficient.** It is byte-truth
only for the codepaths the fixed corpus exercises. A second,
coverage-driven instrument — the verification-probe series
([`csrc/tests/codegen_probes/`](../csrc/tests/codegen_probes/),
`run_probes.sh`) — authors *new* fixtures that exercise codepaths the
corpus does not, comparing C-vs-Python on the full meaningful contract
(exit code + stderr + Stage-1 asm + binary). Its first batch (typecast)
immediately surfaced real divergences the corpus was blind to (listed
in *Known residue*), confirming that "all stages green on the corpus"
under-states the work remaining for a true drop-in. That series is
ongoing.

## Architecture

The pipeline and the Python→C structural mapping are documented in
[`c-port-plan.md`](c-port-plan.md). In brief:

```
BASIC → zxbpp (preprocess) → zxblex/zxbparser (lex/parse, recursive-descent)
      → AST (tagged-union structs) → optimizer passes
      → translator (AST → IC quads, visitor) → backend (quads → Z80 asm + peephole)
      → asm preprocess (#include runtime) → zxbasm (assemble) → .bin/.tap/.tzx/.sna/.z80
```

Key implementation decisions (all to preserve byte-identity with a
GC'd, PLY-based Python original):

- **Hand-written recursive-descent** parsers replace PLY lex/yacc.
  LALR(1) reduction order is reproduced where it is observable in output
  (e.g. the statement-level call disambiguation below).
- **Tagged-union AST** (`AstNode` with a common header + `u.<variant>`)
  replaces the 50+ inheriting `Symbol` classes.
- **Arena allocation** replaces Python GC — allocate during compilation,
  free all at end.
- **No external dependencies**, matching the Python original; the only
  vendored code is permissively-licensed portability shims (`ya_getopt`,
  `cwalk`).
- Floating-point, peephole pattern ordering, and path formatting are all
  reproduced exactly, because every one of them is observable in the
  emitted bytes.

## Methodology

The port was driven entirely by the stage meter, in a tight loop:

1. Run the meter; group the remaining `S1-DIVERGE` programs by **root
   cause**, not by program.
2. Read the **Python reference** for that cause (`src/…`) before writing
   any C — the rule is *match what Python does, not what the code "should"
   do*.
3. Fix the cause in `csrc/` (the only writable tree; `src/` and `tests/`
   are read-only upstream mirrors).
4. Rebuild, re-meter, and confirm the cross-component guards
   (`test-zxbc-parse`, `test-zxbpp`, `test-zxbasm`) still hold.
5. Commit per cause, recording the divergence kind and the
   `S1-DIVERGE before → after` delta.

A net-regressive change was treated as a defect to self-correct (revert →
re-read Python → redo), never as a stopping point. Long meter runs were
wrapped in a watchdog (`perl -e 'alarm N; exec @ARGV'`, since macOS has no
`timeout`).

The closing stretch took the zx48k `S1-DIVERGE` count from roughly **415**
down to **0**, then closed the residual Stage-2 assembler errors.

## Notable root-cause classes (closing long tail)

The final long tail was dominated by a handful of *general* defects, each
of which had been masking multiple fixtures. Documented here because each
is a reusable lesson about where byte-identity hides:

### Type & constant handling
- **`NUMBER.t` float representation.** Python stores a number's text as
  `str(value)` — the shortest decimal that round-trips through `float()`.
  The port had used `%g` (6 significant figures), so `PI` and any
  constant-folded float lost precision; that truncated text was re-parsed
  downstream and produced different 40-bit FP bytes. Fixed with a
  shortest-round-trip formatter ([`csrc/zxbc/pyfloat.c`](../csrc/zxbc/pyfloat.c)).
- **Unary-MINUS constant fold.** Folding `-x` must *re-infer* the literal's
  type from the new value (Python builds a fresh `SymbolNUMBER`), or
  `DATA -1` mistypes its element header.
- **Argument typecasts at parse time.** `CHR$`, `STR$`, the trig/log/sqrt
  family, and `PEEK` all `make_typecast` their operand (to ubyte / float /
  float / uinteger respectively) in the Python grammar; without it the
  backend reads the wrong operand width (e.g. a `UByte` read as a 5-byte
  float, or a missing `__U8TOFREG` / `__FTOU32REG` conversion).
- **`CAST(t, const)`** must route through `make_typecast` so a constant
  operand folds to a retyped literal instead of emitting a runtime cast.

### Symbol, scope & labels
- **`.`-prefixed labels** (`.core.ZXBASIC_USER_DATA…`) keep their verbatim
  mangled name; the general `.LABEL._`-prefix rule must not be applied to
  them.
- **`#include` / `#pragma once` deduplication** must key on the *canonical
  realpath*, not the spelling used to reach the file — otherwise a
  self-referential relative include (`../zx48k/…`) never matches the
  once-set and recurses until the path overflows.
- **Suffix-stripped symbol lookup.** The lexer's `ARRAY_ID` promotion
  must strip a trailing `$`/`%`/`&` before the table lookup, or
  `c$ = a$` (whole string-array copy) is mis-parsed as a scalar `LET`.
- **O>1 accessed-filtering** of locals (in `compute_offsets` and the
  stdcall teardown) must match Python's `Scope.values(filter_by_opt=True)`,
  or a write-only local string gets a spurious second `__MEM_FREE`.

### Control flow & statements
- **3-or-more `ELSEIF` chains.** A latent AST-construction bug attached
  the third and later `ELSEIF` as extra children of the *first* nested
  `IF` rather than nesting under the previous one; `visit_IF`'s
  `child_count == 3` else-gate then silently dropped every branch from the
  second `ELSEIF` onward. This affected *any* 3+ `ELSEIF` statement.
- **Statement-level `f(x) OP y`.** A bare `test(1) + test(2)` statement
  reduces in Python via `statement : ID arguments` — the call applied to
  the *whole* `(1) + test(2)` as a single argument — not as a binary of two
  call results. (The `LET`/expression context keeps the binary reading.)

### Backend emitters
- **`emit_pstoref` / `emit_pstoref16`** — storing a Float/Fixed scalar to a
  local/parameter slot had no dispatch handler, so the value was left
  pushed on the stack with no store.
- **String-array copy** (`STR_ARRAYCOPY`) and the array-element
  `visit_READ` path needed the correct `mangled` vs `data_label` operand,
  matching Python's distinct choices in the two visitors.

### Preprocessor & assembler
- **Object-like macro bodies** must preserve trailing whitespace/newlines;
  Python lstrips only the first body token. A `\`-continued blank line in a
  macro (`#define BREAK \ … nop \ <blank>`) contributes a trailing newline
  that both emits a blank line *and* advances the source-line counter —
  stripping it shifted every subsequent `#line` by one.
- **`zxbasm` trailing-`h` hex literals.** `t_HEXA` is defined before
  `t_BIN` in the Python lexer, so `0B1h` is hex `0xB1`, not `0b`-binary; the
  C lexer's `0b` rule had to yield to a trailing-`h` lookahead.

## Build & verify

```bash
# Build (from repo root)
cd csrc/build && cmake .. && make -j4 && cd ../..

# Full byte-identical pipeline meter — zx48k
make test-zxbc-stages

# Same meter — zxnext
./csrc/tests/run_zxbc_stage_validation.sh \
    ./csrc/build/zxbc/zxbc ./csrc/build/zxbasm/zxbasm \
    tests/functional/arch/zxnext

# Cross-component equivalence guards
make test-zxbc-parse      # zxbc exit-code parity vs Python --parse-only
make test-zxbpp           # preprocessor strict harness
make test-zxbasm          # assembler (success = binary parity)
```

The meter requires a Python 3.11+ reference interpreter to produce the
ground-truth side of each comparison.

## Known residue (open divergences — required for full drop-in)

Corpus codegen byte-identity is complete; these are the surfaces that
are **not** yet byte-for-byte and so block a full drop-in claim. They
are dominated by the **rejection/diagnostic** path (behaviour on
invalid input, and warning text) — not by codegen.

- **`zxbc` rejection-surface FALSE_NEG (34).** Programs the Python
  reference *rejects* (exit 1 + error) that the C currently *accepts*
  (exit 0, emits a binary). Confirmed example from the verification
  probes: a string argument passed to a numeric function parameter —
  Python errors `Cannot convert string to a value. Use VAL() function`;
  C silently compiles it. (The argument-typecast check was narrowed to
  numeric→numeric during the codegen campaign; the string↔numeric
  function-call path is unported. The LET/assignment path is correct.)
- **`zxbc` diagnostic-text STDERR_MISMATCH (47).** Divergent error/
  warning text on inputs both accept. Includes the confirmed **`[W…]`
  warning-prefix gap**: C emits `warning: <text>` where Python emits
  `warning: [W<code>] <text>` (`src/api/errmsg.py:100` `WARNING_PREFIX`).
  The warning *text* matches; the bracketed code prefix + per-code
  registry are missing.
- **Leaking internal NYI stub.** A by-reference parameter given a
  non-variable argument makes C print an internal residue message
  (`visit_ARGUMENT byref array — non-ARRAYACCESS shape residue`) and
  exit 0 with a binary, where Python errors `Expected a variable name,
  not an expression (parameter By Reference)`. Both a false-positive and
  a "fail loudly, never stub" violation.
- **`zxbasm` error-message wording / parser error-recovery (11 FAIL).**
  `test-zxbasm` *error-test* harness 22 PASS / 11 FAIL — exact `stderr`
  text + post-error cascade on *invalid* assembler input (Python stops
  at the first `Unexpected token`; C reports a different message and a
  follow-on error). The *success* (binary-output) assembler tests are
  60/0. Distinct subsystem (assembler-parser error-recovery), pre-existing.
- **`.bin`-format INCBIN residue (2).** `test-zxbc-outfmt` is BYTE_EQUAL
  10 / BYTE_DIFF 0 / **SKIP_C_ERROR 2** — two `tap_incbin` fixtures the
  C path does not yet assemble; SKIP-C-error is never a legitimate
  terminal state.
- **`skip-known-bug` (chr / chr1 / const6).** Inherited from the
  abandoned port as "Python output known-wrong." Under the current bar
  these are **not** grandfathered: an entry is a legitimate skip only
  with an open upstream ticket or external evidence that Python is wrong
  at the pinned commit — otherwise C must match Python. Justification
  pending.
- **`skip-python` fixtures.** Corpus programs the Python reference itself
  rejects or cannot assemble (incl. cases where Python's *own* `zxbasm`
  cannot assemble Python's *own* emitted asm); excluded from the meter
  denominator because there is no ground truth to match — a Python-side
  limit, not a C defect.

The full remaining map is being completed mechanically by the
coverage-driven probe series (`csrc/tests/codegen_probes/`); each RED
probe becomes an item here until closed.

## Repository pointers

- Plan & architecture: [`c-port-plan.md`](c-port-plan.md),
  [`plans/`](plans/)
- C port sources: [`../csrc/`](../csrc/) (mirrors the Python `src/` layout)
- Stage meter: [`../csrc/tests/run_zxbc_stage_validation.sh`](../csrc/tests/run_zxbc_stage_validation.sh)
- Verification-probe series (coverage of untested codepaths): [`../csrc/tests/codegen_probes/`](../csrc/tests/codegen_probes/) (`run_probes.sh`)
- Changelog: [`CHANGELOG-c.md`](CHANGELOG-c.md)
- Python reference (read-only upstream mirror): `../src/`
