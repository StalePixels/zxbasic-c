
![Boriel ZX Basic](img/zxbasic_logo.png)

[![license](https://img.shields.io/badge/License-AGPLv3-blue.svg)](./LICENSE.txt)

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
| 2 | Assembler (`zxbasm`) — 62 binary-exact tests | 0/62 | 🔜 Next up |
| 3 | BASIC compiler frontend (lexer + parser + AST) | — | ⏳ Planned |
| 4 | Optimizer + IR generation (AST → Quads) | — | ⏳ Planned |
| 5 | Z80 backend (Quads → Assembly) — 1,175 ASM tests | — | ⏳ Planned |
| 6 | Full integration + all output formats | — | ⏳ Planned |

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

This builds `csrc/build/zxbpp/zxbpp` — the C preprocessor binary.

### Running the Tests

```bash
# Run all 96 preprocessor tests against expected output:
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp
```

### 🐍 Python Ground-Truth Comparison

Want to see for yourself that C matches Python? You'll need Python 3.12+:

```bash
brew install python@3.12   # macOS

# Run both Python and C on every test, diff the outputs:
./csrc/tests/compare_python_c.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp
```

This runs the original Python `zxbpp` and the C port on all 91 test inputs and
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

The rest of the toolchain (`zxbasm`, `zxbc`) still requires Python — for now. 😏

## 🗺️ The Road to NextPi

The big picture: a fully native C compiler toolchain that runs on the
[NextPi](https://www.specnext.com/) — a Raspberry Pi accelerator board for the
ZX Spectrum Next. No Python runtime needed, just a single binary.

Here's how we get there, one step at a time:

```
 Phase 0  ✅  Infrastructure — arena allocator, strings, vectors, hash maps
    │
 Phase 1  ✅  zxbpp — Preprocessor (you are here! 📍)
    │         Can already replace Python's zxbpp in your workflow
    │
 Phase 2  🔜  zxbasm — Z80 Assembler
    │         62 binary-exact tests to pass
    │         After this: zxbpp + zxbasm work without Python
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
| CLI | argparse | `getopt_long` |

See **[plan.md](plan.md)** for the full implementation plan with detailed breakdown.

## 📜 Original Project

Copyleft (K) 2008, Jose Rodriguez-Rosa (a.k.a. Boriel) <http://www.boriel.com>

All files in this project are covered under the
[AGPLv3 LICENSE](http://www.gnu.org/licenses/agpl.html) except those placed in
directories `library/` and `library-asm`, which are licensed under
[MIT license](https://en.wikipedia.org/wiki/MIT_License) unless otherwise
specified in the files themselves.

- 🏠 [Original ZX BASIC project](https://github.com/boriel-basic/zxbasic)
- 📖 [ZX BASIC documentation](https://zxbasic.readthedocs.io/en/latest/)
- 💬 [Community forum](https://forum.boriel.com/)
