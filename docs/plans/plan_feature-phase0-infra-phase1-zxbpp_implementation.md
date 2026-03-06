# WIP: Phase 0 Infrastructure + Phase 1 Preprocessor (zxbpp)

**Branch:** `feature/phase0-infra-phase1-zxbpp`
**Started:** 2026-03-06
**Status:** Complete

## Plan

Implementing the first two phases from [plan.md](../../plan.md):

**Phase 0 — Infrastructure Setup:** Project skeleton, build system (CMake), shared utilities
(strings, dynamic arrays, hash maps), arena memory allocator, CLI argument parsing, test
harness, and CI integration.

**Phase 1 — Preprocessor (zxbpp):** Port the preprocessor (~1,500 lines Python) to C using
a hand-written recursive-descent approach. Covers `#include`, `#define`/`#undef`,
`#ifdef`/`#ifndef`/`#else`/`#endif`, `#if` expression evaluation, `#line` directives,
BASIC and ASM preprocessing modes, token paste/stringize, block comments, and full CLI
compatibility.

### Tasks

#### Phase 0: Infrastructure
- [x] CMake build system
- [x] Common utilities: string handling (dynamic strings)
- [x] Common utilities: dynamic arrays (growable vectors)
- [x] Common utilities: hash maps
- [x] Arena memory allocator
- [x] CLI parsing (getopt_long) in main.c
- [x] Test harness shell script (with stdlib include path auto-detection)
- [ ] CI integration (GitHub Actions)

#### Phase 1: Preprocessor (zxbpp)
- [x] Study Python preprocessor source to understand exact behavior
- [x] Hand-written recursive-descent preprocessor (not flex/bison)
- [x] `#include` file handling with include path search
- [x] `#include once` and `#pragma once`
- [x] `#include` with `[arch:XXX]` architecture modifier
- [x] `#include` with macro expansion in filename
- [x] Windows backslash path support, `./` prefix normalization
- [x] Relative path normalization via realpath()
- [x] `#define` / `#undef` macro expansion (object-like and function-like)
- [x] Nested/recursive macro expansion
- [x] Macro argument pre-expansion before substitution
- [x] Object-like macro rescan for function-like calls
- [x] Token paste (`##`) operator
- [x] Stringize (`#`) operator
- [x] `#ifdef` / `#ifndef` / `#else` / `#endif` conditional compilation
- [x] `#if` expression evaluation (==, !=, <>, <, <=, >, >=, &&, ||, parens)
- [x] Macro expansion in `#if` expressions (object-like and function-like)
- [x] `#line` directive emission matching Python output model
- [x] `has_output` flag for first-define vs subsequent-define distinction
- [x] ifdef scope resets `has_output` (matching Python grammar scoping)
- [x] BASIC comment stripping (`'` and `REM`) from content lines
- [x] ASM mode (`asm`/`end asm`) with `;` comment char
- [x] ASM mode `#line` consumption (silent tracking, no output)
- [x] Block comments (`/' ... '/`)
- [x] Line continuation: backslash `\` (for #define) and underscore `_` (BASIC)
- [x] `#pragma`, `#require`, `#init`, `#error`, `#warning` directives
- [x] Builtin macros: `__FILE__`, `__LINE__`, `__BASE_FILE__`, `__ABS_FILE__`
- [x] CLI: `-o`, `-d`, `-e`, `-D`, `-I`, `--arch`, `--expect-warnings`
- [x] **96/96 tests passing** (91 normal + 5 error tests), **91/91 Python-identical**
- [x] Error handling: suppress output on errors, macro arg count check
- [x] Python ground-truth comparison script (compare_python_c.sh)

## Progress Log

### 2026-03-06T00:00 — Kickoff
- Branch created from `main` at `cdfc70cd`.
- Created this WIP progress tracker.

### 2026-03-06T00:10 — Phase 0 core utilities complete
- Implemented arena allocator, strbuf, vec, hashmap.
- CMake build system wired up.
- Test harness shell script created.
- Studied Python preprocessor source in depth.

### 2026-03-06T00:15 — Remote mishap (fixed)
- Discovered `origin` remote was pointing to Python repo instead of C port.
- Fixed: renamed remotes, re-pushed to correct repo.

### 2026-03-06 — Initial preprocessor implementation
- Built recursive-descent preprocessor engine (~1400 lines).
- First 46 tests passing.

### 2026-03-06 — Output model rewrite
- Read Python PLY grammar source to understand exact output model.
- Rewrote #line emission to match Python behavior precisely.
- 51 tests passing.

### 2026-03-06 — Rapid feature iteration (51→91 tests)
- **Comment stripping + ASM mode** (51→58): Strip `'`/`REM` comments from
  content lines, track `asm`/`end asm` blocks, use `;` for ASM comments.
- **Nested macro expansion** (58→61): Pre-expand macro arguments, rescan
  object-like macros that expand to function-like names.
- **Token paste + stringize** (61→67): `##` and `#` operators in macro bodies.
- **Block comments + underscore continuation** (67→70): `/' ... '/` multi-line
  comments, `_` as BASIC line continuation (not part of identifiers).
- **#if expression evaluator** (70→74): Full recursive-descent parser for
  conditional expressions with operators and macro expansion.
- **Output model fixes** (74→80): ifdef scope resets, ASM #line suppression,
  include closing #line removal, path normalization.
- **Include system** (80→90): stdlib include paths, Windows backslash paths,
  macro expansion in #include, relative path normalization, #pragma/#require
  whitespace trimming.
- **Architecture modifier** (90→91): `[arch:XXX]` modifier in #include.

### 2026-03-06 — Python ground-truth validation (91→96 tests)
- Set up Python 3.12 (`/opt/homebrew/bin/python3.12`) as ground truth reference.
- Created `compare_python_c.sh` to run both Python and C on all tests.
- Fixed error handling: suppress output when errors occur (matching Python's
  `if not global_.has_errors`), add macro argument count validation,
  handle `#define foo()` as epsilon param, reject `\` in ASM content.
- Added `.err` files for 5 error tests. Now **96/96 tests passing, 0 skipped**.
- **91/91 outputs identical to Python** (verified via comparison script).

## Decisions & Notes

- **Hand-written parser instead of flex/bison:** The Python preprocessor uses PLY
  (Python Lex/Yacc), but the output model is so tightly coupled to the grammar
  productions that replicating it exactly in bison would be harder than a direct
  recursive-descent approach. The hand-written parser gives us precise control over
  #line emission timing and output formatting.
- **`has_output` flag:** Tracks whether any content has been emitted, matching the
  Python grammar's distinction between `program : define NEWLINE` (first → blank line)
  and `program : program define NEWLINE` (subsequent → #line directive).
- **ifdef scope resets:** Each enabled ifdef/ifndef/if block resets `has_output` to
  false, matching how the Python grammar creates a new `program` production scope
  inside each ifdef body.
- **ASM mode:** In ASM blocks, `'` is a valid token (register names like `af'`),
  `;` starts comments, and `#line` directives are consumed silently.

## Blockers

None. Phase 1 is feature-complete with all tests passing.

## Commits

`eb46f8ee` - wip: start feature/phase0-infra-phase1-zxbpp — init progress tracker
`0df8bbd6` - feat: add Phase 0 core infrastructure — arena, strbuf, vec, hashmap
`570eb3c7` - feat: add CMake build system and test harness skeleton
`4478cc39` - feat: initial zxbpp preprocessor — builds and runs basic macro expansion
`cc90a50f` - fix: rewrite #line emission to match Python preprocessor output model
`60f05a51` - fix: crash in parse_macro_args due to use-after-detach
`cd1f5be1` - feat: line continuation in #define with BASIC comments
`cea28f8b` - feat: strip inline comments from content lines and add ASM mode
`cbdda850` - feat: nested macro expansion and argument pre-expansion
`b021fba4` - feat: implement token pasting (##) and stringizing (#) operators
`40a76d75` - feat: block comments (/' ... '/), underscore line continuation
`acc41519` - feat: full #if expression evaluator with operators and macros
`a31e89af` - fix: multiple output model fixes for includes, ASM, and ifdef scoping
`1a57c308` - feat: include improvements — Windows paths, macro expansion, stdlib paths
`b455c042` - fix: directive whitespace trimming, relative path normalization
`32be4b49` - feat: architecture-specific includes [arch:XXX] modifier
`fae4c88b` - fix: suppress output on errors, add macro arg count check, ASM backslash error
`29b90c79` - feat: Python ground-truth comparison script, test harness .err support
`1f1d6c1e` - test: add .err files for 5 error tests
`b023b7e2` - docs: update CLAUDE.md with Python ground-truth testing workflow
`32be4b49` - feat: architecture-specific includes [arch:XXX] modifier
