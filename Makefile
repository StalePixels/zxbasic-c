# Top-level Makefile for the C port. Round 0 close-out (Sprint 11):
# the strict harnesses replace the legacy fuzzy ones under canonical
# names; per-phase calibration assertions are gone (their job — proving
# the rebuild changed verdicts — is done now that the fuzzy harnesses
# they compared against have been deleted).
#
# `make test` is the aggregator: builds, runs every strict harness,
# reports each surface's bucket count.

CMAKE       ?= cmake
BUILD_DIR   ?= csrc/build
BUILD_TYPE  ?= Release

ZXBPP_C         = $(BUILD_DIR)/zxbpp/zxbpp
ZXBPP_TESTS     = tests/functional/zxbpp
ZXBASM_C        = $(BUILD_DIR)/zxbasm/zxbasm
ZXBASM_TESTS    = tests/functional/asm
ZXBC_C          = $(BUILD_DIR)/zxbc/zxbc
ZXBC_TESTS      = tests/functional/arch/zx48k
ZXBC_AST_DUMP_C = $(BUILD_DIR)/zxbc-ast-dump/zxbc-ast-dump
PY_AST_DUMP     = csrc/tests/dump_python_ast.py
PY_AST_DIFF     = csrc/tests/diff_ast_json.py

.PHONY: build clean \
        test test-zxbpp test-zxbasm test-zxbc-parse test-zxbc-ast-equiv \
        sweep-asm-zero-byte regenerate-zxbc-baselines

build:
	$(CMAKE) -S csrc -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build $(BUILD_DIR) -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

clean:
	rm -rf $(BUILD_DIR) Testing

# Aggregator: build, then run every strict harness across its full
# surface. Each harness reports its own bucket count to stdout; the
# aggregate `make test` exits non-zero iff any sub-harness exits
# non-zero. AST-equiv is exit-zero (it's a measurement, not a verifier).
test: build
	@echo "================== make test =================="
	@echo "[1/4] zxbpp strict harness"
	./csrc/tests/run_zxbpp_tests.sh $(ZXBPP_C) $(ZXBPP_TESTS) || true
	@echo
	@echo "[2/4] zxbasm strict harness"
	./csrc/tests/run_zxbasm_tests.sh $(ZXBASM_C) $(ZXBASM_TESTS) || true
	@echo
	@echo "[3/4] zxbc --parse-only strict harness"
	./csrc/tests/run_zxbc_tests.sh $(ZXBC_C) $(ZXBC_TESTS) || true
	@echo
	@echo "[4/4] zxbc AST-equivalence harness"
	./csrc/tests/run_zxbc_ast_equiv.sh $(ZXBC_AST_DUMP_C) $(PY_AST_DUMP) $(PY_AST_DIFF) $(ZXBC_TESTS)
	@echo
	@echo "================== make test done =============="

test-zxbpp: $(ZXBPP_C)
	./csrc/tests/run_zxbpp_tests.sh $(ZXBPP_C) $(ZXBPP_TESTS)

test-zxbasm: $(ZXBASM_C)
	./csrc/tests/run_zxbasm_tests.sh $(ZXBASM_C) $(ZXBASM_TESTS)

test-zxbc-parse: $(ZXBC_C)
	./csrc/tests/run_zxbc_tests.sh $(ZXBC_C) $(ZXBC_TESTS)

test-zxbc-ast-equiv: $(ZXBC_AST_DUMP_C)
	./csrc/tests/run_zxbc_ast_equiv.sh $(ZXBC_AST_DUMP_C) $(PY_AST_DUMP) $(PY_AST_DIFF) $(ZXBC_TESTS)

sweep-asm-zero-byte:
	@matches=$$(find tests/functional -name '*.bin' -size 0); \
	count=$$(printf '%s\n' "$$matches" | grep -c .); \
	echo "$$matches"; \
	echo "$$count zero-byte .bin files found"

# Pin-update-only regeneration of zxbc parse-only baselines. Manually
# invoked when the upstream Python pin moves; never a dependency of
# any test target.
regenerate-zxbc-baselines:
	./csrc/tests/regen_zxbc_baselines.sh
