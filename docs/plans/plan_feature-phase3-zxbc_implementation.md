# WIP: Phase 3 — BASIC Compiler Frontend

**Branch:** `feature/phase3-zxbc`
**Started:** 2026-03-07
**Status:** In Progress

## Plan

Port the BASIC compiler frontend from Python to C, as defined in [Phase 3 of c-port-plan.md](../c-port-plan.md#phase-3-basic-compiler-frontend).

**Python source:** ~11,800 lines across `src/zxbc/`, `src/symbols/`, `src/api/`
**Estimated C output:** ~15,000 lines

### Tasks

#### 3a: Foundation (Complete)
- [x] Type system enums (TYPE, CLASS, SCOPE, CONVENTION)
- [x] Options/config system (compiler flags struct)
- [x] Error/warning message system (errmsg equivalent)
- [x] Global state struct (replaces api/global_.py)
- [x] Base AST node struct (Tree/Symbol equivalent)
- [x] AST node tag enum (30 node types)
- [x] Individual node structs (tagged union approach)
- [x] Node factory functions (make_node equivalents)
- [x] Type system nodes (TypeInfo with basic/alias/ref tags)

#### 3b: Symbol Table (Complete)
- [x] Scope struct (HashMap + parent pointer + level)
- [x] SymbolTable struct (scope chain, basic_types, type_registry)
- [x] Lookup/declare/enter_scope/exit_scope operations
- [x] Basic type registration
- [x] symboltable_declare_variable — type refs, suffix stripping, duplicate detection
- [x] symboltable_declare_param — SCOPE_parameter, duplicate error with line numbers
- [x] symboltable_declare_array — TYPEREF + BOUNDLIST validation
- [x] check_is_declared / check_is_undeclared — scope-aware lookup
- [x] is_temporary_value — matches Python's api/check.py

#### 3c: Lexer (Complete)
- [x] Token types enum (120+ tokens including all keywords)
- [x] Keywords table (113 entries)
- [x] Lexer states (INITIAL, string, asm, preproc, comment, bin)
- [x] Number parsing (decimal, hex $XX/0xXX/NNh, octal, binary %NN/NNb)
- [x] String escape sequences (ZX Spectrum specific)
- [x] Label detection (column-based, fixed for start-of-input)
- [x] Line continuation (_ and \)
- [x] Block comments with nesting

#### 3d: Parser — Syntax (Complete)
- [x] Precedence levels matching Python's PLY table
- [x] Expression grammar: binary ops, unary, parenthesized, constant folding
- [x] Statement grammar: LET, IF, FOR, WHILE, DO, PRINT, DIM, CONST
- [x] Function/SUB declarations with calling convention, params, return type
- [x] DIM: scalar, array, multi-var, AT, initializers (=> {...})
- [x] CAST, address-of (@), string slicing (x TO y, partial)
- [x] ASM inline blocks
- [x] Preprocessor directives (#line, #init, #require, #pragma)
- [x] Builtin functions with optional parens and multi-arg (PEEK type, CHR$, LBOUND)
- [x] POKE with type, optional parens, deterministic disambiguation
- [x] Print attributes (INK, PAPER, BRIGHT, FLASH, OVER, INVERSE, BOLD, ITALIC)
- [x] ON GOTO/GOSUB, SAVE/LOAD/VERIFY, ERROR
- [x] Named arguments (name:=expr)
- [x] Single-line IF, END WHILE, keyword-as-identifier
- [x] PLOT/DRAW/CIRCLE with graphics attributes
- [x] Parse-only mode: **1036/1036 (100%)**

#### 3e: Build & CLI (Complete)
- [x] CMakeLists.txt for zxbc
- [x] Main entry point (CLI argument parsing with ya_getopt_long)
- [x] --parse-only mode for testing
- [x] Config file loading (-F), cmdline_set bitmask for override semantics
- [x] Preprocessor integration (zxbpp_lib static library)
- [x] ya_getopt re-entrancy fix (static start/end reset)

#### 3f: Test Coverage (Complete)
- [x] test_utils (14 tests) — matches test_utils.py
- [x] test_config (6 tests) — matches test_config.py + test_arg_parser.py
- [x] test_types (10 tests) — matches test_symbolBASICTYPE.py
- [x] test_ast (61 tests) — matches all 19 tests/symbols/ files
- [x] test_symboltable (22 tests) — matches test_symbolTable.py (18 + 4 extras)
- [x] test_check (4 tests) — matches test_check.py
- [x] test_cmdline (15 tests) — matches test_zxb.py + test_arg_parser.py
- [x] run_cmdline_tests.sh (4 tests) — zxbc exit-code tests
- [x] CI workflow updated (Unix + Windows)

#### 3g: Semantic Analysis — Symbol Resolution (TODO)
Port the symbol table `access_*` methods that resolve identifiers during parsing.
Reference: `src/api/symboltable/symboltable.py` lines 326-475.

- [ ] `access_id()` — resolve ID to var/func/array/label with class-based dispatch
- [ ] `access_var()` — auto-declare undeclared vars, suffix handling, implicit types
- [ ] `access_call()` — callable check, var/func/array disambiguation
- [ ] `access_func()` / `access_array()` — specific class accessors
- [ ] `access_label()` — label resolution
- [ ] Wire up ID resolution in expression parsing (replace bare AST_ID creation)
- [ ] Wire up `make_call()` disambiguation — `ID(args)` → function call / array access / string slice

#### 3h: Semantic Analysis — Type Coercion (TODO)
Port the type coercion machinery that inserts TYPECAST nodes.
Reference: `src/symbols/typecast.py`, `src/symbols/binary.py`, `src/symbols/unary.py`.

- [ ] `make_typecast()` — implicit coercion with error reporting, constant static casts
- [ ] `make_binary()` full semantics — operand type coercion, CONSTEXPR wrapping, string checks
- [ ] `make_unary()` — type propagation, constant folding
- [ ] Boolean-to-ubyte coercion for non-boolean operators
- [ ] SHL/SHR special coercion (float→ulong, amount→ubyte)
- [ ] POW operand coercion to float
- [ ] CONSTEXPR wrapping for static non-constant expressions

#### 3i: Semantic Analysis — Scope & Functions (TODO)
Port the scope management and function semantics.
Reference: `src/zxbc/zxbparser.py` function-related p_* actions.

- [ ] FUNCTION_LEVEL stack tracking (push/pop during SUB/FUNCTION)
- [ ] Register parameters in scope during function body parsing
- [ ] Namespace mangling (`_name`, `_ns.name`)
- [ ] `enter_scope(namespace)` with namespace compounding
- [ ] `leave_scope()` — offset computation, unused var warnings, unknown→global promotion
- [ ] `move_to_global_scope()` — forward function references
- [ ] `check_call_arguments()` — parameter matching, typecast, byref validation
- [ ] Forward declaration (DECLARE) matching
- [ ] RETURN semantics — sub vs function context, return type checking/casting
- [ ] `declare_label()` — global label registration, DATA linkage

#### 3j: Semantic Analysis — Statements (TODO)
Wire up semantic actions for each statement type.

- [ ] LET assignment — access_id(), type inference from RHS, typecast insertion
- [ ] FOR — access_var() on loop var, common_type for start/end/step, make_typecast
- [ ] PRINT — typecast on AT/TAB args (to ubyte)
- [ ] DATA/READ/RESTORE — label creation, DATA function wrappers
- [ ] ON GOTO/GOSUB — label resolution
- [ ] DIM AT — constant address requirement checking
- [ ] `make_static()` — local vars at absolute addresses

#### 3k: Post-Parse Validation (TODO)
Port the post-parse checks and passes that Python runs even for --parse-only.

- [ ] `check_pending_calls()` — deferred validation of forward-referenced function calls
- [ ] `check_pending_labels()` — post-parse label existence check
- [ ] Error/warning messages — port remaining from errmsg.py
- [ ] Unused var/param warnings at scope exit

#### 3l: Post-Parse Visitors (TODO)
Port the AST visitors that Python runs before code generation (even for --parse-only).

- [ ] UnreachableCodeVisitor — dead code removal after RETURN/END
- [ ] FunctionGraphVisitor — mark accessed functions via call graph
- [ ] AST visitor framework (function pointer dispatch over node tags)

## Progress Log

### 2026-03-07 (session 1)
- Started work. Branch created from `main` at `d92e3f24`.
- Built zxbc skeleton: types.h, options.h/c, errmsg.h/c, zxbc.h, ast.c, compiler.c, main.c.
- Implemented full BASIC lexer (850+ lines).
- Implemented full recursive descent parser with Pratt expression parsing (~2000 lines).
- Iterative improvement: 658 → 832 → 889 → 918 → 929 → 955 → 962 → 973 → 984/1036.

### 2026-03-07 (session 2)
- Parser: 984 → 1020/1036 (98.5%).
- Fixed lexer: indented label detection, BIN without digits returns 0.
- Redesigned POKE handler with deterministic disambiguation.
- Fixed IF THEN edge cases, expression-as-statement, named args.

### 2026-03-07 (session 3)
- Code quality audit: removed all `(void)` casts and token-skipping hacks.
- Implemented parse_gfx_attributes() for PLOT/DRAW/CIRCLE.
- Integrated zxbpp as library — **1036/1036 (100%)**.

### 2026-03-07 (session 4)
- Fixed --org storage, added parse_int, config file loading.
- Created test harness, 56 unit tests across 5 programs.
- CI workflow updated with unit + cmdline tests.
- Windows build fixes (compat.h, test_config portability).

### 2026-03-07 (session 5)
- Extracted zxbc_parse_args() into args.c for testability.
- Fixed ya_getopt re-entrancy bug (static start/end not reset).
- Added full symbol table API (declare_variable, declare_param, declare_array).
- Added check module (is_temporary_value).
- Expanded test_ast to 61 tests covering all 19 tests/symbols/ files.
- Total: 132 unit tests + 4 shell tests = 136 C-specific tests.
- VERSION bumped to 1.18.7+c3.

## Decisions & Notes

- **Hand-written recursive descent** chosen over flex+bison (user confirmed).
- AST nodes use tagged union with common header (30 tags).
- Pratt parser for expressions handles all 13 precedence levels.
- **zxbpp_lib is a static library** linked into both zxbpp and zxbc.
- ya_getopt re-entrancy: must reset static start/end vars when ya_optind=0.
- tmpfile() for portable stderr capture in tests.
- test_build_parsetab.py is N/A (PLY-specific).
- Python NUMBER arithmetic tests (__add__, __eq__) test operator overloading — N/A for C.
- Python --parse-only runs 3 AST visitors (unreachable code, function graph, optimizer) after parsing. Our C --parse-only currently exits after syntax parsing only.

## Blockers

None currently.

## Commits
3a1cbc63 - wip: start feature/phase3-zxbc — init progress tracker
ab28fe0b - feat: zxbc skeleton — types, AST, symbol table, options, errmsg, CLI
c2b8a8ac - feat: BASIC lexer with all states, keywords, number formats
e1f5ef92 - feat: BASIC parser — recursive descent with Pratt expressions (955/1036)
166bdf7d - fix: parser improvements — 984/1036 tests pass (95%)
2c9a253c - fix: parser improvements — 1020/1036 tests pass (98.5%)
480537f5 - fix: parser quality — proper gfx attributes, no voided values (1030/1036)
14e35283 - feat: integrate zxbpp preprocessor into zxbc — 1036/1036 tests pass (100%)
38da3b2c - feat: fix --org storage, add parse_int, config file loading
f4d7aef2 - test: add unit tests matching Python test suites — 56 tests
956eee30 - ci: add unit tests and cmdline tests to CI workflow
7a74f74c - fix: Windows build — add compat.h for strcasecmp, fix test_config portability
ee654b3b - feat: extract args parsing, add cmdline value tests matching Python
1720ed4a - feat: add full symbol table API + check module, matching Python test suites
484dc650 - feat: expand AST tests to cover all 19 symbol node types (61 tests)
edd66845 - docs: update all docs for release 1.18.7+c3, bump VERSION
