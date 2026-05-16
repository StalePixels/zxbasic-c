/*
 * AST -> IC translator (the S5.2 calibration slice).
 *
 * Port of, for the calibration construct only:
 *   src/arch/z80/visitor/translator_visitor.py  visit_BLOCK   (:97-100)
 *   src/arch/z80/visitor/translator.py          visit_END     (:65-68)
 *                                               visit_NUMBER  (:56-58)
 *   src/arch/z80/visitor/translator_inst_visitor.py emit/ic_end/ic_inline
 *                                               (:21-25, 103-104, 130-131)
 *
 * Runs AFTER the optimiser passes (main.c:116 visitor_run_passes), so for
 * `calib.bas` the tree is BLOCK[ BLOCK(empty, was LET), SENTENCE(END)[NUMBER 0] ]
 * (opt_visit_let already pruned the unused LET). The only program-derived
 * IC is Quad("end","0"); the driver then appends Quad("inline", marker).
 *
 * VarTranslator / data_ast is intentionally NOT here: for the degenerate
 * O2 calibration the unused `a` is dropped (VarTranslator.visit_VARDECL
 * early-returns at O>1) so the data_ast contributes zero bytes; the
 * ordered symbol-table drain + the ic_var/ic_vard storage emitters are
 * S5.3 (the "8/16-bit integer scalar" CR), where they are first exercised.
 */
#ifndef ZXBC_TRANSLATOR_H
#define ZXBC_TRANSLATOR_H

#include "zxbc.h"
#include "backend.h"

typedef struct Translator {
    CompilerState *cs;
    Backend       *backend;
} Translator;

/* zxbc.py:125-126  translator.visit(zxbparser.ast) — appends Quads to
 * backend->memory. `ast` must already be optimiser-processed. */
void translator_visit(Translator *tr, AstNode *ast);

/* zxbc.py:150  translator.ic_inline(";; --- end of user code ---"). */
void translator_ic_inline(Translator *tr, const char *asm_code);

#endif /* ZXBC_TRANSLATOR_H */
