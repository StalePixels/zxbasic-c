# WIP: Phase 3 — BASIC Compiler Frontend

**Branch:** `feature/phase3-zxbc`
**Started:** 2026-03-07
**Status:** In Progress

## Plan

Port the BASIC compiler frontend from Python to C, as defined in [Phase 3 of c-port-plan.md](../c-port-plan.md#phase-3-basic-compiler-frontend).

**Python source:** ~11,800 lines across `src/zxbc/`, `src/symbols/`, `src/api/`
**Estimated C output:** ~15,000 lines

### Tasks

#### Foundation
- [ ] Type system enums (TYPE, CLASS, SCOPE, CONVENTION)
- [ ] Options/config system (compiler flags struct)
- [ ] Error/warning message system (errmsg equivalent)
- [ ] Global state struct (replaces api/global_.py)

#### AST Nodes
- [ ] Base AST node struct (Tree/Symbol equivalent)
- [ ] AST node tag enum (all 25+ node types)
- [ ] Individual node structs (tagged union approach)
- [ ] Node factory functions (make_node equivalents)
- [ ] Type system nodes (SymbolTYPE, SymbolBASICTYPE, etc.)

#### Symbol Table
- [ ] Scope struct (variable bindings per scope level)
- [ ] SymbolTable struct (scope stack, type registry)
- [ ] Lookup/declare/enter_scope/exit_scope operations
- [ ] Basic type registration

#### Lexer
- [ ] Token types enum (all BASIC tokens + keywords)
- [ ] Keywords table
- [ ] Lexer states (INITIAL, string, asm, preproc, comment, bin)
- [ ] Number parsing (decimal, hex, octal, binary)
- [ ] String escape sequences (ZX Spectrum specific)
- [ ] Label detection (column-based)
- [ ] Line continuation
- [ ] Block comments

#### Parser
- [ ] Precedence declarations
- [ ] Expression grammar rules
- [ ] Statement grammar rules (LET, IF, FOR, WHILE, DO, PRINT, etc.)
- [ ] Function/SUB declarations
- [ ] DIM/array declarations
- [ ] Type casting rules
- [ ] ASM inline blocks
- [ ] Preprocessor directives (#line, #init, #require, #pragma)
- [ ] Semantic actions (type checking, symbol table updates)

#### Semantic Checking
- [ ] Type compatibility checks (common_type)
- [ ] Class checking (check_class)
- [ ] Numeric/string/unsigned predicates
- [ ] Label validation

#### Build Integration
- [ ] CMakeLists.txt for zxbc
- [ ] flex/bison integration
- [ ] Main entry point (CLI argument parsing)

#### Testing
- [ ] Test harness script
- [ ] Parse-only mode (verify all .bas files parse without errors)
- [ ] Python comparison script

## Progress Log

### 2026-03-07
- Started work. Branch created from `main` at `d92e3f24`.
- Launched research agents to study Python source (AST nodes, parser grammar, symbol table).

## Decisions & Notes

- Following same patterns as zxbasm: Arena allocator, hand-rolled lexer (or flex), state struct
- CLAUDE.md specifies flex+bison for zxbc parser (unlike zxbpp which is hand-written)
- AST nodes use tagged union with common header, matching CLAUDE.md architecture
- The parser is 3,593 lines of PLY yacc rules — this is the largest single file to port
- 1,036 .bas test files in tests/functional/arch/zx48k/

## Blockers

None currently.

## Commits
d92e3f24 - Branch created from main
