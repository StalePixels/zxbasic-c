/*
 * Codegen driver — port of the post-passes / `--output-format=asm` path of
 * src/zxbc/zxbc.py main() (zxbc.py:125-214) + output() (zxbc.py:45-68) +
 * the O<=2 Optimizer.optimize (optimizer/main.py:86-200) for the S5.2
 * calibration. main.c calls this in place of the codegen stub.
 *
 * Scope (S5.2, recorded in the tracker): the calibration construct only.
 * FunctionTranslator / emit_data_blocks / emit_strings / emit_jump_tables
 * are genuine no-ops here (no functions/DATA/strings/jumptables in the
 * degenerate program); the zxbpp ASM-mode re-filter + get_inits are
 * identity for directive-free asm (calib emits none — REQUIRES empty).
 * Those become real work in later sprints (functions S5.5+, strings/DATA
 * S5.8, runtime #include/REQUIRES S7); deferral is documented, not silent.
 */
#ifndef ZXBC_CODEGEN_H
#define ZXBC_CODEGEN_H

#include "zxbc.h"

/* Drive AST -> .asm and write cs->opts.output_filename. Returns 0 on
 * success (mirrors gl.has_errors). Precondition: not parse_only, rc==0,
 * passes already run (main.c:116). */
int codegen_emit(CompilerState *cs, AstNode *ast);

#endif /* ZXBC_CODEGEN_H */
