/*
 * passes/optimizer.h — Phase 2 pass 3: OptimizerVisitor
 *
 * Port of Python src/api/optimize.py:198-490 (class OptimizerVisitor).
 * Constant folding / re-association (BINARY), dead-branch elimination
 * (IF/WHILE/FOR), CONSTEXPR unwrap, @-address constant rewrites,
 * dead-function pruning, unused-assignment rewrites and the CHR$ fold.
 *
 * The master O-gate (optimize.py:201-205) makes the whole pass a no-op
 * when optimization_level < 1 — every node is returned unvisited. The
 * five diagnostics in the class body are all warning_* (never error()),
 * so this pass is meter-neutral (test-zxbc-parse exit-code delta = 0);
 * its value is faithful AST shaping for Phase-5 codegen. See the
 * authoritative port map docs/captures/zxbasic-c/phase2-optimizer-port.md
 * (Q-A, §2) and the .c file's per-handler fidelity notes.
 */
#ifndef ZXBC_PASSES_OPTIMIZER_H
#define ZXBC_PASSES_OPTIMIZER_H

#include "zxbc.h"

/* Run OptimizerVisitor over `ast`. No-op when
 * cs->opts.optimization_level < 1 (Python visit override). Idempotent;
 * mutates the AST in place (folds, prunes, replaces subtrees). */
void optimizer_run(CompilerState *cs, AstNode *ast);

#endif /* ZXBC_PASSES_OPTIMIZER_H */
