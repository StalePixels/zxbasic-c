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
- [x] Type system enums (TYPE, CLASS, SCOPE, CONVENTION)
- [x] Options/config system (compiler flags struct)
- [x] Error/warning message system (errmsg equivalent)
- [x] Global state struct (replaces api/global_.py)

#### AST Nodes
- [x] Base AST node struct (Tree/Symbol equivalent)
- [x] AST node tag enum (29 node types)
- [x] Individual node structs (tagged union approach)
- [x] Node factory functions (make_node equivalents)
- [x] Type system nodes (TypeInfo with basic/alias/ref tags)

#### Symbol Table
- [x] Scope struct (HashMap + parent pointer + level)
- [x] SymbolTable struct (scope chain, basic_types, type_registry)
- [x] Lookup/declare/enter_scope/exit_scope operations
- [x] Basic type registration

#### Lexer
- [x] Token types enum (120+ tokens including all keywords)
- [x] Keywords table (113 entries)
- [x] Lexer states (INITIAL, string, asm, preproc, comment, bin)
- [x] Number parsing (decimal, hex $XX/0xXX/NNh, octal, binary %NN/NNb)
- [x] String escape sequences (ZX Spectrum specific)
- [x] Label detection (column-based, fixed for start-of-input)
- [x] Line continuation (_ and \)
- [x] Block comments with nesting

#### Parser (hand-written recursive descent + Pratt expressions)
- [x] Precedence levels matching Python's PLY table
- [x] Expression grammar: binary ops, unary, parenthesized, constant folding
- [x] Statement grammar: LET, IF, FOR, WHILE, DO, PRINT, DIM, CONST
- [x] Function/SUB declarations with calling convention, params, return type
- [x] DIM: scalar, array, multi-var, AT, initializers (=> {...})
- [x] CAST, address-of (@), string slicing (x TO y, partial)
- [x] ASM inline blocks
- [x] Preprocessor directives (#line, #init, #require, #pragma)
- [x] Builtin functions with optional parens and multi-arg (PEEK type, CHR$, LBOUND)
- [x] POKE with type, optional parens, speculative parse for all grammar forms
- [x] Print attributes (INK, PAPER, BRIGHT, FLASH, OVER, INVERSE, BOLD, ITALIC)
- [x] ON GOTO/GOSUB, SAVE/LOAD/VERIFY, ERROR
- [x] Named arguments (name:=expr)
- [x] Single-line IF with colon-separated stmts, END IF on same line
- [x] Sinclair-style IF without THEN
- [x] END WHILE alternative to WEND
- [x] Keyword-as-identifier in parameter/function names

#### Semantic Checking
- [ ] Type compatibility checks (common_type) — basic version done
- [ ] Class checking (check_class)
- [ ] Full type inference and coercion
- [ ] Label validation
- [ ] Function overload resolution

#### Build Integration
- [x] CMakeLists.txt for zxbc
- [x] Main entry point (CLI argument parsing with ya_getopt_long)
- [x] --parse-only mode for testing

#### Testing
- [x] Parse-only mode (1020/1036 = 98.5% of .bas files parse successfully)
- [ ] Test harness script
- [ ] Python comparison script

## Progress Log

### 2026-03-07
- Started work. Branch created from `main` at `d92e3f24`.
- Launched research agents to study Python source.
- Built zxbc skeleton: types.h, options.h/c, errmsg.h/c, zxbc.h, ast.c, compiler.c, main.c.
- Implemented full BASIC lexer (850+ lines): all operators, keywords, number formats, strings, ASM blocks, comments.
- Implemented full recursive descent parser with Pratt expression parsing (~2000 lines).
- Iterative parser improvement: 658 → 832 → 889 → 918 → 929 → 955 → 962 → 973 → 984/1036 tests passing.
- Remaining 52 failures: ~25 need preprocessor (#include/#define), ~12 are double-index string slicing (complex), ~15 are edge cases.

### 2026-03-07 (session 2)
- Parser improvements: 984 → 1020/1036 (98.5%).
- Fixed lexer: indented label detection, BIN without digits returns 0.
- Redesigned POKE handler with speculative parse for all Python grammar forms.
- Fixed IF THEN: edge cases (THEN: newline, END IF continuation).
- Added expression-as-statement, named args in sub calls without parens.
- NUMBER at statement start treated as label, AS with unknown ID as type.
- Remaining 16 failures: 7 preprocessor-dependent, 7 expected errors, 2 real gaps (single-line FOR, no-parens function call in expression).

## Decisions & Notes

- **Hand-written recursive descent** chosen over flex+bison (user confirmed). CLAUDE.md updated.
- AST nodes use tagged union with common header (29 tags), matching architecture plan.
- Pratt parser for expressions handles all 13 precedence levels with constant folding.
- Keywords can be used as identifiers in parameter names (ZX BASIC allows this).
- Single-line IF handles colon-separated statements and inline END IF.

## Blockers

None currently.

## Commits
3a1cbc63 - wip: start feature/phase3-zxbc — init progress tracker
ab28fe0b - feat: zxbc skeleton — types, AST, symbol table, options, errmsg, CLI
c2b8a8ac - feat: BASIC lexer with all states, keywords, number formats
e1f5ef92 - feat: BASIC parser — recursive descent with Pratt expressions (955/1036)
166bdf7d - fix: parser improvements — 984/1036 tests pass (95%)
2c9a253c - fix: parser improvements — 1020/1036 tests pass (98.5%)
