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
cd csrc/build && cmake .. && make
```

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
- Each component (zxbpp, zxbasm, zxbc) is a standalone executable with its own `main.c`

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

```bash
# Build and quick test:
cd csrc/build && cmake .. && make -j4 && cd ../..
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp

# Full Python comparison (slower, requires Python 3.11+):
./csrc/tests/compare_python_c.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp
```

### Test File Conventions

Each component has its own input/output file types:

| Component | Input | Expected Output | Test Dir |
|-----------|-------|-----------------|----------|
| zxbpp | `.bi` | `.out` (stdout), `.err` (errors) | `tests/functional/zxbpp/` |
| zxbasm | `.asm` | `.bin` (binary) | `tests/functional/asm/` |
| zxbc | `.bas` | varies (`.asm`, `.bin`, `.tap`, etc.) | `tests/functional/arch/` |

- C binaries must accept **identical CLI flags** as the Python originals
- Python test runner: `tests/functional/test.py` (used by pytest via `test_prepro.py`, `test_asm.py`, `test_basic.py`)

## Keeping Things Up To Date

This project has several living documents and CI artefacts that MUST stay in sync with the code. When you add features, fix bugs, or complete phases:

- **README.md** — Update the status table, test counts, and phase progress. The `zxbpp tests` badge is static (`96%2F96` etc.) — update the number when tests are added. The `C Build` badge is live from CI.
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
