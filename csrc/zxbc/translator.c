/*
 * AST -> IC translator — calibration slice. See translator.h.
 *
 * Faithfulness: Python's NodeVisitor is a generator trampoline; visit_END
 * `yield`s its child (the trampoline visits it) then calls
 * ic_end(child.t). The observable result is: child visited first, then a
 * single Quad("end", child.t) appended. The C port reproduces that
 * observable IC sequence with an explicit child-visit + emit (the value a
 * Python visit_* `yield`s is not consumed on the calibration path —
 * ic_end reads child.t, which for a NUMBER is str(value)).
 */
#include "translator.h"
#include "visitor.h"
#include "ic.h"

#include <stdio.h>
#include <stdint.h>

/* TranslatorInstVisitor.emit (translator_inst_visitor.py:21-25):
 *   quad = Quad(*args); self.backend.MEMORY.append(quad) */
static void tr_emit(Translator *tr, const char *instr, int nargs,
                    const char *const *args) {
    Quad *q = quad_new(tr->backend->arena, instr, nargs, args);
    vec_push(tr->backend->memory, q);
}

/* ic_end (translator_inst_visitor.py:103-104): self.emit("end", t) */
static void tr_ic_end(Translator *tr, const char *t) {
    const char *args[1] = { t };
    tr_emit(tr, IC_END, 1, args);
}

void translator_ic_inline(Translator *tr, const char *asm_code) {
    /* ic_inline (translator_inst_visitor.py:130-131): emit("inline", code) */
    const char *args[1] = { asm_code };
    tr_emit(tr, IC_INLINE, 1, args);
}

/* visit_NUMBER (translator.py:56-58): yields node.value. Python Symbol
 * NUMBER.t == str(value); make_number() does not set t, so resolve it
 * here using the same format ast_number() uses (ast.c:71-77) so the
 * value read by visit_END is byte-identical. */
static AstNode *tr_visit_number(Visitor *v, AstNode *node) {
    (void)v;
    if (node->t == NULL) {
        double value = node->u.number.value;
        char buf[64];
        if (value == (double)(int64_t)value)
            snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)value);
        else
            snprintf(buf, sizeof(buf), "%g", value);
        Translator *tr = v->ctx;
        node->t = arena_strdup(&tr->cs->arena, buf);
    }
    return node;
}

/* visit_BLOCK (translator_visitor.py:97-100): for child in children: yield
 * child. Faithful analogue: visit each child in order (no tree rewrite —
 * the Translator emits IC as a side effect, it does not replace nodes). */
static AstNode *tr_visit_block(Visitor *v, AstNode *node) {
    for (int i = 0; i < node->child_count; i++)
        visitor_visit(v, node->children[i]);
    return node;
}

/* visit_END (translator.py:65-68): yield child[0]; ic_end(child[0].t). */
static AstNode *tr_visit_end(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *operand = node->child_count > 0 ? node->children[0] : NULL;
    if (operand)
        visitor_visit(v, operand);
    tr_ic_end(tr, (operand && operand->t) ? operand->t : "0");
    return node;
}

void translator_visit(Translator *tr, AstNode *ast) {
    Visitor v;
    visitor_init(&v, tr->cs);
    v.ctx = tr;
    visitor_on_tag(&v, AST_BLOCK, tr_visit_block);
    visitor_on_tag(&v, AST_NUMBER, tr_visit_number);
    visitor_on_sentence(&v, "END", tr_visit_end);
    visitor_visit(&v, ast);
}
