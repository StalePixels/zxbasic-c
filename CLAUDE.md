# CLAUDE.md — ZX BASIC C Port

## What This Project Is

A **C port of [Boriel ZX BASIC](https://github.com/boriel-basic/zxbasic)**, a Python compiler toolchain (~38,500 lines) for the ZX Spectrum. The port targets embedding on resource-constrained platforms (NextPi) where Python is impractical.

**This is a port, not a rewrite.** Always reference how the Python does it. The goal is **byte-for-byte identical output** — same inputs, same flags, same output files.

## Repository Layout

```
zxbasic-c/
├── src/                    # ORIGINAL Python source (READ-ONLY reference)
│   ├── zxbpp/              # Preprocessor
│   ├── zxbasm/             # Assembler
│   ├── zxbc/               # Compiler frontend
│   ├── symbols/            # AST node types
│   ├── api/                # Symbol table, type checking, optimization
│   ├── arch/               # Z80 backend, peephole optimizer
│   ├── outfmt/             # Output format generators (.tap, .tzx, .sna, .z80)
│   └── ply/                # Embedded PLY (Python Lex-Yacc)
├── csrc/                   # C PORT (our work)
│   ├── CMakeLists.txt      # Top-level CMake build
│   ├── common/             # Shared utilities (arena, strbuf, vec, hashmap)
│   └── ...                 # Component subdirectories mirror src/
├── tests/functional/       # Test suite (shared with Python original)
├── docs/c-port-plan.md     # Full implementation plan with phase breakdown
├── zxbpp.py, zxbc.py, zxbasm.py  # Python entry points (reference)
```

## Build

```bash
cmake -S csrc -B csrc/build -DCMAKE_BUILD_TYPE=Release
cmake --build csrc/build -j8
```

Produces `csrc/build/bin/zxbasic-suite` (the real binary) plus
`zxbpp`/`zxbasm`/`zxbc` symlinks next to it that dispatch via argv[0].
On Windows the symlinks are replaced with `.exe` copies (no symlink
primitive). Opt-in `-DZXBASIC_BUILD_STANDALONE=ON` adds separate
per-tool executables for single-applet debugging; not the default.

## Key Rules

1. **Always read the Python source first** when unsure about behavior. The C port must match exactly, not what you think the code "should" do.
2. **Do not modify `src/`** — that's the Python reference implementation (synced from upstream).
3. **Do not modify `tests/`** — those are shared test fixtures (synced from upstream).
4. **NEVER push to `python-upstream` or `boriel-basic/zxbasic`** — that is Boriel's repo. We are read-only consumers. All our work goes to `origin` (`StalePixels/zxbasic-c`) only.
5. **No external dependencies** — the Python original has zero; the C port should match.
6. **Battle-tested over hand-rolled** — when cross-platform portability shims or utilities are needed, use a proven, permissively-licensed library (e.g. ya_getopt for getopt_long) rather than writing a homebrew implementation. Tried-and-tested > vibe-coded.
7. **See `docs/c-port-plan.md`** for the full phased implementation plan, architecture mapping, and test strategy.
8. **Never discard, void, or stub out parsed values.** This is a byte-for-byte compiler port. Every language construct must produce correct AST output — no `(void)result`, no token-skipping loops, no "consume until newline" shortcuts. If the Python builds an AST node from a parsed value, the C must too. If you don't know how to handle a construct, stop and study the Python — don't guess, don't skip, don't stub. A parse that silently drops data is worse than a parse that fails loudly.
9. **No speculative or guess-based parsing.** Don't invent grammar rules or function signatures. Every parser handler must correspond to an actual Python grammar production. Read `src/zxbc/zxbparser.py` (or the relevant Python source) and implement exactly what it does — including which AST nodes are created, what children they have, and what names/tags they use.

## Architecture Decisions

| Aspect | Python Original | C Approach |
|--------|----------------|------------|
| Parsing (zxbpp) | PLY lex/yacc | Hand-written recursive-descent |
| Parsing (zxbasm, zxbc) | PLY lex/yacc | Hand-written recursive-descent |
| AST nodes | 50+ classes with inheritance | Tagged union structs with common header |
| Memory | Python GC | Arena allocator (allocate during compilation, free all at end) |
| Strings | Python str (immutable) | `StrBuf` (growable) + arena-allocated `char*` |
| Dynamic arrays | Python list | `VEC(T)` macro (type-safe growable array) |
| Hash tables | Python dict | `HashMap` (string-keyed, open addressing) |
| CLI | argparse | `ya_getopt` (BSD-2-Clause, bundled) |
| Path manipulation | `os.path` | `cwalk` (MIT, bundled) |
| Cross-platform compat | N/A (Python) | `compat.h` (thin MSVC shims) |

## Common Utilities (csrc/common/)

- **`arena.h`** — Arena allocator: `arena_init`, `arena_alloc`, `arena_strdup`, `arena_destroy`
- **`strbuf.h`** — Growable string buffer: `strbuf_init`, `strbuf_append`, `strbuf_cstr`, `strbuf_free`
- **`vec.h`** — Type-safe dynamic array: `VEC(T)`, `vec_init`, `vec_push`, `vec_pop`, `vec_free`
- **`hashmap.h`** — String-keyed hash map: `hashmap_init`, `hashmap_set`, `hashmap_get`, `hashmap_remove`

## Bundled Libraries (csrc/common/)

These are vendored, permissively-licensed libraries chosen over hand-rolled implementations (see rule 6):

- **`ya_getopt.h`/`.c`** — Portable `getopt_long` ([ya_getopt](https://github.com/kubo/ya_getopt), BSD-2-Clause). Drop-in replacement for POSIX getopt on all platforms including MSVC.
- **`cwalk.h`/`.c`** — Cross-platform path manipulation ([cwalk](https://github.com/likle/cwalk), MIT). Provides `cwk_path_get_basename`, `cwk_path_get_dirname`, `cwk_path_get_extension`, etc. Set `cwk_path_set_style(CWK_STYLE_UNIX)` at startup for consistent forward-slash paths.
- **`compat.h`** — Minimal POSIX→MSVC shim (our own). Only contains `#define` aliases (`strncasecmp`→`_strnicmp`, etc.) and thin wrappers for OS calls (`realpath`→`_fullpath`, `getcwd`→`_getcwd`) with backslash normalization. No path logic — that's cwalk's job.

## Coding Conventions

- C11 standard, warnings: `-Wall -Wextra -Wpedantic`
- Structs use `typedef` (e.g., `typedef struct Foo { ... } Foo;`)
- Arena allocation preferred over malloc/free for compiler data
- State passed via struct pointer — no globals
- Each component (zxbpp, zxbasm, zxbc) has its own `main.c` exposing a
  `<tool>_main(argc, argv)` entry point. In the production build these
  are linked into the multicall `zxbasic-suite` binary which dispatches
  via argv[0]; an opt-in `ZXBASIC_BUILD_STANDALONE` build re-adds a
  thin `main()` wrapper around each `*_main` to produce separate
  per-tool executables for single-applet debugging.

## Testing

The gold standard is **each C binary as a drop-in replacement for its Python counterpart** — run the same tests, get the same output, no one can tell the difference. This applies to every component: zxbpp, zxbasm, zxbc.

### Python Reference (Ground Truth)

The original Python toolchain is the source of truth. Use Python 3.11+ to run any component:

```bash
# Run any Python component from project root:
python3 -c "
import sys; sys.path.insert(0, '.')
from src.<component>.<component> import entry_point
sys.argv = ['<component>', '<input_file>']
result = entry_point()
sys.exit(result)
"
```

### Test Strategy Per Component

Each component gets two test harnesses in `csrc/tests/`:

1. **`run_<component>_tests.sh <binary> <test-dir>`** — Standalone: compares C output against `.out`/`.err` reference files.
2. **`compare_python_c.sh <binary> <test-dir>`** — Runs BOTH Python and C on every test, diffs outputs. The ultimate proof of drop-in equivalence.

Always validate against Python when adding features — don't trust assumptions.

**What "matching" means:** A test "matches" when C and Python produce the **same exit code** for the same input file and flags. Not "C exits 0" — that only measures syntax parsing. Python's `--parse-only` runs full semantic analysis, post-parse validation, and AST visitors before returning. A file that Python rejects with exit code 1 (semantic error) must also be rejected by C with exit code 1.

```bash
# Build and quick test:
cmake -S csrc -B csrc/build -DCMAKE_BUILD_TYPE=Release
cmake --build csrc/build -j4
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/bin/zxbpp tests/functional/zxbpp

# Full Python comparison (slower, requires Python 3.11+):
./csrc/tests/compare_python_c.sh ./csrc/build/bin/zxbpp tests/functional/zxbpp
```

### Test File Conventions

Each component has its own input/output file types:

| Component | Input | Expected Output | Test Dir |
|-----------|-------|-----------------|----------|
| zxbpp | `.bi` | `.out` (stdout), `.err` (errors) | `tests/functional/zxbpp/` |
| zxbasm | `.asm` | `.bin` (binary) | `tests/functional/asm/` |
| zxbc | `.bas` | `.asm` (assembly output) | `tests/functional/arch/zx48k/` |

- C binaries must accept **identical CLI flags** as the Python originals
- Python test runner: `tests/functional/test.py` (used by pytest via `test_prepro.py`, `test_asm.py`, `test_basic.py`)

### Beyond Functional Tests — Full Test Inventory

The Python project has unit and integration tests beyond the functional `.bas`/`.bi`/`.asm` files. **All of these must be matched by the C port.** Don't assume functional tests are sufficient.

| Suite | Location | What it tests | Files |
|-------|----------|---------------|-------|
| CLI / flags | `tests/cmdline/` | `--parse-only`, `--org` hex, `-F` config file, cmdline-overrides-config | `test_zxb.py` + fixtures |
| API / config | `tests/api/` | arg parser defaults, type checking, symbol table, config, utils | 5 test files |
| AST nodes | `tests/symbols/` | Node construction for all 20 AST node types | 20 test files |
| Backend | `tests/arch/zx48k/backend/` | Memory cell operations | 1 test file |
| Optimizer | `tests/arch/zx48k/optimizer/` | Basic blocks, CPU state, optimizer passes | 6 test files |
| Peephole | `tests/arch/zx48k/peephole/` | Pattern matching, evaluation, templates | 4 test files |
| Compiler | `tests/zxbc/` | Parser table generation | 1 test file |

### Codegen Probes — the enumeration meter (`csrc/tests/codegen_probes/`)

**Use this, not just the corpus.** The inherited `tests/functional/` corpus is byte-truth ONLY for codepaths it happens to exercise. "All corpus tests pass" does NOT prove the port has every Python check. The 2026-05-21 probe-batch-2 finding documented this structurally: "C's post-parse SEMANTIC-VALIDATION layer is largely ABSENT — a missing subsystem, not edge bugs." Driving work fixture-by-fixture from the corpus misses the gaps the corpus is silent about. The probe series is the enumeration tool that catches them.

- **Layout:** `csrc/tests/codegen_probes/<category>/<fixture>.bas`. Eight categories: `arithmetic`, `arrays`, `controlflow`, `errors`, `strings`, `switches`, `typecast`, `warnings`. ~90 hand-authored fixtures targeting codepaths the corpus skips.
- **Harness:** `./csrc/tests/codegen_probes/run_probes.sh <category>` (one category at a time; absolute paths also accepted).
- **Contract:** per fixture, compares C vs the Python oracle on FOUR parts (in this precedence order — bucket localises the first divergence):
  1. exit code → `PROBE-DIFF-EXIT`
  2. stderr (path-normalised) → `PROBE-DIFF-STDERR`
  3. Stage-1 ASM (`--output-format=asm`, path-normalised) → `PROBE-DIFF-ASM`
  4. End-to-end binary (default `-o out.bin`, RAW cmp — never normalise binary) → `PROBE-DIFF-BIN`

  All four match → `PROBE-EQUAL`. Python traceback / internal error → `SKIP-PY-ERROR`. Stderr surfaces W-warnings and typecast/semantic errors that the staged meters do NOT check — a divergence here is invisible to those meters.
- **Coverage harness:** `csrc/tests/codegen_probes/_coverage/cov_driver.py` — in-process coverage driver authored alongside the probes. Use as guidance for where to add new probes (not as gospel).
- **When you fix a class of divergence, write a probe for it.** Probes are additive — every new C codepath that diverges (or did diverge) earns a probe so the gap can't silently regress.
- **Don't loosen the probe runner.** It's deliberately strict on byte-cmp; failures classify by the EARLIEST divergence so the bucket is diagnostic. Adding skip switches or tolerance to make a probe pass is the wrong fix — the probe is right; the C is wrong.

### Released-program corpus — the real-world meter (`csrc/tests/released_corpus/`)

Parity meter over real shipped ZX BASIC programs (games/demos/utils from
zxbasic.readthedocs.io/released_programs). Same 4-part first-divergence
contract as the probe runner; always exits 0 (diagnostic). `./fetch.sh`
captures sha256-pinned archives into a gitignored `cache/` (third-party bytes
are never committed), `./run_corpus.sh` runs the meter, `--diff <id>` dumps a
program's first divergence. Read its `README.md` for tiers (BINARY-EQUAL /
FRONTEND-EQUAL), the drift guard, and the `fixups/` flyby-fix mechanism.
**Local/manual only — not in `make test` or CI** (by-design not green while
DIFF-* findings are open; fetch needs network). Same discipline as the probes:
don't loosen the runner, don't re-pin a manifest sha to silence DRIFT without
confirming the new bytes. As of 2026-06-12 (after the string-literal embedded-NUL fix): 29
programs — 5 BINARY-EQUAL, 10 FRONTEND-EQUAL, 0 DIFF-EXIT, 14 DIFF-STDERR
(14 open findings). The DIFF-EXIT findings (o-trix, retrobsesion) were the
C assembler's flat-64K memory image aborting on a >64K emission where Python's
sparse-dict model tolerates it — fixed via `csrc/zxbasm/zxbasm.h MAX_MEM =
0x20000`. retrobsesion's deeper divergence was NOT a string-array-constant gap
(that prior triage was wrong): ZX BASIC string literals with control-code
escapes (`\{p0}` → bytes `11 00`) lex to embedded NULs, and the C lexer/parser
truncated them at the first NUL via `arena_strdup`/`strlen`. Fixed by carrying an
explicit byte length lexer → token → AST_STRING (`make_string_n`); the codegen
chain was already NUL-safe. retrobsesion, saltarin and walking-around-porto all
reached BINARY-EQUAL from that single fix (plus a same-day W170/W190
include-filename-attribution fix that cleared retrobsesion's shallower stderr
divergence first).

## Keeping Things Up To Date

This project has several living documents and CI artefacts that MUST stay in sync with the code. When you add features, fix bugs, or complete phases:

- **README.md — refresh WITH EVERY BANKED PORT COMMIT.** Not "as phases complete" — every time a fix-agent's cluster is banked. The badges (parse-meter count, probe count + RED/in-progress count), the Phase 3 numbers (parse PASS, omatrix EQUAL/BIN-DIFF, stages both archs), the probe meter section, and the road-to-NextPi ASCII map are all status surfaces that go stale fast. If the bank changes any of those numbers — including +1 RED on a newly-authored probe — bump the README in the SAME bank commit (or the very next one). Stale badge numbers are project lying; don't leave them lying. **Numbers, not narrative** (maintainer rule, 2026-06-11): refresh means updating the current-state counts — it does NOT mean adding dated finding writeups, fix histories, or per-bug callout blocks to the README. The README is not a bug tracker; root-cause history goes in commit messages, open findings in the suite that tracks them.
- **CLAUDE.md** (this file) — Update test file conventions table, test commands, and any new component patterns as phases are completed.
- **docs/c-port-plan.md** — Check off completed items as phases progress.
- **docs/plans/** — WIP progress files for active branches.
- **CI workflow** (`.github/workflows/c-build.yml`) — Add new test steps as components are completed. The workflow builds on Linux x86_64, macOS ARM64, and Windows x86_64, runs tests on all three, and does a Python ground-truth comparison on Linux. Note: zxbpp text tests are skipped on Windows (path differences in `#line` directives); zxbasm binary tests run everywhere.
- **Test harnesses** (`csrc/tests/`) — Each new component needs its own `run_<component>_tests.sh` and an entry in `compare_python_c.sh` (or a component-specific comparison script).

If test counts change, the README badge lies until you fix it. Don't leave it lying.

## Pitfalls

- **PLY vs bison** — PLY's LALR(1) conflict resolution can differ from bison's. Validate parser table equivalence.
- **Path handling** — Test outputs contain file paths; C output must produce identical path formats.
- **Macro expansion** — This is a BASIC preprocessor with its own rules, not a C preprocessor. Don't assume standard semantics.
- **Floating-point determinism** — f16/float operations must produce identical bit patterns to Python.
- **Peephole pattern ordering** — Optimization pass ordering must match exactly for byte-identical output.
