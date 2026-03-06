
![Boriel ZX Basic](img/zxbasic_logo.png)

[![license](https://img.shields.io/badge/License-AGPLv3-blue.svg)](./LICENSE.txt)

ZX BASIC — C Port
------------------

This is a **C language port** of the [Boriel ZX BASIC compiler](https://github.com/boriel-basic/zxbasic),
originally written in Python by Jose Rodriguez-Rosa (a.k.a. Boriel).

### What is this?

This is an **agentic porting experiment** — a test of whether an AI coding assistant can
systematically port a non-trivial compiler (~38,500 lines of Python) to C, producing a
drop-in replacement with **byte-for-byte identical output**.

The toolchain being ported — `zxbc` (compiler), `zxbasm` (assembler), and `zxbpp`
(preprocessor) — is validated stage by stage against the original's comprehensive test
suite of 1,285+ functional tests.

The goal is not to produce a perfect piece of art software. It is to test the viability
of AI-driven porting at this scale, with a practical end-goal: a C implementation of
the compiler suitable for **embedding on [NextPi](https://www.specnext.com/)** and
similar resource-constrained platforms where a Python runtime is undesirable.

### Who is doing this?

This port is being developed by **Claude** (Anthropic's AI coding assistant, model Claude Opus 4.6),
directed and supervised by [@Xalior](https://github.com/Xalior). Claude is analysing the
original Python codebase, designing the C architecture, writing the implementation, and
verifying correctness against the existing test suite.

### Goals

- **Byte-for-byte identical output** — given the same input and flags, the C compiler
  must produce exactly the same `.asm`, `.bin`, `.tap`, `.tzx`, `.sna`, and `.z80` files
  as the Python original
- **Drop-in CLI replacement** — same command-line flags and options as the original
  `zxbc`, `zxbasm`, and `zxbpp`
- **Verified by existing tests** — the original project's 1,285 BASIC functional tests,
  62 binary-exact tests, and 91 preprocessor tests serve as the acceptance criteria
- **No new features** — this is a faithful port, not a rewrite

### Porting Strategy

The port follows a bottom-up, phase-by-phase approach:

| Phase | Component | Status |
|-------|-----------|--------|
| 0 | C project infrastructure, common utilities, CLI parsing | Planned |
| 1 | Preprocessor (`zxbpp`) — 91 test cases | Planned |
| 2 | Assembler (`zxbasm`) — 62 binary-exact test cases | Planned |
| 3 | BASIC compiler frontend (lexer + parser + AST) | Planned |
| 4 | Optimizer + IR generation (AST to Quads) | Planned |
| 5 | Z80 backend (Quads to Assembly) — 1,175 ASM test cases | Planned |
| 6 | Full integration + all output formats | Planned |

### Original Project

Copyleft (K) 2008, Jose Rodriguez-Rosa (a.k.a. Boriel) <http://www.boriel.com>

All files in this project are covered under the [AGPLv3 LICENSE](http://www.gnu.org/licenses/agpl.html)
except those placed in directories `library/` and `library-asm`.
Those are licensed under [MIT license](https://en.wikipedia.org/wiki/MIT_License) unless otherwise
specified in the files themselves.

- [Original ZX BASIC project](https://github.com/boriel-basic/zxbasic)
- [ZX BASIC documentation](https://zxbasic.readthedocs.io/en/latest/)
- [Community forum](https://forum.boriel.com/)
