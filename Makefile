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

.PHONY: build clean test-zxbpp-strict test-zxbpp-fuzzy verify-phase1-calibration verify-phase2-calibration test-zxbasm-strict test-zxbasm-fuzzy sweep-asm-zero-byte verify-phase3-calibration

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
