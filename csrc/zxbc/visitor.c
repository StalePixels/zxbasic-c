/*
 * visitor.c — AST visitor dispatch framework (Phase 2)
 *
 * See visitor.h. Faithful port of the Python NodeVisitor model
 * (src/ast/ast.py:28) and GenericVisitor.generic_visit
 * (src/api/optimize.py:72-76): tag-keyed function-pointer dispatch with
 * an AST_SENTENCE kind sub-dispatch, generic recursive post-order
 * descent with child write-back, optional pre/post hooks.
 *
 * S2.1 ships the framework only; it is inert (no pass registers a
 * handler, and visitor_run_passes is an empty wire-in point) until the
 * three passes land in S2.2-S2.4.
 */
#include "visitor.h"

#include <string.h>

void visitor_init(Visitor *v, CompilerState *cs) {
    memset(v, 0, sizeof(*v));
    v->cs = cs;
}

void visitor_on_tag(Visitor *v, AstTag tag, VisitFn fn) {
    if (tag >= 0 && tag < AST_COUNT)
        v->tag_handlers[tag] = fn;
}

void visitor_on_sentence(Visitor *v, const char *kind, VisitFn fn) {
    if (!kind)
        return;
    /* Replace if already registered (last registration wins, mirroring
     * Python method override). */
    for (int i = 0; i < v->sentence_count; i++) {
        if (strcmp(v->sentence_kinds[i], kind) == 0) {
            v->sentence_handlers[i] = fn;
            return;
        }
    }
    if (v->sentence_count >= VISITOR_MAX_SENTENCE_HANDLERS)
        return; /* cap is generous; silently ignoring keeps the
                 * framework total — a pass needing >32 sentence kinds
                 * would be a design smell to surface, not a crash. */
    v->sentence_kinds[v->sentence_count] = kind;
    v->sentence_handlers[v->sentence_count] = fn;
    v->sentence_count++;
}

static VisitFn resolve_handler(Visitor *v, AstNode *node) {
    if (node->tag == AST_SENTENCE && node->u.sentence.kind) {
        for (int i = 0; i < v->sentence_count; i++) {
            if (strcmp(v->sentence_kinds[i], node->u.sentence.kind) == 0)
                return v->sentence_handlers[i];
        }
    }
    if (node->tag >= 0 && node->tag < AST_COUNT)
        return v->tag_handlers[node->tag];
    return NULL;
}

AstNode *visitor_generic(Visitor *v, AstNode *node) {
    if (!node)
        return NULL;
    /* Visit children in order, writing the (possibly replaced) child
     * back — Python: `node.children[i] = yield self.visit(child)`.
     * A NULL result is stored as-is (Python keeps None in the list;
     * downstream walkers NULL-skip). Post-order: the node is returned
     * only after its children are visited/rewritten. */
    for (int i = 0; i < node->child_count; i++)
        node->children[i] = visitor_visit(v, node->children[i]);
    return node;
}

AstNode *visitor_visit(Visitor *v, AstNode *node) {
    if (!node)
        return NULL;

    if (v->pre_hook)
        v->pre_hook(v, node);

    VisitFn fn = resolve_handler(v, node);
    AstNode *result = fn ? fn(v, node) : visitor_generic(v, node);

    if (v->post_hook && result)
        result = v->post_hook(v, result);

    return result;
}

void visitor_run_passes(CompilerState *cs, AstNode *ast) {
    /* S2.1 (Phase 2 groundwork): inert wire-in point. The three passes
     * — UnreachableCodeVisitor, FunctionGraphVisitor, OptimizerVisitor
     * (Python src/api/optimize.py:92/161/198, run in that order per
     * src/zxbc/zxbc.py:107-141) — are registered and invoked here in
     * S2.2-S2.4. Until then this is a no-op so the framework is inert
     * and every prior meter stays byte-identical. */
    (void)cs;
    (void)ast;
}
