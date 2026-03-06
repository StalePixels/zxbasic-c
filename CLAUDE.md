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
├── plan.md                 # Full implementation plan with phase breakdown
├── zxbpp.py, zxbc.py, zxbasm.py  # Python entry points (reference)
```

## Build

```bash
cd csrc/build && cmake .. && make
```

## Key Rules

1. **Always read the Python source first** when unsure about behavior. The C port must match exactly, not what you think the code "should" do.
2. **Do not modify `src/`** — that's the Python reference implementation.
3. **Do not modify `tests/`** — those are shared test fixtures used by both Python and C.
4. **No external dependencies** — the Python original has zero; the C port should match.
5. **See `plan.md`** for the full phased implementation plan, architecture mapping, and test strategy.

## Architecture Decisions

| Aspect | Python Original | C Approach |
|--------|----------------|------------|
| Parsing (zxbpp) | PLY lex/yacc | Hand-written recursive-descent |
| Parsing (zxbasm, zxbc) | PLY lex/yacc | flex + bison |
| AST nodes | 50+ classes with inheritance | Tagged union structs with common header |
| Memory | Python GC | Arena allocator (allocate during compilation, free all at end) |
| Strings | Python str (immutable) | `StrBuf` (growable) + arena-allocated `char*` |
| Dynamic arrays | Python list | `VEC(T)` macro (type-safe growable array) |
| Hash tables | Python dict | `HashMap` (string-keyed, open addressing) |
| CLI | argparse | `getopt_long` |

## Common Utilities (csrc/common/)

- **`arena.h`** — Arena allocator: `arena_init`, `arena_alloc`, `arena_strdup`, `arena_destroy`
- **`strbuf.h`** — Growable string buffer: `strbuf_init`, `strbuf_append`, `strbuf_cstr`, `strbuf_free`
- **`vec.h`** — Type-safe dynamic array: `VEC(T)`, `vec_init`, `vec_push`, `vec_pop`, `vec_free`
- **`hashmap.h`** — String-keyed hash map: `hashmap_init`, `hashmap_set`, `hashmap_get`, `hashmap_remove`

## Coding Conventions

- C11 standard, warnings: `-Wall -Wextra -Wpedantic`
- Structs use `typedef` (e.g., `typedef struct Foo { ... } Foo;`)
- Arena allocation preferred over malloc/free for compiler data
- State passed via struct pointer — no globals
- Each component (zxbpp, zxbasm, zxbc) is a standalone executable with its own `main.c`

## Testing

The gold standard is **the C binary as a drop-in replacement for Python** — run the same tests, get the same output, no one can tell the difference.

### Python Reference (Ground Truth)

The original Python preprocessor is the source of truth. Use brew Python 3.12 to run it:

```bash
# Python 3.12 via Homebrew
/opt/homebrew/bin/python3.12

# Run Python zxbpp from project root:
/opt/homebrew/bin/python3.12 -c "
import sys; sys.path.insert(0, '.')
from src.zxbpp.zxbpp import entry_point
sys.argv = ['zxbpp', 'tests/functional/zxbpp/prepro01.bi']
result = entry_point()
sys.exit(result)
"
```

### Test Harnesses

- **`csrc/tests/run_zxbpp_tests.sh <binary> <test-dir>`** — Standalone test harness. Compares C output against `.out` files (normal tests) and `.err` files (error tests). Currently: **96/96 passing**.
- **`csrc/tests/compare_python_c.sh <binary> <test-dir>`** — Runs BOTH Python and C on every test, diffs their outputs. This is the ultimate proof: **91/91 identical** (5 helper .bi files skipped).

```bash
# Quick test after changes:
cd csrc/build && make -j4 && cd ../..
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp

# Full Python comparison (slower, requires Python 3.12):
./csrc/tests/compare_python_c.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp
```

### Test File Types

- `.bi` — Input file (BASIC source with preprocessor directives)
- `.out` — Expected stdout (91 normal tests)
- `.err` — Expected error description (5 error tests: prepro07, 22, 28, 35, 76)

### Key Rules

- C binaries must accept **identical CLI flags** as the Python originals
- Tests compare C output against the **same expected files** used by Python's test suite
- Always validate against Python when adding features — don't trust assumptions

## Pitfalls

- **PLY vs bison** — PLY's LALR(1) conflict resolution can differ from bison's. Validate parser table equivalence.
- **Path handling** — Test outputs contain file paths; C output must produce identical path formats.
- **Macro expansion** — This is a BASIC preprocessor with its own rules, not a C preprocessor. Don't assume standard semantics.
- **Floating-point determinism** — f16/float operations must produce identical bit patterns to Python.
- **Peephole pattern ordering** — Optimization pass ordering must match exactly for byte-identical output.
