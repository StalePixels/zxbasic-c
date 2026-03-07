# WIP: Phase 2 — Z80 Assembler (zxbasm) C Port

**Branch:** `feature/phase2-zxbasm`
**Started:** 2026-03-06
**Status:** Core Complete (61/61 tests pass, Python-identical output)

## Plan

Port the Z80 assembler (`zxbasm`) from Python to C, following the same workflow as Phase 1 (zxbpp). The C binary must be a drop-in replacement: same CLI flags, same input, byte-for-byte identical output.

Reference: [docs/c-port-plan.md](../c-port-plan.md) Phase 2.

### Tasks

- [x] Research: Read all Python zxbasm source, understand architecture
- [x] Research: Catalogue all test cases and their structure (61 with .bin, 32 without)
- [x] Create csrc/zxbasm/ directory structure and CMakeLists.txt
- [x] Implement ASM lexer (hand-written, matching Python token patterns)
- [x] Implement ASM parser (recursive-descent, all Z80 + ZX Next instructions)
- [x] Implement Z80 instruction encoding (827 opcodes via lookup table)
- [x] Implement ZX Next extended opcodes
- [x] Implement memory model with ORG support
- [x] Implement label resolution (two-pass: parse then resolve pending)
- [x] Implement expression evaluation (labels, constants, arithmetic, bitwise)
- [x] Implement preprocessor integration (reuse zxbpp C binary)
- [x] Implement temporary labels (nB/nF with namespace-aware resolution)
- [x] Implement PROC/ENDP scoping and LOCAL labels
- [x] Implement PUSH/POP NAMESPACE
- [x] Implement #init directive (CALL+JP code emission)
- [x] Implement output: raw binary (.bin)
- [x] Implement CLI with matching flags (-d, -e, -o, -O)
- [x] Create test harness: run_zxbasm_tests.sh
- [x] Create test harness: compare_python_c_asm.sh
- [x] Pass all 61 binary-exact test files
- [ ] Implement output: TAP tape format (.tap)
- [ ] Implement output: TZX tape format (.tzx)
- [ ] Implement output: SNA snapshot (.sna)
- [ ] Implement output: Z80 snapshot (.z80)
- [ ] Implement BASIC loader generation
- [ ] Implement memory map output (-M)
- [ ] Update CI workflow for zxbasm tests
- [ ] Update README.md, CHANGELOG-c.md, docs

## Progress Log

### 2026-03-06T00:00 — Start
- Branch created from `main` at `db822c79`.
- Launched research agents to study Python source and existing C patterns.

### 2026-03-06 — Initial assembler
- Built complete Z80 assembler: lexer, recursive-descent parser, 827-opcode table
- Preprocessor integration via zxbpp in ASM mode
- Two-pass assembly: parse + resolve forward references
- 48/61 tests passing

### 2026-03-07 — Fix remaining failures (48→61/61)
- Fixed number lexer: temp label suffix (b/f) must be checked before hex digits
- Fixed opcode emitter: XX skip logic was eating second arg (LD (IX+N),N)
- Fixed second pass: set pending=false before re-emitting bytes for DEFB/DEFW
- Fixed temp label resolution: namespace-aware comparison (Python Label.__eq__)
- Implemented #init directive: CALL+JP code emission after assembly
- Fixed preprocessor: UTF-8 BOM skipping, line continuation in ASM mode
- Fixed IX/IY offset parsing: full expression as offset
- All 61/61 tests pass, Python ground-truth comparison confirms byte-identical output

## Decisions & Notes

- Hand-written recursive-descent parser (no flex/bison dependency), matching Phase 1
- Arena allocation for all assembler data structures
- Reuse csrc/common/ utilities (arena, strbuf, vec, hashmap)
- Reuse zxbpp C binary for preprocessing (fork+exec, same as Python)
- 827 Z80+ZX Next opcodes in static lookup table (z80_opcodes.h)
- Temp labels use namespace comparison per Python Label.__eq__

## Blockers

None currently.

## Commits
d103bf57 - wip: start phase 2 (zxbasm) — init progress tracker
b82552ad - feat: initial zxbasm assembler — compiles and passes smoke test
665d94d9 - fix: resolve all 13 remaining zxbasm test failures — 61/61 pass
