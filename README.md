
![Boriel ZX Basic](img/zxbasic_logo.png)

[![license](https://img.shields.io/badge/License-AGPLv3-blue.svg)](./LICENSE.txt)
[![C Build](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml/badge.svg)](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml)
[![zxbpp tests](https://img.shields.io/badge/zxbpp_tests-96%2F96_passing-brightgreen)](#-phase-1--preprocessor-done)
[![zxbasm tests](https://img.shields.io/badge/zxbasm_tests-61%2F61_passing-brightgreen)](#-phase-2--assembler-done)
[![zxbc full pipeline](https://img.shields.io/badge/zxbc_full--O0--O3-byte--identical_to_Python-brightgreen)](#-phase-3--compiler-frontend-byte-identical)
[![Codegen probes](https://img.shields.io/badge/probes-129_GREEN_0_RED-brightgreen)](#probe-enumeration-meter)
[![C unit tests](https://img.shields.io/badge/C_unit_tests-132_passing-blue)](#c-unit-test-suite)
[![Port status](https://img.shields.io/badge/port-agentically_verified_complete-yellow)](#-port-complete--2026-05-28-agentically-verified-not-yet-user-verified)

ZX BASIC — C Port 🚀
---------------------

A **C language port** of the [Boriel ZX BASIC compiler](https://github.com/boriel-basic/zxbasic),
originally written in Python by Jose Rodriguez-Rosa (a.k.a. Boriel).

## 🏁 Port Complete — 2026-05-28 *(agentically verified, not yet user-verified)*

> ⚠️ **This is an agent's self-declaration of completion**, supported by the
> automated test meters. It has **not yet been independently verified or
> signed off by [@Xalior](https://github.com/Xalior) (the human
> maintainer)**. The meters are reproducible — anyone can run `make test`
> and `make test-slow` and check. Human cold-read + real-world build
> sign-off is still pending.

Per the automated gates, the toolchain — `zxbpp` (preprocessor), `zxbasm`
(assembler), `zxbc` (compiler) — is a **byte-for-byte drop-in replacement**
for the Python original across every measured surface: the full
`tests/functional/` corpus at every optimization level, all 132 internal-API
unit tests, all 129 hand-authored probe fixtures, and the gated 3-stage codegen
pipeline on both `zx48k` and `zxnext` archs. CI green on Linux x86_64 /
Linux arm64 / macOS arm64 / Windows x86_64.

Single-command verification:

```bash
make test       # ~5 min fast tier — routine green-light gate
make test-slow  # ~18 min deep tier — full byte-for-byte equivalence sweep
```

## 🎯 What was this?

An **agentic porting experiment** — a test of whether an AI coding assistant
could systematically port a non-trivial compiler (~38,500 lines of Python) to
C, producing a drop-in replacement with byte-for-byte identical output. The
answer turned out to be yes, with the discipline scaffolding documented in the
close-out doc above.

The toolchain was validated stage by stage against the original's comprehensive
test suite of 1,285+ functional tests, plus an additional 129-fixture probe
series authored alongside the port to catch codepaths the inherited corpus is
silent on.

The practical end-goal: a C implementation of the compiler suitable for
**embedding on [NextPi](https://www.specnext.com/)** and similar
resource-constrained platforms. The NextPi ships with a lightweight Python 2
install, but ZX BASIC requires Python 3.11+ — far too heavy for the hardware.
Native C binaries sidestep the problem entirely.

## 📊 Current Status

| Phase | Component | Tests | Status |
|-------|-----------|-------|--------|
| 0 | Infrastructure (arena, strbuf, vec, hashmap, CMake) | — | ✅ Complete |
| 1 | **Preprocessor (`zxbpp`)** | **96/96** 🎉 | ✅ Complete |
| 2 | **Assembler (`zxbasm`)** | **61/61** 🎉 | ✅ Complete |
| 3 | **Compiler frontend (`zxbc`)** | **1033/1033** parse-only PASS / **0 false-positives** | ✅ Byte-identical except 3 known upstream Python bugs |
| 4 | **Optimizer + IR generation (AST → Quads)** | byte-identical -O1/-O2/-O3 to Python | ✅ Complete |
| 5 | **Z80 backend (Quads → Assembly + peephole)** | zx48k 895/886/886 stages GREEN; zxnext 197/197/197 GREEN | ✅ Complete |
| 6 | Full integration + all output formats (.tap/.tzx/.sna/.z80) | exercised by stage validation | ✅ Complete |
| 7 | Full-equivalence umbrella + `make test` / `make test-slow` | FULL-EQUAL 888 / 0 DIFF; 129 probe GREEN / 0 RED | ✅ **agentically verified complete** (pending user sign-off) |

### 🔬 Phase 3 — Compiler Frontend: Byte-Identical

The `zxbc` frontend has reached **byte-for-byte parity with Python** across the
entire `tests/functional/arch/zx48k` corpus (1,036 fixtures) at every
optimization level (`-O0`/`-O1`/`-O2`/`-O3`), with the sole exception of three
fixtures (`chr`, `chr1`, `const6`) where Python's `-O2`/`-O3` optimizer is
known-broken in the pinned upstream commit (documented in the upstream
CHANGELOG; C compiles them correctly).

**Parse meter** (exit-code + cached-Python-baseline stderr comparison):
- ✅ **1033/1033 PASS** — C and Python produce byte-identical parse-only output
- ✅ **0 false-positives** — C never errors on a file that Python accepts
- ✅ **0 false-negatives** — C never silently accepts a file Python rejects
- ✅ **0 stderr-mismatch residuals** — diagnostic text fully aligned with Python

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
129 hand-authored fixtures (`csrc/tests/codegen_probes/`) that deliberately
drive codepaths the inherited corpus is silent on. The probe runner compares
the FULL contract per fixture (exit, stderr, Stage-1 ASM, end-to-end binary)
against the Python oracle. **129 probes GREEN, 0 RED** across 10
categories (typecast, warnings, errors, arithmetic, strings, arrays, controlflow,
switches, preprocessor, zxbasm). Wave-4 closed three more legacy
zxbasm Error-FAIL fixtures via additive probes: `ldix2` (reject the
malformed `LP IX LP ...` shape so `Unexpected token ')' [RP]` is
emitted), `preprocerr2` (PLY p_error(None) footer for NEWLINE-only
lex streams), and the immediate-next-line cascade-suppression
regression on `no_zxnext` (per-statement decl tracking so a label-
decl-then-error line no longer poisons the next line's token render).
Wave-3 closed the final three wave-1 zxbasm divergences. Wave-2 closed
div-by-zero, [W200] truncation tag, unknown-char token-render, and
bad-operand uppercase casing. This is the enumeration-completeness check —
corpus-pass alone doesn't prove the port has every Python check; the probe
meter does. Every new oversight surfaced from real-world compilation gets
a RED probe first, GREEN fix second.

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
| **Total** | **132** | |

### 🔬 Phase 2 — Assembler: Done!

The `zxbasm` C binary is an **agentically-verified drop-in replacement** for the Python original (pending user sign-off — see top banner):

- ✅ **61/61 tests passing** — zero failures, byte-for-byte identical binary output
- ✅ **61/61 Python comparison** — confirmed by automated side-by-side harness
- ✅ Full Z80 instruction set (827 opcodes) including ZX Next extensions
- ✅ Two-pass assembly: labels, forward references, expressions, temporaries
- ✅ PROC/ENDP scoping, LOCAL labels, PUSH/POP NAMESPACE
- ✅ `#init` directive, EQU/DEFL, ORG, ALIGN, INCBIN
- ✅ Hand-written recursive-descent parser (~2,200 lines of C)
- ✅ Preprocessor integration (reuses the C zxbpp binary)

### 🔬 Phase 1 — Preprocessor: Done!

The `zxbpp` C binary is an **agentically-verified drop-in replacement** for the Python original (pending user sign-off — see top banner):

- ✅ **96/96 tests passing** (91 normal + 5 error tests) — zero skipped
- ✅ **91/91 outputs identical to Python** — confirmed by automated side-by-side harness
- ✅ All preprocessor features: `#define`, `#include`, `#ifdef`/`#if`, macro expansion, token pasting, stringizing, block comments, ASM mode, line continuation, `#pragma`/`#require`/`#init`/`#error`/`#warning`, architecture-specific includes
- ✅ Hand-written recursive-descent parser (~3,000 lines of C)

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

The umbrella entry point is the top-level `Makefile`:

```bash
# Fast tier — the routine green-light gate (~5 min on a recent workstation).
# zxbpp + zxbasm + zxbc parse + zxbc codegen + 129-fixture probe series +
# the C unit tests. Exits non-zero on any regression.
make test

# Deep tier — `make test` PLUS the byte-for-byte equivalence meters:
# full corpus, 3-stage gated pipeline (zx48k + zxnext), -O matrix sweep
# (zx48k + zxnext). Used pre-bank / nightly / pre-release. Wall-clock
# dominated by the gated stage harness.
make test-slow
```

Individual harnesses can still be driven directly:

```bash
# Preprocessor tests (91 success + 5 error):
./csrc/tests/run_zxbpp_tests.sh ./csrc/build/zxbpp/zxbpp tests/functional/zxbpp

# Assembler tests (60 success + 32 error, binary-exact):
./csrc/tests/run_zxbasm_tests.sh ./csrc/build/zxbasm/zxbasm tests/functional/asm

# C unit tests via CTest:
cd csrc/build && ctest --output-on-failure
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
series (`csrc/tests/codegen_probes/`, **129 fixtures across 10 categories** —
arithmetic, arrays, controlflow, errors, preprocessor, strings, switches,
typecast, warnings, zxbasm) covers codepaths the inherited corpus doesn't
reach — **129/129 GREEN**, hand-authored to enforce no silent drift on subtle
semantics (typecast cross-products, loop-stack EXIT/CONTINUE checks,
`@`-address-of in constant contexts, class mismatches with proper "a VAR"/"an
ARRAY" article handling, etc.).

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
    │         1033/1033 parse-only PASS, 0 false-positives, 129 probes GREEN
    │
 Phase 4  ✅  Optimizer + IR — byte-identical to Python at -O1/-O2/-O3
    │
 Phase 5  ✅  Z80 Backend — Quads → Assembly + peephole
    │         zx48k 895/886/886 + zxnext 197/197/197 stages ALL GREEN
    │
 Phase 6  ✅  Integration — All output formats (.tap, .tzx, .sna, .z80, .bin, .ir)
    │         Full CLI compatibility (every upstream flag accepted)
    │
 Phase 7  ✅  Full-equivalence umbrella — `make test` / `make test-slow`
    │         FULL-EQUAL 888 / 0 DIFF; 129 probes GREEN; 132 unit tests GREEN
    │
    🏁  PORT AGENTICALLY VERIFIED COMPLETE — every automated meter green,
        prose audit grounded, pending user sign-off. Native C binaries,
        drop-in for Python, suitable for NextPi and any embedded platform —
        no Python needed.
```

Each phase was independently useful — you didn't have to wait for the whole
thing. After Phase 2, you can preprocess and assemble entirely in C. After
Phase 7, Python is no longer needed at all. 🎯

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
