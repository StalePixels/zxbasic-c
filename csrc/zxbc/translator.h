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

/* LOOPS entry (translator.py:1009-1027): (loop_type, end_label,
 * continue_label). loop_type is "FOR"/"WHILE"/"DO". Resolution scans
 * inner->outer (reverse). Python class-level mutable; C port is
 * Translator-scoped, cleared per translator_visit (mirrors reset()). */
typedef struct LoopEntry {
    const char *kind;     /* "FOR" / "WHILE" / "DO" */
    const char *end_label;
    const char *cont_label;
} LoopEntry;

/* JumpTable (translator_visitor.py:160-162 + JumpTable dataclass): one
 * ON GOTO/GOSUB jump table; emitted by emit_jump_tables as
 * ic_vard(label, ["#count"] + ["##mangled" ...]). */
typedef struct JumpTableEntry {
    const char *label;          /* the tmp_label() table label */
    AstNode   **addresses;      /* the target LABEL id nodes (children[1:]) */
    int         naddresses;
} JumpTableEntry;

typedef struct Translator {
    CompilerState *cs;
    Backend       *backend;

    /* TranslatorVisitor class state (translator_visitor.py:43-61). Python
     * keeps these class-level + clears them in reset(); the C port scopes
     * them to the Translator and clears them at the top of
     * translator_visit (the faithful reset() analogue). */
    LoopEntry       loops[64];   /* the LOOPS stack (translator.py:1009) */
    int             loops_len;
    JumpTableEntry  jump_tables[64];
    int             jump_tables_len;
    const char     *prev_token;  /* PREV_TOKEN (visit_CHKBREAK guard) */
    const char     *curr_token;  /* CURR_TOKEN */

    /* S5.7a — pending-function queue, the gl.FUNCTIONS analogue
     * (global_.py:109). Translator.visit_FUNCDECL (translator.py:196-198)
     * enqueues top-level FUNCDECLs during the main walk (deferred, no
     * inline emission); FunctionTranslator.start()
     * (function_translator.py:43-47) FIFO-drains; nested FUNCDECLs found
     * mid-drain are appended via FunctionTranslator.visit_FUNCDECL
     * (:174-176). A flat work-list, not recursion. The C queue holds the
     * AST_FUNCDECL nodes (child[0]=entry ID, child[1]=PARAMLIST,
     * child[2]=body) — Python aliases gl.FUNCTIONS with the entries whose
     * .ref carries body/params; the C FUNCDECL node is the equivalent
     * carrier. */
    AstNode       **pending_funcs;
    int             pending_len;
    int             pending_cap;
    int             pending_head;   /* FIFO read cursor (pop(0)) */

    /* S5.7b — processed-function guard (node identity on the FUNCTION
     * *entry* ID). Python's invariant: each function body is emitted
     * exactly once — `p_funcdeclforward` (zxbparser.py:2918-2930) returns
     * None for a `DECLARE`, so a forward declaration produces NO FUNCDECL
     * node and only the real definition's FUNCDECL ever reaches
     * gl.FUNCTIONS. The C parser, by contrast, emits a (empty-body)
     * FUNCDECL for the `DECLARE` *and* one for the definition, both
     * carrying the *same shared entry ID node*. Draining both would emit
     * the function label twice and, worse, any caller/queue re-entry on
     * the shared entry is unbounded. This set keys on the entry ID node
     * so visit_FUNCTION runs at most once per function — faithfully
     * reproducing Python's "each FUNCDECL processed once" without
     * changing the parser's AST shape. */
    AstNode       **emitted_funcs;
    int             emitted_len;
    int             emitted_cap;
} Translator;

/* zxbc.py:125-126  translator.visit(zxbparser.ast) — appends Quads to
 * backend->memory. `ast` must already be optimiser-processed. */
void translator_visit(Translator *tr, AstNode *ast);

/* zxbc.py:150  translator.ic_inline(";; --- end of user code ---"). */
void translator_ic_inline(Translator *tr, const char *asm_code);

/* emit_jump_tables (translator_visitor.py:160-162; zxbc.py:148): drains
 * the ON GOTO/GOSUB JUMP_TABLES into ic_vard quads. Must run after
 * translator_visit, before ic_inline(end-of-user-code). No-op when no
 * ON GOTO/GOSUB appeared. */
void translator_emit_jump_tables(Translator *tr);

/* S5.7a — FunctionTranslator(...).start() (function_translator.py:43-47;
 * zxbc.py:132-133). FIFO-drains the pending-function queue filled by
 * translator_visit's deferred visit_FUNCDECL, emitting each function's
 * prologue (label + ic_enter) / body / epilogue (__leave label +
 * ic_leave). Nested FUNCDECLs encountered while walking a body are
 * appended to the same queue mid-drain. Must run AFTER translator_visit
 * (mirrors zxbc.py: Translator.visit then FunctionTranslator.start).
 * Core slice: no params/ABI, no local arrays, no stdcall teardown
 * (S5.7b/c). No-op when no functions were declared. */
void translator_function_start(Translator *tr);

/* ---- S5.3 shared TranslatorInstVisitor surface (var_translator.c) ---- */

/* TranslatorInstVisitor.emit (translator_inst_visitor.py:21-25):
 *   Quad(*args); backend.MEMORY.append(quad). args already string-coerced
 *   by the caller (mirrors Quad.__init__'s str() of each arg). */
void tr_emit_quad(Translator *tr, const char *instr, int nargs,
                  const char *const *args);

/* TranslatorInstVisitor.TSUFFIX (translator_inst_visitor.py:27-51):
 * a TYPE/TYPEREF/BASICTYPE -> the DataType string ("u8","i8","u16",...).
 * Returns an arena-stable static string literal. */
const char *tr_tsuffix(const TypeInfo *type_);

/* Translator.default_value (translator.py:1029-1078): the list of 2-hex
 * byte/word strings an initialized scalar compiles to. Returns the count
 * and writes arena-owned strings into *out (caller passes a buffer big
 * enough — max 5 for float). Faithful for the S5.3 integer-scalar path;
 * CONSTEXPR/float/fixed branches ported verbatim. */
int tr_default_value(Translator *tr, const TypeInfo *type_,
                     AstNode *expr, char **out, int out_cap);

/* VarTranslator over data_ast (var_translator.py whole file). Visits the
 * BLOCK of VARDECL(entry) appending the data-space Quads (var/vard/varx/
 * deflabel/label). Mirrors zxbc.py:196-199. */
void var_translator_visit(Translator *tr, AstNode *data_ast);

#endif /* ZXBC_TRANSLATOR_H */
