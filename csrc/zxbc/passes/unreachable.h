/*
 * passes/unreachable.h — Phase 2 pass: UnreachableCodeVisitor
 *
 * Port of Python src/api/optimize.py:92-158 (UnreachableCodeVisitor).
 * Prunes provably-unreachable statements after a block "ender", scrubs
 * CHKBREAK after LABEL/RETURN, collapses recursively-empty blocks to
 * NOP (O>=1), and emits the W190 "function should return a value" /
 * W180 "unreachable code" diagnostics. Both diagnostics are WARNINGS
 * (Python @register_warning) — they never set has_errors and do not
 * change --parse-only's exit code, so this pass is meter-neutral
 * (test-zxbc-parse delta = 0); its value is faithful AST shaping for
 * the downstream passes / Phase-5 codegen.
 */
#ifndef ZXBC_PASSES_UNREACHABLE_H
#define ZXBC_PASSES_UNREACHABLE_H

#include "zxbc.h"

/* Run UnreachableCodeVisitor over `ast`. Idempotent; mutates the AST
 * in place (prunes children, may replace an empty block with NOP). */
void unreachable_run(CompilerState *cs, AstNode *ast);

#endif /* ZXBC_PASSES_UNREACHABLE_H */
