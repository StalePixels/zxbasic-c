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
- [ ] CMake build system with flex/bison support
- [ ] Common utilities: string handling (dynamic strings)
- [ ] Common utilities: dynamic arrays (growable vectors)
- [ ] Common utilities: hash maps
- [ ] Arena memory allocator
- [ ] Global options struct and CLI parsing (getopt_long)
- [ ] Test harness shell script
- [ ] CI integration (GitHub Actions)

#### Phase 1: Preprocessor (zxbpp)
- [ ] Study Python preprocessor source to understand exact behavior
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

## Decisions & Notes

- Using CMake as build system (plan.md specified "CMake or Makefile" — CMake chosen for
  better flex/bison integration and cross-platform support).
- Will target C11 standard for modern but widely-supported C.
- Arena allocator will be the primary allocation strategy per plan.md.

## Blockers

None currently.

## Commits

`pending` - wip: init progress tracker
