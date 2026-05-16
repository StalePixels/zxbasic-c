/*
 * passes/functiongraph.h — Phase 2 pass: FunctionGraphVisitor
 *
 * Port of Python src/api/optimize.py:161 FunctionGraphVisitor. Pure
 * symbol-table liveness marking: sets `u.id.accessed = true` on the
 * symbol-table entries of functions/subs/labels reachable from global
 * scope (directly for global-scope calls; transitively through
 * accessed FUNCDECL bodies). Reads the AST, mutates only symbol-entry
 * `accessed` flags — no AST mutation. Consumed downstream by S2.4's
 * OptimizerVisitor and Phase-5 codegen; on its own it changes no meter.
 */
#ifndef ZXBC_PASSES_FUNCTIONGRAPH_H
#define ZXBC_PASSES_FUNCTIONGRAPH_H

#include "zxbc.h"

/* Run FunctionGraphVisitor over `ast`. Idempotent; safe on recursive
 * call graphs (mirrors Python UniqueVisitor's visited-set). */
void functiongraph_run(CompilerState *cs, AstNode *ast);

#endif /* ZXBC_PASSES_FUNCTIONGRAPH_H */
