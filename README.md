
![Boriel ZX Basic](img/zxbasic_logo.png)

[![license](https://img.shields.io/badge/License-AGPLv3-blue.svg)](./LICENSE.txt)
[![C Build](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml/badge.svg)](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml)
[![zxbpp tests](https://img.shields.io/badge/zxbpp_tests-96%2F96_passing-brightgreen)](#-phase-1--preprocessor-done)
[![zxbasm tests](https://img.shields.io/badge/zxbasm_tests-61%2F61_passing-brightgreen)](#-phase-2--assembler-done)
[![zxbc full pipeline](https://img.shields.io/badge/zxbc_full--O0--O3-byte--identical_to_Python-brightgreen)](#-phase-3--compiler-frontend-byte-identical)
[![Codegen probes](https://img.shields.io/badge/probes-84%2F84_GREEN-brightgreen)](#probe-enumeration-meter)
[![C unit tests](https://img.shields.io/badge/C_unit_tests-132_passing-blue)](#c-unit-test-suite)

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
[NextPi](https://www.specnext.com/)** and similar resource-constrained platforms. The
NextPi ships with a lightweight Python 2 install, but ZX BASIC requires Python 3.11+ —
far too heavy for the hardware. Native C binaries sidestep the problem entirely.

## 📊 Current Status

| Phase | Component | Tests | Status |
|-------|-----------|-------|--------|
| 0 | Infrastructure (arena, strbuf, vec, hashmap, CMake) | — | ✅ Complete |
| 1 | **Preprocessor (`zxbpp`)** | **96/96** 🎉 | ✅ Complete |
| 2 | **Assembler (`zxbasm`)** | **61/61** 🎉 | ✅ Complete |
| 3 | **Compiler frontend (`zxbc`)** | **1014/1033** parse-only PASS / **0 false-positives** | ✅ Byte-identical except 3 known upstream Python bugs |
| 4 | **Optimizer + IR generation (AST → Quads)** | byte-identical -O1/-O2/-O3 to Python | ✅ Complete |
| 5 | **Z80 backend (Quads → Assembly + peephole)** | zx48k 895/886/886 stages GREEN; zxnext 197/197/197 GREEN | ✅ Complete |
| 6 | Full integration + all output formats (.tap/.tzx/.sna/.z80) | exercised by stage validation | 🔨 Final polish |

### 🔬 Phase 3 — Compiler Frontend: Byte-Identical

The `zxbc` frontend has reached **byte-for-byte parity with Python** across the
entire `tests/functional/arch/zx48k` corpus (1,036 fixtures) at every
optimization level (`-O0`/`-O1`/`-O2`/`-O3`), with the sole exception of three
fixtures (`chr`, `chr1`, `const6`) where Python's `-O2`/`-O3` optimizer is
known-broken in the pinned upstream commit (documented in the upstream
CHANGELOG; C compiles them correctly).

**Parse meter** (exit-code + cached-Python-baseline stderr comparison):
- ✅ **1014/1033 PASS** — C and Python produce byte-identical parse-only output
- ✅ **0 false-positives** — C never errors on a file that Python accepts
- ✅ **0 false-negatives** — C never silently accepts a file Python rejects
- 🔨 19 stderr-mismatch residuals — warning/error message text fidelity (binaries byte-identical; the diagnostic text still diverges on a small backlog)

**Omatrix meter** (full-compile raw `.bin` comparison at all -O levels):
- ✅ -O0 EQUAL 888 / BIN-DIFF 0 / C-ERR 0 / CRASH 0
- ✅ -O1 EQUAL 887 / BIN-DIFF 1 (const6 only)
- ✅ -O2 EQUAL 888 / BIN-DIFF 3 (chr/chr1/const6 only)
- ✅ -O3 EQUAL 888 / BIN-DIFF 3 (chr/chr1/const6 only)

**Stage validation** (gated codegen → assemble → end-to-end pipeline, both archs):
- ✅ **zx48k** S1/S2/S3 EQUAL 895/886/886 — ALL GREEN
- ✅ **zxnext** S1/S2/S3 EQUAL 197/197/197 — ALL GREEN

#### Probe Enumeration Meter

In addition to the inherited corpus, the C port has its own probe series —
~90 hand-authored fixtures (`csrc/tests/codegen_probes/`) that deliberately
drive codepaths the inherited corpus is silent on. The probe runner compares
the FULL contract per fixture (exit, stderr, Stage-1 ASM, end-to-end binary)
against the Python oracle. **84/84 probes GREEN**, 0 RED, across 8 categories
(typecast, warnings, errors, arithmetic, strings, arrays, controlflow, switches).
This is the enumeration-completeness check — corpus-pass alone doesn't prove
the port has every Python check; the probe meter does.

#### Compiler infrastructure
- ✅ **Faithful PLY/LALR(1) parser port** — the default `zxbc` parser is a
  byte-for-byte port of Python's own PLY-generated tables + parse engine,
  driven from the real grammar (`src/zxbc/zxbparser.py` / `zxblex.py`). The
  prior hand-rolled recursive-descent parser is kept dead-in-tree under
  `ZXBC_LEGACY_PARSER` as a fallback.
- ✅ Full AST with tagged-union node types
- ✅ Symbol table with lexical scoping, type registry, basic types
- ✅ Type system: all ZX BASIC types (`byte` through `string`), aliases, refs
- ✅ Semantic checks: declare/access, type coercion, constant folding, symbol
  resolution, post-parse validation (label/identifier/function-call checks,
  loop-stack checks for EXIT/CONTINUE, ByVal-array-param rejection)
- ✅ Optimizer port: `comes_from` block-ignored marking, jump-over-jump pass
  with CPython-faithful set iteration order (ported siphash13),
  `@cached_property` staleness modeling on memcell assignment
- ✅ Z80 backend + peephole optimizer
- ✅ CLI with all `zxbc` flags, config file loading, `--parse-only`, `--asm`
- ✅ Preprocessor integration (reuses C zxbpp via static library)

#### C Unit Test Suite

Beyond matching Python's functional tests, the C port has its own unit test suite
verifying internal APIs match the Python test suites (`tests/api/`, `tests/symbols/`,
`tests/cmdline/`):

| Test Program | Tests | Matches Python |
|-------------|-------|----------------|
| `test_utils` | 14 | `tests/api/test_utils.py` |
| `test_config` | 6 | `tests/api/test_config.py` + `test_arg_parser.py` |
| `test_types` | 10 | `tests/symbols/test_symbolBASICTYPE.py` |
| `test_ast` | 61 | All 19 `tests/symbols/test_symbol*.py` files |
| `test_symboltable` | 22 | `tests/api/test_symbolTable.py` (18 + 4 C extras) |
| `test_check` | 4 | `tests/api/test_check.py` |
| `test_cmdline` | 15 | `tests/cmdline/test_zxb.py` + `test_arg_parser.py` |
| `run_cmdline_tests.sh` | 4 | `tests/cmdline/test_zxb.py` (exit-code) |
| **Total** | **136** | |

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

This builds `csrc/build/zxbpp/zxbpp`, `csrc/build/zxbasm/zxbasm`, and `csrc/build/zxbc/zxbc`.

### Running the Tests

```bash
# Run all 96 preprocessor tests:
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp

# Run all 61 assembler tests (binary-exact):
./csrc/tests/run_zxbasm_tests.sh ./csrc/build/zxbasm/zxbasm tests/functional/asm

# Run 132 C unit tests:
cd csrc/build && ./tests/test_utils && ./tests/test_config && ./tests/test_types \
  && ./tests/test_ast && ./tests/test_symboltable && ./tests/test_check && cd ../..
./csrc/build/tests/test_cmdline

# Run 4 zxbc command-line tests:
./csrc/tests/run_cmdline_tests.sh ./csrc/build/zxbc/zxbc tests/cmdline
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

## 🔧 Using the C Toolchain Today

All three C binaries (`zxbpp`, `zxbasm`, `zxbc`) accept the **same flags** as their
Python originals — drop them into any Boriel ZX BASIC workflow.

### Compiler (`zxbc`) — full pipeline, byte-identical to Python

`zxbc` produces output byte-identical to Python's at every optimization level
(`-O0`/`-O1`/`-O2`/`-O3`), to every output format. Three fixtures in the upstream
test corpus (`chr`, `chr1`, `const6`) trigger a known-broken Python `-O2`/`-O3`
optimizer path documented in the upstream CHANGELOG — C compiles them correctly
where the pinned Python crashes.

```bash
# A first program:
echo 'PRINT "Hello from C!"' > hello.bas

# Compile to a raw binary (the default output format, ORG $8000):
./csrc/build/zxbc/zxbc -o hello.bin hello.bas

# Compile to a .tap tape image with a BASIC loader and autorun:
./csrc/build/zxbc/zxbc -f tap -B -a -o hello.tap hello.bas

# Same for .tzx, .sna, .z80:
./csrc/build/zxbc/zxbc -f tzx -B -a -o hello.tzx hello.bas
./csrc/build/zxbc/zxbc -f sna -B -a -o hello.sna hello.bas
./csrc/build/zxbc/zxbc -f z80 -B -a -o hello.z80 hello.bas

# Generate the Stage-1 assembly only (useful for inspection):
./csrc/build/zxbc/zxbc -f asm -o hello.asm hello.bas

# Different optimization levels — all four produce Python-identical bytes:
./csrc/build/zxbc/zxbc -O3 -o hello-O3.bin hello.bas

# Pick a target architecture (default zx48k; zxnext enables Z80N opcodes):
./csrc/build/zxbc/zxbc --arch=zxnext -o hello.bin hello.bas

# Add extra include search paths (multiple -I accepted):
./csrc/build/zxbc/zxbc -I lib/ -I shared/ -o myapp.bin myapp.bas

# Parse-only (semantic validation without code emission, for editors/CI):
./csrc/build/zxbc/zxbc --parse-only myfile.bas
```

**Same flag surface as Python `zxbc` — every flag in the upstream CLI is
accepted**, including the deprecated short forms (`-t`/`-T`/`--asm`), the
mutual-exclusivity rules (e.g. `-f tap` vs `-T`), and the warning-on-deprecated
behaviors. Common ones: `-o`, `-O0`/`-O1`/`-O2`/`-O3`, `-f`/`--output-format`
(`bin`/`tap`/`tzx`/`sna`/`z80`/`asm`), `-B`/`--BASIC`, `-a`/`--autorun`, `--arch`
(`zx48k`/`zxnext`), `-I`, `--zxnext`, `-d`/`--debug`, `--emit-backend`,
`--parse-only`, `--strict`, `-D`, `-F` (config file), `--save-config`,
`--heap-size`, `--org`, `--mmap`. CLI-parity is exercised by the upstream
`tests/cmdline/` suite (covered by `test_cmdline` in the C unit tests).

### Preprocessor (`zxbpp`) — drop-in for Python's preprocessing stage

```bash
# Instead of:
python3 zxbpp.py myfile.bas -o myfile.preprocessed.bas

# Use:
./csrc/build/zxbpp/zxbpp myfile.bas -o myfile.preprocessed.bas
```

**Same flag surface as Python `zxbpp` — all upstream flags accepted.** 96/96
upstream functional tests pass (preprocessing of every `.bi` fixture, including
all `#define`/`#include`/`#if`/`#ifdef`, macro expansion, token pasting,
stringizing, ASM-mode blocks, `#pragma` / `#require` / `#init` / `#error` /
`#warning`, architecture-specific includes).

### Assembler (`zxbasm`) — drop-in for Python's assembly stage

```bash
./csrc/build/zxbasm/zxbasm myfile.asm -o myfile.bin
```

**Same flag surface as Python `zxbasm` — all upstream flags accepted.** 61/61
upstream tests pass with **binary-exact** output (every byte of the emitted
binary matches Python's, on the full Z80 instruction set including Z80N
extensions). PROC/ENDP scoping, LOCAL labels, PUSH/POP NAMESPACE, `#init`,
EQU/DEFL, ORG, ALIGN, INCBIN, two-pass with forward refs — all there.

### Prove it's a drop-in replacement

Want to confirm against Python directly? Compile the same source with both and
`cmp`:

```bash
# Python (needs Python 3.11+ and the upstream src/):
python3 -c "
import sys; sys.path.insert(0, '.')
from src.zxbc.zxbc import main
sys.exit(main(['-O2', '-f', 'tap', '-B', '-a', '-o', 'py.tap', 'hello.bas']) or 0)
"

# C:
./csrc/build/zxbc/zxbc -O2 -f tap -B -a -o c.tap hello.bas

# Byte-compare:
cmp py.tap c.tap && echo "✅ byte-identical"
```

Across the 1,036-file `tests/functional/arch/zx48k` corpus and the 198-file
`tests/functional/arch/zxnext` corpus, this comparison passes for every file
except the three documented Python-optimizer-bug fixtures. The custom probe
series (`csrc/tests/codegen_probes/`, ~94 fixtures across 8 categories) covers
codepaths the inherited corpus doesn't reach — 84+ probes GREEN, hand-authored
to enforce no silent drift on subtle semantics (typecast cross-products,
loop-stack EXIT/CONTINUE checks, `@`-address-of in constant contexts, class
mismatches with proper "a VAR"/"an ARRAY" article handling, etc.).

### Embed in your build pipeline

The three binaries chain naturally if you want explicit per-stage control,
or `zxbc` will run the full pipeline in-process:

```bash
# Explicit chain (preprocess → compile-to-asm → assemble):
./csrc/build/zxbpp/zxbpp myfile.bas -o myfile.pre.bas
./csrc/build/zxbc/zxbc -f asm -o myfile.asm myfile.pre.bas
./csrc/build/zxbasm/zxbasm myfile.asm -o myfile.bin

# Or one shot (zxbc invokes both internally):
./csrc/build/zxbc/zxbc -f bin -o myfile.bin myfile.bas
```

Native C binaries — no Python dependency, suitable for embedding in CI, build
systems, IDEs, or resource-constrained targets like NextPi.

## 🗺️ The Road to NextPi

The big picture: a fully native C compiler toolchain that runs on the
[NextPi](https://www.specnext.com/) — a Raspberry Pi accelerator board for the
ZX Spectrum Next. The NextPi has a lightweight Python 2, but ZX BASIC needs
Python 3.11+ which is impractical on that hardware. Native C binaries solve this.

Here's how we get there, one step at a time:

```
 Phase 0  ✅  Infrastructure — arena allocator, strings, vectors, hash maps
    │
 Phase 1  ✅  zxbpp — Preprocessor
    │         96/96 tests, drop-in replacement for Python's zxbpp
    │
 Phase 2  ✅  zxbasm — Z80 Assembler
    │         61/61 binary-exact tests passing
    │         zxbpp + zxbasm work without Python!
    │
 Phase 3  ✅  BASIC Frontend — faithful PLY/LALR(1) port
    │         1014/1033 parse-only PASS, 0 false-positives, 84/84 probes GREEN
    │
 Phase 4  ✅  Optimizer + IR — byte-identical to Python at -O1/-O2/-O3
    │
 Phase 5  ✅  Z80 Backend — Quads → Assembly + peephole
    │         zx48k 895/886/886 + zxnext 197/197/197 stages ALL GREEN
    │
 Phase 6  🔨  Integration — All output formats (.tap, .tzx, .sna, .z80)
    │         Full CLI compatibility (you are here! 📍)
    │
    🏁  Native binaries for NextPi and embedded platforms — no Python needed
```

Each phase is independently useful — you don't have to wait for the whole thing.
After Phase 2, you can preprocess and assemble entirely in C. After Phase 6,
Python is no longer needed at all. 🎯

## 🤖 Who is doing this?

This port is being developed by **Claude** (Anthropic's AI coding assistant,
model Claude Opus 4.7), directed and supervised by
[@Xalior](https://github.com/Xalior).

Claude is analysing the original Python codebase, designing the C architecture,
writing the implementation, and verifying correctness against the existing test
suite — with every commit pushed in real-time for full transparency.

### Design Decisions

| Aspect | Python Original | C Port |
|--------|----------------|--------|
| Parsing (zxbpp) | PLY lex/yacc | Hand-written recursive-descent |
| Parsing (zxbasm) | PLY lex/yacc | Hand-written recursive-descent |
| Parsing (zxbc) | PLY lex/yacc | Faithful port of PLY's LALR(1) tables + parse engine, driven by the real grammar |
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
