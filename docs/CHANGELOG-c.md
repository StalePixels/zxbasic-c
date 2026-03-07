# Changelog — ZX BASIC C Port

All notable changes to the C port. Versioning tracks upstream
[boriel-basic/zxbasic](https://github.com/boriel-basic/zxbasic) with a `+cN` suffix.

## [1.18.7+c3] — 2026-03-07

Phase 3 — BASIC Compiler Frontend (`zxbc`).

### Added

- **zxbc** — BASIC compiler frontend (parse-only mode)
  - Hand-written recursive-descent lexer + parser
  - Drop-in CLI replacement: same flags as Python `zxbc`
  - Full AST with 30 tagged-union node kinds matching Python's 50+ Symbol classes
  - Symbol table with lexical scoping and scope chain lookup
  - Type system: all ZX BASIC types (`byte` through `string`), aliases, refs, type registry
  - `CompilerOptions` struct with `cmdline_set` bitmask for config-file override semantics
  - Config file loading (`-F`), `--parse-only`, `--org` hex parsing
  - Preprocessor integration via static library (zxbpp_lib)
  - **914/1036 matching Python** (88%) — exit-code parity with Python `--parse-only`
  - All 1036 `.bas` files parse syntactically; 122 need semantic analysis to match Python's error detection
  - Semantic infrastructure: type coercion (`make_typecast`, `make_binary_node`), constant folding, symbol resolution (`access_var`, `access_call`)
- **Symbol table API** — `csrc/zxbc/compiler.c`
  - `symboltable_declare_variable()` — type refs, deprecated suffix stripping/validation
  - `symboltable_declare_param()` — SCOPE_parameter, duplicate detection with line numbers
  - `symboltable_declare_array()` — TYPEREF + BOUNDLIST validation
  - `symboltable_check_is_declared()` / `check_is_undeclared()` — scope-aware lookup
  - `is_temporary_value()` — matches Python's `api/check.py`
- **`ast_number()`** — NUMBER node creation with auto type inference from value
- **C unit test suite** — 132 tests across 7 programs + 4 shell tests
  - `test_utils` (14) — matches `tests/api/test_utils.py`
  - `test_config` (6) — matches `tests/api/test_config.py` + `test_arg_parser.py`
  - `test_types` (10) — matches `tests/symbols/test_symbolBASICTYPE.py`
  - `test_ast` (61) — matches all 19 `tests/symbols/test_symbol*.py` files
  - `test_symboltable` (22) — matches `tests/api/test_symbolTable.py` (18 + 4 extras)
  - `test_check` (4) — matches `tests/api/test_check.py`
  - `test_cmdline` (15) — matches `tests/cmdline/test_zxb.py` + `test_arg_parser.py`
  - `run_cmdline_tests.sh` (4) — zxbc exit-code tests
- **CI** — added all unit tests + cmdline tests to workflow (Unix + Windows)

### Fixed

- **ya_getopt re-entrancy** — reset static `start`/`end` variables when `ya_optind=0`
- **ast_tag_name buffer overflow** — missing "ARRAYINIT" entry (caught by ASan)
- **TYPEREF basic_type resolution** — follow `final_type` chain via `resolve_basic_type()`

## [1.18.7+c2] — 2026-03-07 (internal)

Phase 2 — Z80 Assembler (`zxbasm`).

### Added

- **zxbasm** — Complete C port of the Z80 assembler
  - Hand-written recursive-descent parser (~1,750 lines of C)
  - Drop-in CLI replacement: same flags as Python `zxbasm`
  - Full Z80 instruction set: 827 opcodes via static lookup table
  - ZX Next extended opcodes (LDIX, NEXTREG, MUL, BSLA, etc.)
  - Two-pass assembly with forward reference resolution
  - Temporary labels (nB/nF) with namespace-aware resolution
  - PROC/ENDP scoping with LOCAL labels
  - PUSH/POP NAMESPACE directives
  - `#init` directive (emits CALL+JP init trampoline)
  - EQU, DEFL, ORG, ALIGN, DS/DEFS, DB/DEFB, DW/DEFW
  - INCBIN (binary file inclusion)
  - Expression evaluation: arithmetic, bitwise, comparisons
  - Preprocessor integration (reuses C zxbpp binary)
  - UTF-8 BOM handling
  - Raw binary (.bin) output format
  - **61/61 tests passing** — byte-for-byte identical to Python
- **Test harnesses** — `csrc/tests/`
  - `run_zxbasm_tests.sh` — standalone test runner (61/61 passing)
  - `compare_python_c_asm.sh` — Python ground-truth comparison (61/61 identical)
- **Cross-platform** — Windows (MSVC) support
  - `ya_getopt` (BSD-2-Clause) — portable `getopt_long`, replaces POSIX `<getopt.h>`
  - `cwalk` (MIT) — portable path manipulation (`dirname`, `basename`), replaces `<libgen.h>`
  - `compat.h` — minimal POSIX→MSVC shims (`strncasecmp`, `realpath`, `getcwd`, etc.)
- **CI** — Linux x86_64, macOS ARM64, Windows x86_64
  - Added zxbasm test steps and Python comparison
  - Windows: builds and runs zxbasm binary tests (61/61)

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
