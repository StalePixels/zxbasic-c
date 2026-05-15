# Top-level Makefile for the C port (Round 0 test-infra rebuild).
# Thin entry layer: defers to CMake for builds; harness shell scripts
# land target by target as later sprints add them.

CMAKE       ?= cmake
BUILD_DIR   ?= csrc/build
BUILD_TYPE  ?= Release

ZXBPP_C      = $(BUILD_DIR)/zxbpp/zxbpp
ZXBPP_TESTS  = tests/functional/zxbpp
ZXBASM_C     = $(BUILD_DIR)/zxbasm/zxbasm
ZXBASM_TESTS = tests/functional/asm
ZXBC_C       = $(BUILD_DIR)/zxbc/zxbc
ZXBC_TESTS   = tests/functional/arch/zx48k

.PHONY: build clean test-zxbpp-strict test-zxbpp-fuzzy verify-phase1-calibration verify-phase2-calibration test-zxbasm-strict test-zxbasm-fuzzy sweep-asm-zero-byte verify-phase3-calibration regenerate-zxbc-baselines test-zxbc-parse-strict test-zxbc-parse-fuzzy verify-phase4-calibration test-zxbc-ast-equiv verify-phase5-calibration

build:
	$(CMAKE) -S csrc -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build $(BUILD_DIR) -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

clean:
	rm -rf $(BUILD_DIR) Testing

test-zxbpp-strict: $(ZXBPP_C)
	./csrc/tests/run_zxbpp_tests_strict.sh $(ZXBPP_C) $(ZXBPP_TESTS)

test-zxbpp-fuzzy: $(ZXBPP_C)
	./csrc/tests/run_zxbpp_tests.sh $(ZXBPP_C) $(ZXBPP_TESTS)

# Calibration: prepro00 must FAIL strict + PASS fuzzy. The disagreement
# is the proof that the strict harness changes verdicts on the same test
# set the fuzzy harness already accepts (Phase 1 calibration test, named
# in docs/captures/zxbasic-c/phase1-strict-divergences.md).
verify-phase1-calibration: $(ZXBPP_C)
	@set -e; \
	strict_out=$$(./csrc/tests/run_zxbpp_tests_strict.sh $(ZXBPP_C) $(ZXBPP_TESTS) 2>&1 || true); \
	fuzzy_out=$$(./csrc/tests/run_zxbpp_tests.sh $(ZXBPP_C) $(ZXBPP_TESTS) 2>&1 || true); \
	if ! echo "$$strict_out" | grep -qE '^  prepro00$$|^--- FAIL: prepro00'; then \
	    echo "FAIL: strict harness did not flag prepro00 as failing"; \
	    echo "$$strict_out" | tail -20; exit 1; \
	fi; \
	if echo "$$fuzzy_out" | grep -qE 'FAIL: prepro00'; then \
	    echo "FAIL: fuzzy harness reported prepro00 FAIL — calibration unsound"; exit 1; \
	fi; \
	if ! echo "$$fuzzy_out" | grep -qE 'Results: 96 passed'; then \
	    echo "FAIL: fuzzy harness did not report 96/96 — base shifted"; \
	    echo "$$fuzzy_out" | tail -3; exit 1; \
	fi; \
	echo "verify-phase1-calibration: OK (prepro00 strict-FAIL + fuzzy-PASS)"

# Phase 2 calibration: prepro22 (zxbpp error test). The .err message body
# is identical between Python and C, but Python reports the macro
# call-site at line 4 while C reports line 7 (the multi-line macro's
# expanded body) — the strict harness's stderr-content comparison
# catches it; the fuzzy harness's exit-code-only check passes both.
verify-phase2-calibration: $(ZXBPP_C)
	@set -e; \
	strict_out=$$(./csrc/tests/run_zxbpp_tests_strict.sh $(ZXBPP_C) $(ZXBPP_TESTS) 2>&1 || true); \
	fuzzy_out=$$(./csrc/tests/run_zxbpp_tests.sh $(ZXBPP_C) $(ZXBPP_TESTS) 2>&1 || true); \
	if ! echo "$$strict_out" | grep -qE 'ERR-FAIL: prepro22|^  prepro22$$'; then \
	    echo "FAIL: strict harness did not flag prepro22 as error-test failing"; \
	    echo "$$strict_out" | tail -25; exit 1; \
	fi; \
	if echo "$$fuzzy_out" | grep -qE 'FAIL: prepro22'; then \
	    echo "FAIL: fuzzy harness reported prepro22 FAIL — calibration unsound"; exit 1; \
	fi; \
	if ! echo "$$fuzzy_out" | grep -qE 'Results: 96 passed'; then \
	    echo "FAIL: fuzzy harness did not report 96/96 — base shifted"; exit 1; \
	fi; \
	echo "verify-phase2-calibration: OK (prepro22 strict-FAIL + fuzzy-PASS)"

test-zxbasm-strict: $(ZXBASM_C)
	./csrc/tests/run_zxbasm_tests_strict.sh $(ZXBASM_C) $(ZXBASM_TESTS)

test-zxbasm-fuzzy: $(ZXBASM_C)
	./csrc/tests/run_zxbasm_tests.sh $(ZXBASM_C) $(ZXBASM_TESTS)

# Sweep for any zero-byte .bin fixtures. With Sprint 4's regen of
# asmerror0.err, the strict harness no longer accepts asmerror0's
# 0-byte fixture as a success-cmp (it's classified as an error test
# now — sibling .err authoritative). New 0-byte arrivals are flagged
# here for fixture-regen review.
sweep-asm-zero-byte:
	@matches=$$(find tests/functional -name '*.bin' -size 0); \
	count=$$(printf '%s\n' "$$matches" | grep -c .); \
	echo "$$matches"; \
	echo "$$count zero-byte .bin files found"

# Phase 3 calibration: asmerror0 must FAIL strict (because Python and
# C disagree on line number — Python:4, C:5; same line-tracking
# divergence shape as Phase 2's prepro22) AND PASS fuzzy (because
# fuzzy compares the empty .bin against zxbasm's empty output, which
# trivially succeeds via the 0-byte cmp loophole this sprint closes).
verify-phase3-calibration: $(ZXBASM_C)
	@set -e; \
	strict_out=$$(./csrc/tests/run_zxbasm_tests_strict.sh $(ZXBASM_C) $(ZXBASM_TESTS) 2>&1 || true); \
	fuzzy_out=$$(./csrc/tests/run_zxbasm_tests.sh $(ZXBASM_C) $(ZXBASM_TESTS) 2>&1 || true); \
	if ! echo "$$strict_out" | grep -qE 'ERR-FAIL: asmerror0|^  asmerror0$$'; then \
	    echo "FAIL: strict harness did not flag asmerror0 as error-test failing"; \
	    echo "$$strict_out" | tail -20; exit 1; \
	fi; \
	if echo "$$fuzzy_out" | grep -qE 'FAIL: asmerror0'; then \
	    echo "FAIL: fuzzy harness reported asmerror0 FAIL — calibration unsound"; exit 1; \
	fi; \
	if ! echo "$$fuzzy_out" | grep -qE 'PASS:[[:space:]]+61'; then \
	    echo "FAIL: fuzzy harness did not report 61/61 — base shifted"; \
	    echo "$$fuzzy_out" | tail -10; exit 1; \
	fi; \
	echo "verify-phase3-calibration: OK (asmerror0 strict-FAIL + fuzzy-PASS)"

# Pin-update-only regeneration of zxbc parse-only baselines. Manually
# invoked when the upstream Python pin moves; never a dependency of any
# test-* target, so casual `make test` doesn't ever silently shift the
# spec. The script's first action is to run Python's own functional
# test suite as a health-check gate.
regenerate-zxbc-baselines:
	./csrc/tests/regen_zxbc_baselines.sh

test-zxbc-parse-strict: $(ZXBC_C)
	./csrc/tests/run_zxbc_tests_strict.sh $(ZXBC_C) $(ZXBC_TESTS)

test-zxbc-parse-fuzzy: $(ZXBC_C)
	./csrc/tests/run_zxbc_tests.sh $(ZXBC_C) $(ZXBC_TESTS)

# Phase 4 calibration: alxinho1.bas — both Python and C exit 1 on the
# undeclared-array case, so the existing exit-code-only harness reports
# MATCH. But Python says `Undeclared array "a"` while C says `'a' is
# neither an array nor a function.` — different error wording for the
# same root cause. The strict harness's stderr-content equality catches
# this; the existing harness silently passes it.
verify-phase4-calibration: $(ZXBC_C)
	@set -e; \
	strict_out=$$(./csrc/tests/run_zxbc_tests_strict.sh $(ZXBC_C) $(ZXBC_TESTS) tests/functional/arch/zx48k/alxinho1.bas 2>&1 || true); \
	fuzzy_out=$$(./csrc/tests/run_zxbc_tests.sh $(ZXBC_C) $(ZXBC_TESTS) tests/functional/arch/zx48k/alxinho1.bas 2>&1 || true); \
	if ! echo "$$strict_out" | grep -qE 'STDERR_MISMATCH:[[:space:]]+1'; then \
	    echo "FAIL: strict harness did not flag alxinho1 as STDERR_MISMATCH"; \
	    echo "$$strict_out" | tail -10; exit 1; \
	fi; \
	if echo "$$fuzzy_out" | grep -qE 'MISMATCH:.*alxinho1|FALSE_(POS|NEG):.*alxinho1'; then \
	    echo "FAIL: fuzzy harness already flags alxinho1 — calibration unsound"; \
	    echo "$$fuzzy_out" | tail -10; exit 1; \
	fi; \
	if ! echo "$$fuzzy_out" | grep -qE '1 matched, 0 mismatched|matched: 1'; then \
	    echo "FAIL: fuzzy harness did not report alxinho1 MATCH"; \
	    echo "$$fuzzy_out" | tail -10; exit 1; \
	fi; \
	echo "verify-phase4-calibration: OK (alxinho1.bas STDERR_MISMATCH strict + MATCH fuzzy)"

ZXBC_AST_DUMP_C = $(BUILD_DIR)/zxbc-ast-dump/zxbc-ast-dump
PY_AST_DUMP     = csrc/tests/dump_python_ast.py
PY_AST_DIFF     = csrc/tests/diff_ast_json.py
AST_FIXTURE_DIR = csrc/tests/ast_equiv_fixtures

test-zxbc-ast-equiv: $(ZXBC_AST_DUMP_C)
	./csrc/tests/run_zxbc_ast_equiv.sh $(ZXBC_AST_DUMP_C) $(PY_AST_DUMP) $(PY_AST_DIFF) $(ZXBC_TESTS)

# Phase 5 calibration: counts TYPECAST occurrences in each dump
# directly. Drifted-Python must contain TYPECAST (Python's parser
# inserts them for the UINTEGER->BYTE coercion in the FOR bounds);
# drifted-C must contain none (the production-fidelity audit's
# canonical port-drift finding). Faithful fixture must have zero
# TYPECAST on either side.
#
# Why this shape, not "drift mentions TYPECAST in the diff": the diff
# tool stops recursion at parent-level shape mismatches (Python
# optimises declarations out; C keeps them as VARDECL nodes), so the
# TYPECAST nodes deep inside the FOR subtree never get to be the
# reported divergence site. Counting TYPECAST occurrences directly
# isolates the calibration signal from the pervasive shape noise.
verify-phase5-calibration: $(ZXBC_AST_DUMP_C)
	@set -e; \
	tmpdir=$$(mktemp -d); \
	.venv/bin/python $(PY_AST_DUMP) $(AST_FIXTURE_DIR)/binary_expr_faithful.bas > $$tmpdir/py_f.json; \
	$(ZXBC_AST_DUMP_C)             $(AST_FIXTURE_DIR)/binary_expr_faithful.bas > $$tmpdir/c_f.json; \
	.venv/bin/python $(PY_AST_DUMP) $(AST_FIXTURE_DIR)/for_typecast_drifted.bas > $$tmpdir/py_d.json; \
	$(ZXBC_AST_DUMP_C)             $(AST_FIXTURE_DIR)/for_typecast_drifted.bas > $$tmpdir/c_d.json; \
	pyf_tc=$$(grep -c '"tag": "TYPECAST"' $$tmpdir/py_f.json || true); \
	cf_tc=$$(grep -c  '"tag": "TYPECAST"' $$tmpdir/c_f.json  || true); \
	pyd_tc=$$(grep -c '"tag": "TYPECAST"' $$tmpdir/py_d.json || true); \
	cd_tc=$$(grep -c  '"tag": "TYPECAST"' $$tmpdir/c_d.json  || true); \
	rm -rf $$tmpdir; \
	if [ "$$pyf_tc" != "0" ] || [ "$$cf_tc" != "0" ]; then \
	    echo "FAIL: faithful fixture has TYPECAST (py=$$pyf_tc c=$$cf_tc) — calibration unsound"; exit 1; \
	fi; \
	if [ "$$pyd_tc" -le "0" ]; then \
	    echo "FAIL: drifted Python AST has no TYPECAST (py=$$pyd_tc) — fixture broken"; exit 1; \
	fi; \
	if [ "$$cd_tc" -ge "$$pyd_tc" ]; then \
	    echo "FAIL: drifted C AST has $$cd_tc TYPECASTs vs Python's $$pyd_tc — port drift not detectable"; exit 1; \
	fi; \
	echo "verify-phase5-calibration: OK (drifted py=$$pyd_tc TYPECAST vs c=$$cd_tc; faithful py=$$pyf_tc c=$$cf_tc)"
