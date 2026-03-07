# WIP: Phase 3 Test Coverage — Beyond Functional Tests

**Branch:** `feature/phase3-zxbc`
**Started:** 2026-03-07
**Status:** Complete

## Plan

Match the Python project's non-functional test suites with C equivalents for all completed port components. Reference: CLAUDE.md "Beyond Functional Tests" table.

### Tasks

- [x] Fix --org bug: value parsed but not stored (case 'S': break; drops it)
- [x] Add `org` field to CompilerOptions, support 0xNNNN hex format
- [x] Implement config file loading (-F) for .ini files
- [x] Create C unit test harness (simple assert-based, no external deps)
- [x] cmdline tests: test_compile_only (--parse-only no output file)
- [x] cmdline tests: test_org_allows_0xnnnn_format
- [x] cmdline tests: test_org_loads_ok_from_config_file_format
- [x] cmdline tests: test_cmdline_should_override_config_file
- [x] config tests: CompilerOptions defaults match Python config.init()
- [x] config tests: None-ignoring behavior for boolean options
- [x] config tests: load/save config from .ini file
- [x] arg_parser tests: defaults tested via config init test
- [x] utils tests: implement parse_int + test all formats (dec, hex, bin, $XX, XXh)
- [x] type system tests: basictype sizes, signedness, predicates
- [x] type system tests: type_equal, type_is_basic, type_is_numeric, type_is_string
- [x] symbol table tests: init (basic types registered)
- [x] symbol table tests: declare, lookup, scoping
- [x] symbol table tests: enter/exit scope, local var cleanup
- [x] check tests: is_temporary_value for STRING, ID, BINARY nodes
- [x] Update CI workflow to run new tests
- [x] Update README badge counts
- [x] Extract zxbc_parse_args() into testable args.c
- [x] test_cmdline: 15 value-verification tests (org, optimization_level, config override)
- [x] test_symboltable: expand to 22 tests (all 18 Python + 4 extras)
- [x] test_check: 4 tests matching test_check.py
- [x] test_ast: expand to 61 tests covering all 19 tests/symbols/ files

### Out of Scope (not yet implemented in C)
- Symbol/AST node arithmetic tests (Python operator overloading — not applicable to C)
- Backend tests (test_memcell.py) — Phase 4
- Optimizer tests (6 files) — Phase 4
- Peephole tests (4 files) — Phase 4

## Progress Log

### 2026-03-07T00:00
- Started work. Surveyed all Python test suites.
- Identified --org bug (value dropped at line 212 of main.c).
- Identified config file loading as missing feature.
- Identified 4 cmdline tests, config defaults, utils, type system, symbol table as actionable.

### 2026-03-07T01:00
- Fixed --org bug: now stores value using parse_int, supports 0xNNNN format.
- Added org, heap_size, heap_address, headerless, parse_only to CompilerOptions.
- Implemented parse_int() in common/utils.c (all Python test cases pass).
- Implemented config_file.c — .ini reader with duplicate key detection.
- Wired up -F/--config-file with cmdline override logic.

### 2026-03-07T02:00
- Created test_harness.h (minimal C test framework, no external deps).
- Created 5 unit test programs: test_utils, test_config, test_types, test_ast, test_symboltable.
- Created run_cmdline_tests.sh (4 tests matching tests/cmdline/test_zxb.py).
- All 56 tests pass. All 1036 parse-only tests still pass. 7 ctest suites green.
- Updated CI workflow to run unit + cmdline tests on Unix and Windows.

### 2026-03-07T03:00
- Extracted zxbc_parse_args() into args.c for testability.
- Fixed ya_getopt re-entrancy bug (static start/end not reset).
- Created test_cmdline.c (15 tests verifying actual option values).
- Added cmdline_set bitmask to CompilerOptions for config-override semantics.

### 2026-03-07T04:00
- Added full symbol table API: declare_variable, declare_param, declare_array.
- Added check module: is_temporary_value.
- Expanded test_symboltable to 22 tests (all 18 Python + 4 extras).
- Created test_check.c (4 tests).

### 2026-03-07T05:00
- Expanded test_ast to 61 tests covering all 19 tests/symbols/ files.
- Fixed ast_tag_name buffer overflow (missing ARRAYINIT entry).
- Total: 132 unit tests + 4 shell tests = 136 C-specific tests.
- Updated all docs, VERSION bumped to 1.18.7+c3.

## Decisions & Notes

- C tests use a simple assert-based harness (no external test framework — matches rule 5: no external deps)
- cmdline tests are shell scripts that invoke the zxbc binary (matching Python's subprocess approach)
- Unit tests are C programs linked against the component libraries
- Config file format: standard .ini with [zxbc] section (Python uses configparser)
- ya_getopt re-entrancy: must reset static start/end vars when ya_optind=0
- tmpfile() used for portable stderr capture in tests (vs fmemopen)
- test_build_parsetab.py is N/A (PLY-specific, not applicable to hand-written parser)
- Python tests for NUMBER arithmetic (__add__, __eq__) test operator overloading — not applicable to C

## Blockers

None currently.

## Commits
b97de372 - wip: start phase 3 test coverage — init progress tracker
38da3b2c - feat: fix --org storage, add parse_int, config file loading
f4d7aef2 - test: add unit tests matching Python test suites — 56 tests
956eee30 - ci: add unit tests and cmdline tests to CI workflow
7a74f74c - fix: Windows build — add compat.h for strcasecmp, fix test_config portability
ee654b3b - feat: extract args parsing, add cmdline value tests matching Python
1720ed4a - feat: add full symbol table API + check module, matching Python test suites
484dc650 - feat: expand AST tests to cover all 19 symbol node types (61 tests)
