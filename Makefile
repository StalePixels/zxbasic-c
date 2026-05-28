# Top-level Makefile for the C port — port-completion umbrella
# (S7.3f / CR#5). Two-tier design:
#
#   make test       — fast tier (≈5 min), the routine green-light gate.
#   make test-slow  — deep tier (≈18 min) including byte-for-byte
#                     full-corpus + 3-stage gated pipeline + -O matrix
#                     on both archs.
#
# See the `test:` target below for the wiring detail and exit semantics.

CMAKE       ?= cmake
BUILD_DIR   ?= csrc/build
BUILD_TYPE  ?= Release

# All applet entries (zxbpp / zxbasm / zxbc) live under $(BUILD_DIR)/bin/
# as symlinks (Unix) or copies (Windows) of the multicall zxbasic-suite
# binary. The harnesses are name-agnostic and take a binary path as argv.
ZXBPP_C            = $(BUILD_DIR)/bin/zxbpp
ZXBPP_TESTS        = tests/functional/zxbpp
ZXBASM_C           = $(BUILD_DIR)/bin/zxbasm
ZXBASM_TESTS       = tests/functional/asm
ZXBC_C             = $(BUILD_DIR)/bin/zxbc
ZXBC_TESTS         = tests/functional/arch/zx48k
ZXBC_TESTS_ZXNEXT  = tests/functional/arch/zxnext
ZXBC_AST_DUMP_C    = $(BUILD_DIR)/bin/zxbc-ast-dump
PY_AST_DUMP        = csrc/tests/dump_python_ast.py
PY_AST_DIFF        = csrc/tests/diff_ast_json.py
CHECK_METER        = csrc/tests/check_meter_green.sh
PROBE_RUN          = csrc/tests/codegen_probes/run_probes.sh
# Ten hand-authored probe categories under csrc/tests/codegen_probes/.
# _coverage/ is tooling (the in-process cov driver), not a test category.
PROBE_CATEGORIES   = arithmetic arrays controlflow errors preprocessor \
                     strings switches typecast warnings zxbasm

.PHONY: build clean \
        test test-fast test-slow \
        test-zxbpp test-zxbasm test-zxbc-parse test-zxbc-ast-equiv \
        test-semantic-fidelity verify-phase1-calibration \
        test-zxbc-codegen verify-phase5-calibration \
        test-zxbc-outfmt verify-phase6-calibration \
        test-zxbc-full verify-phase7-calibration \
        test-zxbc-stages test-zxbc-stages-zxnext \
        test-zxbc-omatrix test-zxbc-omatrix-zxnext \
        test-codegen-probes \
        test-unit \
        test-cmdline-parity \
        sweep-asm-zero-byte regenerate-zxbc-baselines

build:
	$(CMAKE) -S csrc -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	$(CMAKE) --build $(BUILD_DIR) -j$$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

clean:
	rm -rf $(BUILD_DIR) Testing

# Aggregator (port-completion gate, S7.3f / CR#5). Two tiers:
#
#   make test       — the FAST tier, the routine green-light gate.
#                     zxbpp, zxbasm, zxbc parse, zxbc codegen, the 10
#                     codegen-probe categories, the C unit tests. Each
#                     exits non-zero (or, for the exit-0-always
#                     measurement harnesses, is wrapped by
#                     csrc/tests/check_meter_green.sh) so `make test`
#                     itself exits non-zero on any regression. Wall-clock
#                     ≈ 5 min on a recent-vintage workstation, dominated
#                     by the codegen meter (3m15s).
#
#   make test-slow  — `make test` PLUS the deep byte-for-byte meters:
#                     zxbc full corpus (zx48k), the gated 3-stage
#                     validation (zx48k + zxnext), and the -O matrix
#                     sweep (zx48k + zxnext). Wall-clock dominated by
#                     the stage-validation harness's 3× pipeline runs.
#                     Run pre-bank for the final close-out and as part
#                     of CI's nightly / pre-release gate.
#
# NOT in either tier:
#   csrc/scripts/nextbuild-sweep.sh — local-only discovery harness,
#     depends on `_ref/NextBuild/` checkout which CI doesn't have.
#     Documented in docs/captures/zxbasic-c/port-completion-outcome.md.
#
# Wiring rule: every harness that exits 0 regardless of divergences
# (zxbc-full, zxbc-stages, zxbc-omatrix, codegen probes) goes through
# csrc/tests/check_meter_green.sh, which parses the printed bucket
# summary and exits non-zero iff any divergence bucket is non-zero.
# Harnesses that already exit non-zero on FAIL (zxbpp, zxbasm,
# zxbc-parse, zxbc-codegen, the unit tests) are invoked directly.

test: test-fast

test-fast: build
	@echo "================== make test (fast tier) =================="
	@start=$$(date +%s); \
	./csrc/tests/run_zxbpp_tests.sh $(ZXBPP_C) $(ZXBPP_TESTS) && \
	echo && \
	./csrc/tests/run_zxbasm_tests.sh $(ZXBASM_C) $(ZXBASM_TESTS) && \
	echo && \
	./csrc/tests/run_zxbc_tests.sh $(ZXBC_C) $(ZXBC_TESTS) && \
	echo && \
	./csrc/tests/run_zxbc_codegen_tests.sh $(ZXBC_C) $(ZXBC_TESTS) && \
	echo && \
	$(MAKE) --no-print-directory test-codegen-probes && \
	echo && \
	$(MAKE) --no-print-directory test-unit && \
	echo && \
	end=$$(date +%s); \
	elapsed=$$((end - start)); \
	echo "================== make test (fast tier) GREEN — $${elapsed}s =================="

test-slow: test-fast
	@echo
	@echo "================== make test-slow (deep tier) =================="
	@start=$$(date +%s); \
	$(CHECK_METER) zxbc-full full -- \
	    ./csrc/tests/run_zxbc_full_tests.sh $(ZXBC_C) $(ZXBC_TESTS) && \
	echo && \
	$(CHECK_METER) zxbc-stages-zx48k stages -- \
	    ./csrc/tests/run_zxbc_stage_validation.sh $(ZXBC_C) $(ZXBASM_C) $(ZXBC_TESTS) && \
	echo && \
	$(CHECK_METER) zxbc-stages-zxnext stages -- \
	    ./csrc/tests/run_zxbc_stage_validation.sh $(ZXBC_C) $(ZXBASM_C) $(ZXBC_TESTS_ZXNEXT) && \
	echo && \
	$(CHECK_METER) zxbc-omatrix-zx48k omatrix-zx48k -- \
	    ./csrc/tests/run_zxbc_omatrix.sh $(ZXBC_C) $(ZXBC_TESTS) && \
	echo && \
	$(CHECK_METER) zxbc-omatrix-zxnext omatrix-zxnext -- \
	    ./csrc/tests/run_zxbc_omatrix.sh $(ZXBC_C) $(ZXBC_TESTS_ZXNEXT) && \
	echo && \
	end=$$(date +%s); \
	elapsed=$$((end - start)); \
	echo "================== make test-slow GREEN — deep tier $${elapsed}s =================="

# Codegen probes — 10 hand-authored probe categories. Each category is
# its own bucket sub-meter; the per-category script exits 0 always so
# we wrap each in check_meter_green.sh (kind=probes) to gate on
# PROBE-DIFF-* non-zero. Loop is shell-driven so first RED stops the
# umbrella with a non-zero exit.
test-codegen-probes: $(ZXBC_C) $(ZXBASM_C) $(ZXBPP_C)
	@for cat in $(PROBE_CATEGORIES); do \
	    $(CHECK_METER) "probes-$$cat" probes -- $(PROBE_RUN) "$$cat" || exit 1; \
	    echo; \
	done

# C unit tests (test_utils, test_config, test_types, test_ast,
# test_symboltable, test_check, test_cmdline) — all CTest-registered.
# Drive via ctest from the build dir so the same harness CI uses runs.
test-unit: build
	@cd $(BUILD_DIR) && ctest --output-on-failure

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

# Phase 6 outfmt (container) meter (S6.1). Measurement — the five-bucket
# count IS the meter; the SKIP_C_ERROR -> BYTE_EQUAL transition across
# S6.2->S6.6 is the red/green. All-RED by design at the Phase-6 entry
# (C has no container generator — codegen.c writes asm text into the
# container path). FALSE_POS must stay 0. Staged outside `make test`,
# same as the Phase-5 codegen meter at its S5.1.
test-zxbc-outfmt: $(ZXBC_C)
	./csrc/tests/run_zxbc_outfmt_tests.sh $(ZXBC_C) $(ZXBC_TESTS)

# Phase 6 named calibration gate (.tap byte-equivalence). Verifier —
# exits non-zero on drift/absence. RED at the Phase-6 entry baseline
# (C emits asm text into the .tap path), GREEN once the C .tap
# generator reproduces Python's tape bytes for the calibration fixture.
verify-phase6-calibration: $(ZXBC_C)
	./csrc/tests/run_phase6_calibration.sh $(ZXBC_C)

# Phase 7 end-to-end full-equivalence meter (S7.3a, plan Phase-7 CR#4).
# Measurement — the five-bucket count IS the port-complete meter; per
# .bas, Python full-compile vs C full-compile (default .bin pipeline,
# no --output-format / no --parse-only), exit code + every emitted
# artefact byte-compared. SKIP-C-error is the loud S7.3 backlog driver
# (never a legitimate terminal state), not a hiding skip. Exit 0 always
# (like the codegen/outfmt meters); the verdict is the printed counts.
# NOT wired into the `make test` aggregate — that is S7.3f/CR#5.
test-zxbc-full: $(ZXBC_C)
	./csrc/tests/run_zxbc_full_tests.sh $(ZXBC_C) $(ZXBC_TESTS)

# Phase 7 named calibration gate (end-to-end .bin byte-equivalence).
# Verifier — exits non-zero on drift/absence. GREEN at this commit
# (the full default pipeline reproduces Python's .bin byte-identically
# for the minimal typed calibration fixture); a RED here means the
# end-to-end pipeline regressed for a simple typed program.
verify-phase7-calibration: $(ZXBC_C)
	./csrc/tests/run_phase7_calibration.sh $(ZXBC_C)

# WIRED-IN multi-stage byte-identical meter (user-directed 2026-05-19).
# Byte-identical is a GATED pipeline, not isolated stages: Stage 1
# codegen ASM == Python; Stage 2 assemble THAT asm, C-bin == Python-bin
# (GATED on S1-EQUAL — Stage 2 can never pass if Stage 1 fails); Stage 3
# end-to-end default pipeline (GATED on S2-EQUAL). This makes the prior
# metrics oversight — codegen meter green while the assembled binary
# diverged, because nothing gated the composition — structurally
# impossible. Port-complete requires every stage green, gated.
test-zxbc-stages: $(ZXBC_C) $(ZXBASM_C)
	./csrc/tests/run_zxbc_stage_validation.sh $(ZXBC_C) $(ZXBASM_C) $(ZXBC_TESTS)

# zxnext stage-validation companion. Same harness, the Spectrum Next
# corpus. Gated GREEN at port-completion (197/197/197). Standalone
# target — wired into `make test-slow`, not `make test`.
test-zxbc-stages-zxnext: $(ZXBC_C) $(ZXBASM_C)
	./csrc/tests/run_zxbc_stage_validation.sh $(ZXBC_C) $(ZXBASM_C) $(ZXBC_TESTS_ZXNEXT)

# -O matrix sweep (S7.3d-9 harness). Both corpora.
test-zxbc-omatrix: $(ZXBC_C)
	./csrc/tests/run_zxbc_omatrix.sh $(ZXBC_C) $(ZXBC_TESTS)

test-zxbc-omatrix-zxnext: $(ZXBC_C)
	./csrc/tests/run_zxbc_omatrix.sh $(ZXBC_C) $(ZXBC_TESTS_ZXNEXT)

# S7.2g end-to-end CLI parity gate. Mechanises the C-vs-Python flag
# equivalence hand-verified per sub-slice across S7.2a–f (zxbc/zxbpp/
# zxbasm: output-format selection, deprecated-flag WARNINGs, tape-
# append, save-config, the argparse-faithful validation/mutex/format
# gates, --opt-strategy, the zxbpp/zxbasm flag-rejection narrowing).
# Verifier — invokes BOTH the C binary and python3.12 on the same argv
# + fixture and compares exit code / error-message content / output
# bytes; exits non-zero iff any case FAILs. Complementary byte/exit
# gate to the fast struct-level cmdline_value_tests (test_cmdline.c,
# untouched). NOT wired into the `make test` aggregate — S7.3 owns the
# `make test` completion meter.
test-cmdline-parity: $(ZXBC_C) $(ZXBPP_C) $(ZXBASM_C)
	./csrc/tests/run_cmdline_parity.sh $(ZXBC_C) $(ZXBPP_C) $(ZXBASM_C) .

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
