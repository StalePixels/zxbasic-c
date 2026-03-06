# WIP: Phase 2 — Z80 Assembler (zxbasm) C Port

**Branch:** `feature/phase2-zxbasm`
**Started:** 2026-03-06
**Status:** In Progress

## Plan

Port the Z80 assembler (`zxbasm`) from Python to C, following the same workflow as Phase 1 (zxbpp). The C binary must be a drop-in replacement: same CLI flags, same input, byte-for-byte identical output.

Reference: [docs/c-port-plan.md](../c-port-plan.md) Phase 2.

### Tasks

- [ ] Research: Read all Python zxbasm source, understand architecture
- [ ] Research: Catalogue all 62 test cases and their structure
- [ ] Research: Understand output format generators (bin, tap, tzx, sna, z80)
- [ ] Create csrc/zxbasm/ directory structure and CMakeLists.txt
- [ ] Implement ASM lexer (flex or hand-written)
- [ ] Implement ASM parser (grammar rules, expression evaluation)
- [ ] Implement Z80 instruction encoding (all opcodes, addressing modes)
- [ ] Implement ZX Next extended opcodes
- [ ] Implement memory model with ORG support
- [ ] Implement label resolution (two-pass or fixup)
- [ ] Implement expression evaluation (labels, constants, arithmetic)
- [ ] Implement preprocessor integration (reuse zxbpp or inline)
- [ ] Implement macro support
- [ ] Implement output: raw binary (.bin)
- [ ] Implement output: TAP tape format (.tap)
- [ ] Implement output: TZX tape format (.tzx)
- [ ] Implement output: SNA snapshot (.sna)
- [ ] Implement output: Z80 snapshot (.z80)
- [ ] Implement BASIC loader generation
- [ ] Implement memory map output (-M)
- [ ] Implement CLI with all flags (matching Python zxbasm exactly)
- [ ] Create test harness: run_zxbasm_tests.sh
- [ ] Create test harness: compare_python_c.sh for zxbasm
- [ ] Pass all 62 binary-exact test files
- [ ] Update CI workflow for zxbasm tests
- [ ] Update README.md, CHANGELOG-c.md, docs

## Progress Log

### 2026-03-06T00:00 — Start
- Branch created from `main` at `db822c79`.
- Launched research agents to study Python source and existing C patterns.

## Decisions & Notes

- Following Phase 1 pattern: hand-written recursive-descent parser (no flex/bison dependency)
- Arena allocation for all assembler data structures
- Reuse csrc/common/ utilities (arena, strbuf, vec, hashmap)

## Blockers

None currently.

## Commits
