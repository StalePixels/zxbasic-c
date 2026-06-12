![Boriel ZX Basic](img/zxbasic_logo.png)

[![license](https://img.shields.io/badge/License-AGPLv3-blue.svg)](./LICENSE.txt)
[![C Build](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml/badge.svg)](https://github.com/StalePixels/zxbasic-c/actions/workflows/c-build.yml)
[![Port status](https://img.shields.io/badge/port-agentically_verified_complete-yellow)](PORTING.md)

ZX BASIC — in C
---------------

A **C implementation of the [Boriel ZX BASIC](https://github.com/boriel-basic/zxbasic)
compiler toolchain** — `zxbc` (compiler), `zxbpp` (preprocessor), and `zxbasm`
(assembler) — built as a **drop-in replacement** for the Python original:
same flags, same inputs, **byte-for-byte identical output**.

ZX BASIC is Jose Rodriguez-Rosa (Boriel)'s BASIC cross-compiler for the
Sinclair ZX Spectrum family. The original (preserved in this repo under
`src/`, with [its README here](README-upstream.md)) requires Python 3.11+.
This port needs no Python at all — three small native binaries with **zero
external dependencies**, suitable for embedded and resource-constrained hosts.
It was built for [NextPi](https://www.specnext.com/) (a Pi Zero inside the
ZX Spectrum Next), where a Python 3.11 runtime is not practical.

## Build

```bash
cmake -S csrc -B csrc/build -DCMAKE_BUILD_TYPE=Release
cmake --build csrc/build -j8
```

This produces `bin/zxbasic-suite` (at the repo root) — a busybox-style
multicall binary — plus `zxbc` / `zxbpp` / `zxbasm` symlinks beside it that
dispatch on their name. (Build state stays under `csrc/build/`; only the
runnable `bin/` lands at the root.) Builds on Linux (x86_64/arm64), macOS (arm64), and Windows
(x86_64); C11, no dependencies beyond a C compiler and CMake.

## Use

Exactly as you would the Python originals. The classic upstream quick-start
works as-is:

```basic
10 CLS
20 PRINT "HELLO WORLD!"
```

```bash
bin/zxbc -f tap --autorun --BASIC hello.bas
# -> hello.tap, ready for your favourite emulator or real hardware
```

All upstream flags, output formats (`.bin`, `.tap`, `.tzx`, `.sna`, `.z80`,
`.asm`, `.ir`), optimization levels (`-O0`–`-O3`), and architectures
(`zx48k`, `zxnext`) are supported. The library search root can be overridden
with the `ZXBASIC_INC_PATH` environment variable.

For the ZX BASIC *language* itself — syntax, library, examples — use the
[upstream documentation](https://zxbasic.readthedocs.io/): this port is
deliberately indistinguishable from the compiler it documents — and if you
find somewhere it's not, please
[open an issue](https://github.com/StalePixels/zxbasic-c/issues)!

## Fidelity, in one paragraph

"Drop-in" is enforced, not aspired to: every commit is gated against the
Python original running live as an oracle — the full upstream functional
corpus end-to-end at every optimization level, a hand-authored probe series
for codepaths the corpus is silent on, CLI-flag parity, and a corpus of real
released ZX BASIC games compiled through both and byte-compared. If you have
an example that fails,
[let us know](https://github.com/StalePixels/zxbasic-c/issues) so we can add
it! The full porting story, how we meter it, and the current verification
status live in [PORTING.md](PORTING.md).

## License & credit

ZX BASIC is Copyleft (K) Jose Rodriguez-Rosa (Boriel) — this port exists
because the original is excellent. Licensed [AGPLv3](LICENSE.txt), same as
upstream; the same library-license carve-outs described in the
[upstream README](README-upstream.md) apply. As with upstream, programs you
compile with it are yours, closed-source and commercial included.
