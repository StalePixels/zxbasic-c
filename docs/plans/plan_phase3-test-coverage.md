# WIP: Phase 3 Test Coverage — Beyond Functional Tests

**Branch:** `feature/phase3-zxbc`
**Started:** 2026-03-07
**Status:** In Progress

## Plan

Match the Python project's non-functional test suites with C equivalents for all completed port components. Reference: CLAUDE.md "Beyond Functional Tests" table.

### Tasks

- [ ] Fix --org bug: value parsed but not stored (case 'S': break; drops it)
- [ ] Add `org` field to CompilerOptions, support 0xNNNN hex format
- [ ] Implement config file loading (-F) for .ini files
- [ ] Create C unit test harness (simple assert-based, no external deps)
- [ ] cmdline tests: test_compile_only (--parse-only no output file)
- [ ] cmdline tests: test_org_allows_0xnnnn_format
- [ ] cmdline tests: test_org_loads_ok_from_config_file_format
- [ ] cmdline tests: test_cmdline_should_override_config_file
- [ ] config tests: CompilerOptions defaults match Python config.init()
- [ ] config tests: None-ignoring behavior for boolean options
- [ ] config tests: load/save config from .ini file
- [ ] arg_parser tests: autorun defaults to unset (not false)
- [ ] arg_parser tests: basic/loader defaults to unset (not false)
- [ ] utils tests: implement parse_int + test all formats (dec, hex, bin, $XX, XXh)
- [ ] type system tests: basictype sizes, signedness, predicates
- [ ] type system tests: type_equal, type_is_basic, type_is_numeric, type_is_string
- [ ] symbol table tests: init (basic types registered)
- [ ] symbol table tests: declare, lookup, scoping
- [ ] symbol table tests: enter/exit scope, local var cleanup
- [ ] check tests: is_temporary_value for STRING, ID, BINARY nodes
- [ ] Update CI workflow to run new tests
- [ ] Update README badge counts

### Out of Scope (not yet implemented in C)
- Symbol/AST node arithmetic tests (tests/symbols/) — need richer AST node behavior
- Backend tests (test_memcell.py) — Phase 4
- Optimizer tests (6 files) — Phase 4
- Peephole tests (4 files) — Phase 4

## Progress Log

### 2026-03-07T00:00
- Started work. Surveyed all Python test suites.
- Identified --org bug (value dropped at line 212 of main.c).
- Identified config file loading as missing feature.
- Identified 4 cmdline tests, config defaults, utils, type system, symbol table as actionable.

## Decisions & Notes

- C tests use a simple assert-based harness (no external test framework — matches rule 5: no external deps)
- cmdline tests are shell scripts that invoke the zxbc binary (matching Python's subprocess approach)
- Unit tests are C programs linked against the component libraries
- Config file format: standard .ini with [zxbc] section (Python uses configparser)

## Blockers

None currently.

## Commits
