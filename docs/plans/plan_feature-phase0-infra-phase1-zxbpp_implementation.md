# WIP: Phase 0 Infrastructure + Phase 1 Preprocessor (zxbpp)

**Branch:** `feature/phase0-infra-phase1-zxbpp`
**Started:** 2026-03-06
**Status:** In Progress

## Plan

Implementing the first two phases from [plan.md](../../plan.md):

**Phase 0 — Infrastructure Setup:** Project skeleton, build system (CMake), shared utilities
(strings, dynamic arrays, hash maps), arena memory allocator, CLI argument parsing, test
harness, and CI integration.

**Phase 1 — Preprocessor (zxbpp):** Port the preprocessor (~1,500 lines Python) to C using
flex/bison. Covers `#include`, `#define`/`#undef`, `#ifdef`/`#ifndef`/`#else`/`#endif`,
`#line` directives, BASIC and ASM preprocessing modes, and full CLI compatibility.

### Tasks

#### Phase 0: Infrastructure
- [x] CMake build system with flex/bison support
- [x] Common utilities: string handling (dynamic strings)
- [x] Common utilities: dynamic arrays (growable vectors)
- [x] Common utilities: hash maps
- [x] Arena memory allocator
- [ ] Global options struct and CLI parsing (getopt_long)
- [x] Test harness shell script
- [ ] CI integration (GitHub Actions)

#### Phase 1: Preprocessor (zxbpp)
- [x] Study Python preprocessor source to understand exact behavior
- [ ] Preprocessor lexer (flex) from zxbpplex.py / zxbasmpplex.py
- [ ] Preprocessor parser (bison) from zxbpp.py
- [ ] `#include` file handling with include path search
- [ ] `#define` / `#undef` macro expansion
- [ ] `#ifdef` / `#ifndef` / `#else` / `#endif` conditional compilation
- [ ] `#line` directive emission
- [ ] BASIC and ASM preprocessing modes
- [ ] CLI: `-o`, `-d`, `-e`, `--arch`, `--expect-warnings`
- [ ] Run against 91 preprocessor tests
- [ ] Fix failures and iterate until all tests pass

## Progress Log

### 2026-03-06T00:00 — Kickoff
- Branch created from `main` at `cdfc70cd`.
- Created this WIP progress tracker.

### 2026-03-06T00:10 — Phase 0 core utilities complete
- Implemented arena allocator, strbuf, vec, hashmap.
- CMake build system with flex/bison wired up.
- Test harness shell script created.
- Studied Python preprocessor source in depth (14 files, ~3000 lines).
  Documented architecture, token set, grammar, macro system, include
  handling, conditional compilation, and output format.

### 2026-03-06T00:15 — Remote mishap (fixed)
- **INCIDENT:** Discovered `origin` remote was pointing to `StalePixels/zxbasic`
  (the Python fork) instead of `StalePixels/zxbasic-c` (the C port repo).
  Accidentally pushed feature branch to the Python repo.
- **Fix:** Deleted accidental branch from Python repo. Renamed remotes so
  `origin` = `zxbasic-c` and `python-upstream` = `zxbasic`. Re-pushed
  feature branch to the correct repo.

## Decisions & Notes

- Using CMake as build system (plan.md specified "CMake or Makefile" — CMake chosen for
  better flex/bison integration and cross-platform support).
- Will target C11 standard for modern but widely-supported C.
- Arena allocator will be the primary allocation strategy per plan.md.
- **Bison 2.3 constraint:** macOS ships bison 2.3 (2006 vintage). Must write
  grammar using old-style syntax — no `%define api.pure full`, no `%code`
  blocks. Using `%union` and traditional yylex/yyparse signatures.
- **Remote layout:** `origin` = `StalePixels/zxbasic-c` (C port),
  `python-upstream` = `StalePixels/zxbasic` (Python source for reference).

## Blockers

None currently.

## Commits

`eb46f8ee` - wip: start feature/phase0-infra-phase1-zxbpp — init progress tracker
`0df8bbd6` - feat: add Phase 0 core infrastructure — arena, strbuf, vec, hashmap
`570eb3c7` - feat: add CMake build system and test harness skeleton
