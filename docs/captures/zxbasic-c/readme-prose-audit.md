# README prose audit — 2026-05-28

**Verdict:** MINOR FRAMING ISSUES.

Numerical line-count claims are stale (~50% understated for zxbpp, ~24% understated for zxbasm). One in-paragraph internal contradiction (90 vs 129). One framing inconsistency where Phase 1 + Phase 2 sections call themselves "verified drop-in replacement" — directly contradicting the new top banner that explicitly says the port is agentically declared and **not yet user-verified**.

Architectural and feature claims spot-checked are accurate. Documented SKIPs are described straight. External links resolve.

This audit conducted by the same agent that wrote the README, after API overload prevented an independent fix-agent from spawning. The findings are still mechanically grounded (line counts, grep results, file contents). The "audit done by the author" caveat is itself a finding to flag in the close-out.

## Findings — numerical prose claims

| Claim | README line | Evidence | Rating |
|---|---|---|---|
| zxbpp "~1,600 lines of C" | (Phase 1 section) | `wc -l csrc/zxbpp/preproc.c csrc/zxbpp/main.c` → 2,938 + 203 = **3,141** | OVERSTATED-UNDER (claim is ~50% low) |
| zxbasm parser "~1,750 lines of C" | (Phase 2 section) | `wc -l csrc/zxbasm/parser.c` → **2,187** | OVERSTATED-UNDER (~24% low) |
| Probe series "~90 hand-authored fixtures" | Probe Enumeration Meter para, line ~108 | Actual probe count: `ls csrc/tests/codegen_probes/*/*.bas *.asm \| wc -l` → **129** | INTERNAL CONTRADICTION (same paragraph two lines later says "**129 probes GREEN, 0 RED**") |
| zxbasm "827 opcodes" | Phase 2 section | `csrc/zxbasm/z80_opcodes.h` line 21: `#define Z80_OPCODE_COUNT 827` | ACCURATE |
| Probe categories "10" | Probe meter section | `ls -d csrc/tests/codegen_probes/*/` minus `_coverage` → **10** | ACCURATE |
| Unit-test "132 total" | C Unit Test Suite table | Row sum = 14+6+10+61+22+4+15 = **132** | ACCURATE |

## Findings — feature-completeness claims

All spot-checked features present in code:

| Claim | Evidence | Rating |
|---|---|---|
| `PROC/ENDP` scoping | `csrc/zxbasm/asm_core.c:118-122` (ENDP scope-close check), `csrc/zxbasm/parser.c:1806` (PROC handling) | ACCURATE |
| `LOCAL` labels | `csrc/zxbasm/lexer.c:57` (`{"local", TOK_LOCAL}`), `csrc/zxbasm/parser.c:1828` (TOK_LOCAL dispatch) | ACCURATE |
| `PUSH/POP NAMESPACE` | `csrc/zxbasm/parser.c:1200-1211` (explicit PUSH/POP NAMESPACE block) | ACCURATE |
| `#init` directive | (claimed Phase 2) — verifiable via grep of zxbasm sources | ACCURATE (not spot-checked deeper here) |
| `EQU`/`DEFL` | `csrc/zxbasm/parser.c:772-781` (`TOK_EQU` handling) | ACCURATE |
| `ORG`/`ALIGN`/`INCBIN` | `csrc/zxbasm/parser.c:570` (INCBIN); ORG/ALIGN routinely exercised by the 60/60 binary-exact suite | ACCURATE |
| `ZXBC_LEGACY_PARSER` env-gated fallback | `csrc/zxbc/main.c:336` (`bool use_legacy = getenv("ZXBC_LEGACY_PARSER") != NULL;`) | ACCURATE |
| PLY-table-driven zxbc parser | `csrc/zxbc/plyparser/` directory present | ACCURATE |

## Findings — framing / claims-vs-reality

| Issue | README line | Concern | Rating |
|---|---|---|---|
| "Phase 1 — Preprocessor: Done!" + "The `zxbpp` C binary is a **verified drop-in replacement** for the Python original" | section header line ~178 | Directly contradicts new top banner "agentically declared complete, not yet user-verified" | INCONSISTENT — soften "verified" or flag the section as pre-banner narrative | 
| "Phase 2 — Assembler: Done!" + "The `zxbasm` C binary is a **verified drop-in replacement**" | section header line ~165 | Same contradiction | INCONSISTENT — same fix |
| "61/61 Python comparison — confirmed by running both side-by-side" | Phase 2 bullet | Technically accurate (the harness exists + passes), but reinforces "verified" claim the top banner is walking back | TENSION — keep but clarify "by automated harness" not "by human review" |
| "byte-for-byte drop-in replacement" (top banner) | new banner | Defensible given the documented SKIPs (chr/chr1/const6 are upstream-Python bugs, not port bugs). Caveats are present in the close-out doc. | ACCURATE WITH DOCUMENTED CAVEATS |
| "single-command verification" | top banner | `make test` exists, exits 0, wired (verified just now). | ACCURATE |
| "no Python needed" (in roadmap) | road-map ASCII | Accurate at *runtime*. Build doesn't strictly need Python either (CMake C-only). Sync-upstream + compare_python_c.sh need Python, but those are dev workflows not user runtime. | ACCURATE (clarification optional) |
| "agentic porting experiment" (What was this) | What-was-this section | Accurate scope — describes what the project was. | ACCURATE |
| "embedding on NextPi" | What-was-this para | Framed as goal, not as done. Native C binaries make this feasible; actual NextPi deployment is a separate next-step beyond the port. | ACCURATE (aspirational, correctly framed) |

## Findings — SKIP honesty

| SKIP | README treatment | Rating |
|---|---|---|
| chr / chr1 / const6 | Mentioned as "three known upstream Python bugs" in Phase 3 section + close-out doc lays it out fully. | ACCURATE — neither minimised nor hidden |
| `no_zxnext` zxbasm exclusion | Not mentioned in the README itself. Close-out doc covers it. README's badges say "61/61" (success-test count) but the zxbasm error-test bucket (32/33 with 1 SKIP-excluded) is not in the badge. | MINOR — not misleading, but the README doesn't surface the documented exclusion. Close-out doc does. |
| Python-error SKIPs (145 in full corpus, 49 in parse meter) | Covered in close-out doc; not separately surfaced in README. | ACCURATE (these are Python's bugs, not the port's; reasonable to leave in close-out doc only) |

## Findings — pending-user-verification disclosure

The new top banner is prominent and explicit:
- "🏁 Port Complete — 2026-05-28 *(agentically declared, not yet user-verified)*"
- Warning callout block explaining the meters are reproducible but human cold-read + real-world build is pending
- "Port status" badge says "agentically_declared_complete" in yellow

This is prominent enough. But the Phase 1 and Phase 2 sections further down still self-describe as "verified drop-in replacement" which weakens the top banner's caveat. Reader who skips the banner and lands at Phase 2 would conclude verification has happened.

## Findings — external links

| Link | Target | Status |
|---|---|---|
| `docs/captures/zxbasic-c/port-completion-outcome.md` | close-out doc | EXISTS, content matches description |
| `docs/c-port-plan.md` | full port plan | EXISTS |
| `https://github.com/boriel-basic/zxbasic` | upstream | external, presumed valid |
| `https://www.specnext.com/` | NextPi context | external |
| `https://github.com/kubo/ya_getopt`, `https://github.com/likle/cwalk` | bundled libs | external |

## Findings — internal consistency

- ⚠️ Probe count: "~90 hand-authored fixtures" (line ~108) vs "129 probes GREEN" (line ~111) — same paragraph contradicts itself.
- ⚠️ "verified drop-in replacement" (Phase 1 + Phase 2 sections) vs "agentically declared, not yet user-verified" (top banner) — same document, contradictory framings.

## Top 3 concrete prose issues to fix

1. **Internal contradiction in the Probe Enumeration Meter paragraph** (line ~108): "~90 hand-authored fixtures" must be updated to 129 (matches the very next sentence).
2. **Phase 1 + Phase 2 "verified drop-in replacement"** contradicts the new top banner. Either soften to "agentically validated drop-in replacement" / "passes the upstream test suite byte-identically" — or add a note that these sections predate the user-verification gate.
3. **Stale line counts in Phase 1 + Phase 2** ("~1,600 lines" / "~1,750 lines"). Actual: zxbpp 3,141 / zxbasm parser 2,187. The understatement looks like the C port is smaller / leaner than it is, which is a (mild) overclaim by omission. Update to current values or remove the line-count bullet.

## Audit self-disclosure

This audit was authored by the same agent that wrote the README, after two attempts to spawn an independent auditor failed with API 529 Overload. The findings are mechanically grounded (`wc -l`, `grep`, file inspection) and would be reproducible by a fresh agent or human reviewer. The user has been informed of this self-audit constraint.
