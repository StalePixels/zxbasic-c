# ZX BASIC C Port — Implementation Plan

## Project Overview

| Metric | Value |
|--------|-------|
| Python source | ~38,500 lines across 185 files |
| Test cases | 1,285 BASIC, 1,175 ASM expected, 62 binary expected, 91 preprocessor |
| External deps | Zero (pure Python + embedded PLY) |
| Entry points | 3: `zxbc` (compiler), `zxbasm` (assembler), `zxbpp` (preprocessor) |
| CLI flags | ~35 for zxbc, ~12 for zxbasm, ~5 for zxbpp |
| Estimated C output | ~70,000 lines |

## Compilation Pipeline (Python Original)

```
BASIC Source File
    |
[PREPROCESSING] (zxbpp)
    |   #include, #define, #ifdef
    |
[LEXICAL ANALYSIS] (zxblex — PLY lex)
    |
[PARSING] (zxbparser — PLY yacc, LALR(1))
    |
[AST] (50+ Symbol node types with inheritance)
    |
[OPTIMIZATION PASSES]
    |   Unreachable code removal
    |   Function call graph analysis
    |   Constant folding
    |
[IR GENERATION] (Translator — visitor pattern)
    |   AST -> Quads (~150 IC instruction types)
    |
[Z80 CODE EMISSION] (Backend)
    |   Quads -> Z80 assembly via dispatch table
    |   Peephole optimization
    |
[ASM PREPROCESSING]
    |   Runtime library inclusion
    |
[ASSEMBLY] (zxbasm — PLY yacc)
    |   Label resolution, instruction encoding
    |
[BINARY OUTPUT]
    |   .bin, .tap, .tzx, .sna, .z80 formats
    v
Final Output
```

## Architecture Mapping: Python to C

| Aspect | Python | C Approach |
|--------|--------|------------|
| Parsing | PLY (Python lex/yacc) | flex + bison (native, faster) |
| AST nodes | 50+ classes with inheritance | Tagged union structs with common header |
| Symbol table | Python dict with scoping | Hash table with scope chain |
| Strings | Python str (immutable, GC) | Arena-allocated, copied |
| Dynamic arrays | Python list | Growable array (`struct { T *data; int len, cap; }`) |
| Visitor pattern | Python methods | Function pointer tables or switch dispatch |
| IC instructions | Python enum | C enum + handler function pointer table |
| Memory management | Python GC | Arena allocator (free everything at end) |
| Config/options | Python class attributes | Global options struct |
| CLI parsing | argparse | getopt_long |

## Phases

### Phase 0: Infrastructure Setup

**Goal:** Project skeleton, build system, shared utilities, test harness.

- [ ] CMake or Makefile build system
- [ ] Common utilities: string handling, dynamic arrays, hash maps
- [ ] Arena memory allocator
- [ ] CLI argument parsing matching all three tools (`getopt_long`)
- [ ] Test harness: shell script that runs C binary, diffs output against expected files
- [ ] CI integration (GitHub Actions)

**Acceptance:** Project builds, test harness runs (all tests fail, but infrastructure works).

### Phase 1: Preprocessor (`zxbpp`)

**Goal:** Port the smallest, most independent component first as proof of concept.

**Python source:** ~1,500 lines across `src/zxbpp/`

Key components:
- [ ] Preprocessor lexer (flex) from `zxbpplex.py` / `zxbasmpplex.py`
- [ ] Preprocessor parser (bison) from `zxbpp.py`
- [ ] `#include` file handling with include path search
- [ ] `#define` / `#undef` macro expansion
- [ ] `#ifdef` / `#ifndef` / `#else` / `#endif` conditional compilation
- [ ] `#line` directive emission
- [ ] BASIC and ASM preprocessing modes
- [ ] CLI: `-o`, `-d`, `-e`, `--arch`, `--expect-warnings`

**Test cases:** 91 preprocessor tests (`.bi` -> `.out` comparison)

**Acceptance:** `zxbpp` C binary passes all 91 preprocessor tests.

### Phase 2: Assembler (`zxbasm`)

**Goal:** Self-contained Z80 assembler producing binary-exact output.

**Python source:** ~3,500 lines across `src/zxbasm/`

Key components:
- [ ] ASM lexer (flex) from `asmlex.py`
- [ ] ASM parser (bison) from `asmparse.py`
- [ ] Z80 instruction encoding (all opcodes, addressing modes)
- [ ] ZX Next extended opcodes
- [ ] Expression evaluation (labels, constants, arithmetic)
- [ ] Memory model with ORG support
- [ ] Label resolution (two-pass or fixup)
- [ ] Macro support and `#include` (reuse Phase 1 preprocessor)
- [ ] Output formats:
  - [ ] `.bin` (raw binary)
  - [ ] `.tap` (TAP tape format)
  - [ ] `.tzx` (TZX tape format)
  - [ ] `.sna` (SNA snapshot)
  - [ ] `.z80` (Z80 snapshot)
- [ ] BASIC loader generation
- [ ] Memory map output (`-M`)
- [ ] CLI: all 12 flags

**Test cases:** 62 binary-exact test files

**Acceptance:** `zxbasm` C binary passes all assembler tests with byte-for-byte identical output.

### Phase 3: BASIC Compiler Frontend

**Goal:** Lexer, parser, AST construction, symbol table, type system.

**Python source:** ~5,500 lines across `src/zxbc/`, `src/symbols/`, `src/api/symboltable/`

Key components:
- [ ] BASIC lexer (flex) from `zxblex.py`
  - Multiple lexer states: string, asm, preproc, comment, bin
  - Keyword recognition from `keywords.py`
- [ ] BASIC parser (bison) from `zxbparser.py` (~3,600 lines of grammar rules)
  - All BASIC statements: LET, IF, FOR, WHILE, DO, PRINT, INPUT, etc.
  - Expressions with operator precedence
  - Function/SUB declarations
  - Array declarations and access
  - Type casting
- [ ] AST node types (C structs) from `src/symbols/` (50+ types)
  - Common node header (tag, type, children, line number)
  - Tagged union or struct-per-type approach
- [ ] Symbol table with lexical scoping from `src/api/symboltable/`
- [ ] Type system: `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `f16`, `f`, `str`
- [ ] Semantic checking from `src/api/check.py`

**Test cases:** Parser correctness validated indirectly via Phase 5 ASM output tests.

**Acceptance:** Frontend parses all 1,285 test `.bas` files without errors.

### Phase 4: Optimizer + IR Generation

**Goal:** AST optimization passes and translation to intermediate code (Quads).

**Python source:** ~3,000 lines across `src/api/optimize.py`, `src/arch/z80/visitor/`

Key components:
- [ ] AST visitor framework (function pointer dispatch)
- [ ] Unreachable code removal pass
- [ ] Function call graph analysis
- [ ] Constant folding / expression simplification
- [ ] Translator: AST -> Quads (visitor pattern over AST)
  - ~150 IC instruction types (`ICInstruction` enum)
  - Quad format: `(instruction, arg1, arg2, arg3)`
  - Temporary variable management
  - String deduplication
  - DATA block emission
  - Jump table generation
- [ ] Function translator (separate pass for SUB/FUNCTION bodies)

**Test cases:** Validated indirectly via Phase 5 ASM output.

**Acceptance:** IR generation produces correct quads for all test programs.

### Phase 5: Z80 Backend

**Goal:** Convert Quads to Z80 assembly, apply peephole optimisation.

**Python source:** ~6,000 lines across `src/arch/z80/backend/`, `src/arch/z80/peephole/`, `src/arch/z80/optimizer/`

Key components:
- [ ] Backend dispatch table: IC instruction -> handler function
- [ ] Code emitters by operand width:
  - [ ] `_8bit.c` (8-bit operations)
  - [ ] `_16bit.c` (16-bit operations)
  - [ ] `_32bit.c` (32-bit operations)
  - [ ] `_f16.c` (16-bit fixed-point)
  - [ ] `_float.c` (floating point)
  - [ ] `_string.c` (string operations)
- [ ] Runtime library integration (`src/arch/z80/backend/runtime/`)
- [ ] Peephole optimizer
  - Pattern matching engine from `src/arch/z80/peephole/engine.py`
  - Optimization patterns from `src/arch/z80/peephole/opts/`
- [ ] Assembly-level optimizer from `src/arch/z80/optimizer/`
  - CPU state tracking (`cpustate.py`)
  - Basic block analysis (`basicblock.py`)
- [ ] Architecture variants:
  - [ ] ZX Spectrum 48K (`src/arch/zx48k/`)
  - [ ] ZX Next (`src/arch/zxnext/`)

**Test cases:** 1,175 ASM output comparison tests

**Acceptance:** `zxbc -f asm` produces identical `.asm` output for all test cases.

### Phase 6: Full Integration + Output Formats

**Goal:** Complete pipeline from BASIC source to final binary, all output formats.

**Python source:** ~2,000 lines across `src/outfmt/`, `src/zxbc/zxbc.py`

Key components:
- [ ] Full compilation pipeline orchestration
- [ ] ASM preprocessing pass (runtime library `#include`)
- [ ] Variable declaration emission
- [ ] Integration with Phase 2 assembler for binary generation
- [ ] Output format generators from `src/outfmt/`:
  - [ ] Binary (`.bin`)
  - [ ] TAP tape (`.tap`)
  - [ ] TZX tape (`.tzx`)
  - [ ] SNA snapshot (`.sna`)
  - [ ] Z80 snapshot (`.z80`)
- [ ] BASIC loader generation (`-B` flag)
- [ ] Autorun support (`-a` flag)
- [ ] Memory map generation (`-M` flag)
- [ ] Headerless mode (`--headerless`)
- [ ] Append binary to tape (`--append-binary`, `--append-headless-binary`)
- [ ] Config file loading/saving (`-F`, `--save-config`)
- [ ] Full CLI with all ~35 flags

**Test cases:** All 1,285 functional tests (ASM + binary output)

**Acceptance:** All tests pass. C binaries are drop-in replacements for Python originals.

## Test Verification Strategy

The existing test infrastructure can be reused with minimal changes:

1. **Drop-in replacement** — C binaries named `zxbc`, `zxbasm`, `zxbpp` with identical CLI
2. **Same expected outputs** — all `.asm`, `.bin`, `.out` expected files unchanged
3. **Text comparison** — ASM output compared after stripping paths and `#line` directives
4. **Binary comparison** — `.bin`/`.tap`/`.tzx`/`.sna`/`.z80` compared byte-for-byte
5. **Incremental validation** — each phase has its own subset of passing tests

Simple shell-based test runner (independent of pytest):
```bash
# For each .bas with a matching expected .asm:
./zxbc input.bas -o output.asm -f asm [flags]
diff -u expected.asm output.asm

# For each .asm with a matching expected .bin:
./zxbasm input.asm -o output.bin
cmp expected.bin output.bin
```

## File Size Estimates

| Phase | Estimated C Lines | Python Lines Ported |
|-------|-------------------|---------------------|
| Phase 0 (infra) | ~3,000 | — |
| Phase 1 (zxbpp) | ~3,000 | ~1,500 |
| Phase 2 (zxbasm) | ~8,000 | ~3,500 |
| Phase 3 (frontend) | ~15,000 | ~5,500 |
| Phase 4 (optimizer+IR) | ~10,000 | ~3,000 |
| Phase 5 (backend) | ~25,000 | ~6,000 |
| Phase 6 (integration) | ~5,000 | ~2,000 |
| **Total** | **~69,000** | **~21,500** |

## Key Risks

- **Floating-point determinism** — f16/float operations must produce identical bit patterns. May require matching Python's exact rounding behaviour.
- **String handling edge cases** — Python's string semantics (immutable, Unicode) vs C's byte arrays. The original targets 8-bit character sets so this should be manageable.
- **PLY grammar ambiguities** — PLY's LALR(1) conflict resolution may differ subtly from bison's. Parser table equivalence must be validated.
- **Peephole pattern ordering** — Optimization pass ordering must match exactly, since different orderings can produce different (but functionally equivalent) output that would fail byte-for-byte comparison.
