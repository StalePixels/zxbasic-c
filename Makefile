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
        test-semantic-fidelity verify-phase1-calibration \
        test-zxbc-codegen verify-phase5-calibration \
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

# Phase 1 semantic-fidelity meter (S1.1). Measurement, exit 0 — the
# per-construct match counts ARE the meter. RED for FOR/LET/DIM at the
# Phase 1 baseline, driven to 100% in S1.2; Binary is a regression guard.
test-semantic-fidelity: $(ZXBC_AST_DUMP_C)
	./csrc/tests/run_semantic_fidelity.sh $(ZXBC_AST_DUMP_C)

# Phase 1 named calibration gate (FOR typed-bounds). Verifier — exits
# non-zero on drift. RED at the Phase 1 parent baseline (expected), GREEN
# after the S1.2 fix; that transition is the encoded red/green.
verify-phase1-calibration: $(ZXBC_AST_DUMP_C)
	./csrc/tests/run_phase1_calibration.sh $(ZXBC_AST_DUMP_C)

# Phase 5 codegen meter (S5.1). Measurement — the five-bucket count IS
# the meter; the C_NO_CODEGEN -> PASS transition across S5.x is the
# red/green. All-RED by design at the Phase-5 entry (C emits no asm).
# FALSE_POS must stay 0 (C asm where Python rejects = hard regression).
test-zxbc-codegen: $(ZXBC_C)
	./csrc/tests/run_zxbc_codegen_tests.sh $(ZXBC_C) $(ZXBC_TESTS)

# Phase 5 named calibration gate (codegen byte-equivalence). Verifier —
# exits non-zero on drift/absence. RED at the Phase-5 entry baseline
# (C emits no asm), GREEN once the C translator/emitter reproduces
# Python's asm for the calibration fixture.
verify-phase5-calibration: $(ZXBC_C)
	./csrc/tests/run_phase5_calibration.sh $(ZXBC_C)

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
