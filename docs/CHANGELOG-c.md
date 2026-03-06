# Changelog — ZX BASIC C Port

All notable changes to the C port. Versioning tracks upstream
[boriel-basic/zxbasic](https://github.com/boriel-basic/zxbasic) with a `+cN` suffix.

## [1.18.7+c1] — 2026-03-06

First release 🎉 — Phase 0 (Infrastructure) + Phase 1 (Preprocessor).

### Added

- **zxbpp** — Complete C port of the ZX BASIC preprocessor
  - Hand-written recursive-descent parser (~1,600 lines of C)
  - Drop-in CLI replacement: same flags as Python `zxbpp`
  - `--version` flag: `zxbpp 1.18.7+c1 (C port)`
  - All preprocessor features:
    - `#define` / `#undef` (object-like and function-like macros)
    - Nested macro expansion with argument pre-expansion
    - Token pasting (`##`) and stringizing (`#`) operators
    - `#include` with path search, `#include once`, `#pragma once`
    - `#include` with `[arch:XXX]` architecture modifier
    - `#include` with macro expansion in filename
    - `#ifdef` / `#ifndef` / `#else` / `#endif` conditional compilation
    - `#if` expression evaluation (==, !=, <>, <, <=, >, >=, &&, ||, parens)
    - `#line` directive emission matching Python output model
    - `#pragma`, `#require`, `#init`, `#error`, `#warning` directives
    - BASIC comment stripping (`'` and `REM`)
    - ASM mode (`asm`/`end asm`) with `;` comment char
    - Block comments (`/' ... '/`)
    - Line continuation: `\` (for `#define`) and `_` (BASIC)
    - Builtin macros: `__FILE__`, `__LINE__`, `__BASE_FILE__`, `__ABS_FILE__`
  - Error handling matches Python exactly (suppress output on errors)
- **Common utilities** — `csrc/common/`
  - Arena memory allocator (`arena.h`)
  - Growable string buffer (`strbuf.h`)
  - Type-safe dynamic arrays (`vec.h`)
  - String-keyed hash map (`hashmap.h`)
- **Build system** — CMake with version from `csrc/VERSION`
- **Test harnesses** — `csrc/tests/`
  - `run_zxbpp_tests.sh` — standalone test runner (96/96 passing)
  - `compare_python_c.sh` — Python ground-truth comparison (91/91 identical)
- **CI** — GitHub Actions
  - Build on Linux x86_64 + macOS ARM64
  - Run all tests + Python ground-truth comparison
  - Release workflow (tag `v*` for binary downloads)
  - Weekly upstream sync auto-PR from boriel-basic/zxbasic
- **Upstream sync** — `csrc/scripts/sync-upstream.sh`

### Upstream

Synced to [boriel-basic/zxbasic](https://github.com/boriel-basic/zxbasic)
`main` @ `9c0693f8` (v1.18.7).

---

_Format: [Keep a Changelog](https://keepachangelog.com/). Dates are ISO 8601._
