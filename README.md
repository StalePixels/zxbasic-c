
![Boriel ZX Basic](img/zxbasic_logo.png)

[![license](https://img.shields.io/badge/License-AGPLv3-blue.svg)](./LICENSE.txt)
[![C Build](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml/badge.svg)](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml)
[![zxbpp tests](https://img.shields.io/badge/zxbpp_tests-96%2F96_passing-brightgreen)](#-phase-1--preprocessor-done)
[![zxbasm tests](https://img.shields.io/badge/zxbasm_tests-61%2F61_passing-brightgreen)](#-phase-2--assembler-done)

ZX BASIC — C Port 🚀
---------------------

A **C language port** of the [Boriel ZX BASIC compiler](https://github.com/boriel-basic/zxbasic),
originally written in Python by Jose Rodriguez-Rosa (a.k.a. Boriel).

## 🎯 What is this?

This is an **agentic porting experiment** — a test of whether an AI coding assistant can
systematically port a non-trivial compiler (~38,500 lines of Python) to C, producing a
drop-in replacement with **byte-for-byte identical output**.

The toolchain being ported — `zxbc` (compiler), `zxbasm` (assembler), and `zxbpp`
(preprocessor) — is validated stage by stage against the original's comprehensive test
suite of 1,285+ functional tests.

The practical end-goal: a C implementation of the compiler suitable for **embedding on
[NextPi](https://www.specnext.com/)** and similar resource-constrained platforms where
a full modern Python runtime is undesirable.

## 📊 Current Status

| Phase | Component | Tests | Status |
|-------|-----------|-------|--------|
| 0 | Infrastructure (arena, strbuf, vec, hashmap, CMake) | — | ✅ Complete |
| 1 | **Preprocessor (`zxbpp`)** | **96/96** 🎉 | ✅ Complete |
| 2 | **Assembler (`zxbasm`)** | **61/61** 🎉 | ✅ Complete |
| 3 | BASIC compiler frontend (lexer + parser + AST) | — | ⏳ Planned |
| 4 | Optimizer + IR generation (AST → Quads) | — | ⏳ Planned |
| 5 | Z80 backend (Quads → Assembly) — 1,175 ASM tests | — | ⏳ Planned |
| 6 | Full integration + all output formats | — | ⏳ Planned |

### 🔬 Phase 2 — Assembler: Done!

The `zxbasm` C binary is a **verified drop-in replacement** for the Python original:

- ✅ **61/61 tests passing** — zero failures, byte-for-byte identical binary output
- ✅ **61/61 Python comparison** — confirmed by running both side-by-side
- ✅ Full Z80 instruction set (827 opcodes) including ZX Next extensions
- ✅ Two-pass assembly: labels, forward references, expressions, temporaries
- ✅ PROC/ENDP scoping, LOCAL labels, PUSH/POP NAMESPACE
- ✅ `#init` directive, EQU/DEFL, ORG, ALIGN, INCBIN
- ✅ Hand-written recursive-descent parser (~1,750 lines of C)
- ✅ Preprocessor integration (reuses the C zxbpp binary)

### 🔬 Phase 1 — Preprocessor: Done!

The `zxbpp` C binary is a **verified drop-in replacement** for the Python original:

- ✅ **96/96 tests passing** (91 normal + 5 error tests) — zero skipped
- ✅ **91/91 outputs identical to Python** — confirmed by running both side-by-side
- ✅ All preprocessor features: `#define`, `#include`, `#ifdef`/`#if`, macro expansion, token pasting, stringizing, block comments, ASM mode, line continuation, `#pragma`/`#require`/`#init`/`#error`/`#warning`, architecture-specific includes
- ✅ Hand-written recursive-descent parser (~1,600 lines of C)

## 🧪 Try It Yourself

### Building

```bash
git clone https://github.com/StalePixels/zxbasic-c.git
cd zxbasic-c
mkdir -p csrc/build && cd csrc/build
cmake ..
make -j4
```

This builds `csrc/build/zxbpp/zxbpp` and `csrc/build/zxbasm/zxbasm`.

### Running the Tests

```bash
# Run all 96 preprocessor tests:
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp

# Run all 61 assembler tests (binary-exact):
./csrc/tests/run_zxbasm_tests.sh ./csrc/build/zxbasm/zxbasm tests/functional/asm
```

### 🐍 Python Ground-Truth Comparison

Want to see for yourself that C matches Python? You'll need Python 3.11+:

```bash
# Install Python 3.12+ if you don't have it:
#   macOS:   brew install python@3.11  (or newer)
#   Ubuntu:  sudo apt install python3
#   Fedora:  sudo dnf install python3

# Run both Python and C on every test, diff the outputs:
./csrc/tests/compare_python_c.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp
./csrc/tests/compare_python_c_asm.sh ./csrc/build/zxbasm/zxbasm tests/functional/asm
```

This runs the original Python tools and the C ports on all test inputs and
confirms their outputs are identical. 🤝

## 🔧 Using the C Preprocessor Today

The C `zxbpp` binary accepts the **exact same flags** as the Python original. You can
drop it into an existing Boriel ZX BASIC workflow right now for the preprocessing step:

```bash
# Instead of:
python3 zxbpp.py myfile.bas -o myfile.preprocessed.bas

# Use:
./csrc/build/zxbpp/zxbpp myfile.bas -o myfile.preprocessed.bas
```

Supported flags: `-o`, `-d`, `-e`, `-D`, `-I`, `--arch`, `--expect-warnings`

Supported flags: `-d`, `-e`, `-o`, `-O` (output format)

The `zxbasm` assembler is also available as a drop-in replacement:

```bash
# Instead of:
python3 zxbasm.py myfile.asm -o myfile.bin

# Use:
./csrc/build/zxbasm/zxbasm myfile.asm -o myfile.bin
```

The compiler frontend (`zxbc`) still requires Python — for now. 😏

## 🗺️ The Road to NextPi

The big picture: a fully native C compiler toolchain that runs on the
[NextPi](https://www.specnext.com/) — a Raspberry Pi accelerator board for the
ZX Spectrum Next. No Python runtime needed, just a single binary.

Here's how we get there, one step at a time:

```
 Phase 0  ✅  Infrastructure — arena allocator, strings, vectors, hash maps
    │
 Phase 1  ✅  zxbpp — Preprocessor
    │         96/96 tests, drop-in replacement for Python's zxbpp
    │
 Phase 2  ✅  zxbasm — Z80 Assembler (you are here! 📍)
    │         61/61 binary-exact tests passing
    │         zxbpp + zxbasm work without Python!
    │
 Phase 3  ⏳  BASIC Frontend — Lexer, parser, AST, symbol table
    │
 Phase 4  ⏳  Optimizer + IR — AST → Quads intermediate code
    │
 Phase 5  ⏳  Z80 Backend — Quads → Assembly + peephole optimizer
    │         1,175 ASM tests + 1,285 BASIC tests to pass
    │
 Phase 6  ⏳  Integration — All output formats (.tap, .tzx, .sna, .z80)
    │         Full CLI compatibility with zxbc
    │
    🏁  Single static binary: zxbasic for NextPi and embedded platforms
```

Each phase is independently useful — you don't have to wait for the whole thing.
After Phase 2, you can preprocess and assemble entirely in C. After Phase 6,
Python is no longer needed at all. 🎯

## 🤖 Who is doing this?

This port is being developed by **Claude** (Anthropic's AI coding assistant,
model Claude Opus 4.6), directed and supervised by
[@Xalior](https://github.com/Xalior).

Claude is analysing the original Python codebase, designing the C architecture,
writing the implementation, and verifying correctness against the existing test
suite — with every commit pushed in real-time for full transparency.

### Design Decisions

| Aspect | Python Original | C Port |
|--------|----------------|--------|
| Parsing (zxbpp) | PLY lex/yacc | Hand-written recursive-descent |
| Parsing (zxbasm, zxbc) | PLY lex/yacc | flex + bison |
| AST nodes | 50+ classes with inheritance | Tagged union structs |
| Memory | Python GC | Arena allocator |
| Strings | Python str (immutable) | `StrBuf` (growable) |
| Dynamic arrays | Python list | `VEC(T)` macro |
| Hash tables | Python dict | `HashMap` (open addressing) |
| CLI | argparse | [`ya_getopt`](https://github.com/kubo/ya_getopt) (BSD-2-Clause) |
| Path manipulation | `os.path` | [`cwalk`](https://github.com/likle/cwalk) (MIT) |

See **[docs/c-port-plan.md](docs/c-port-plan.md)** for the full implementation plan with detailed breakdown.

## 🔄 Upstream Sync

The `src/` and `tests/functional/` directories are a read-only mirror of the
canonical Python source at [boriel-basic/zxbasic](https://github.com/boriel-basic/zxbasic).
A weekly CI job checks for upstream changes and opens a PR automatically if
anything has moved. You can also sync manually:

```bash
./csrc/scripts/sync-upstream.sh
```

This keeps our Python reference and test suite in lockstep with Boriel's latest,
so the C port is always validated against the real thing. 🎯

## 📜 Original Project

Copyleft (K) 2008, Jose Rodriguez-Rosa (a.k.a. Boriel) <http://www.boriel.com>

All files in this project are covered under the
[AGPLv3 LICENSE](http://www.gnu.org/licenses/agpl.html) except those placed in
directories `library/` and `library-asm`, which are licensed under
[MIT license](https://en.wikipedia.org/wiki/MIT_License) unless otherwise
specified in the files themselves.

- 💜 [Upstream, Original, Amazing, ZX BASIC project](https://github.com/boriel-basic/zxbasic)
- 📖 [ZX BASIC documentation](https://zxbasic.readthedocs.io/en/latest/)
- 💬 [Community forum](https://forum.boriel.com/)
