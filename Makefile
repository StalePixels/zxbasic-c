# Top-level Makefile for the C port (Round 0 test-infra rebuild).
# Thin entry layer: defers to CMake for builds; harness shell scripts
# land target by target as later sprints add them.

CMAKE       ?= cmake
BUILD_DIR   ?= csrc/build
BUILD_TYPE  ?= Release

ZXBPP_C     = $(BUILD_DIR)/zxbpp/zxbpp
ZXBPP_TESTS = tests/functional/zxbpp

.PHONY: build clean test-zxbpp-strict test-zxbpp-fuzzy verify-phase1-calibration

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
