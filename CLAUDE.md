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

The gold standard is **each C binary as a drop-in replacement for its Python counterpart** — run the same tests, get the same output, no one can tell the difference. This applies to every component: zxbpp, zxbasm, zxbc.

### Python Reference (Ground Truth)

The original Python toolchain is the source of truth. Use Python 3.12+ to run any component:

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

# Full Python comparison (slower, requires Python 3.12):
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

## Pitfalls

- **PLY vs bison** — PLY's LALR(1) conflict resolution can differ from bison's. Validate parser table equivalence.
- **Path handling** — Test outputs contain file paths; C output must produce identical path formats.
- **Macro expansion** — This is a BASIC preprocessor with its own rules, not a C preprocessor. Don't assume standard semantics.
- **Floating-point determinism** — f16/float operations must produce identical bit patterns to Python.
- **Peephole pattern ordering** — Optimization pass ordering must match exactly for byte-identical output.
