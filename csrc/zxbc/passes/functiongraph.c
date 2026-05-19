/*
 * passes/functiongraph.c — Phase 2 pass: FunctionGraphVisitor
 *
 * Faithful port of Python src/api/optimize.py:161-195
 * (FunctionGraphVisitor) on the S2.1 visitor framework.
 *
 * Python keys handlers off get_parent(FUNCDECL) to ask "is this node at
 * global scope?". The C AST does not link AstNode.parent (ast_add_child
 * never sets it), so a parent walk would always say "global" and
 * over-mark. Instead the global-scope predicate is computed during the
 * walk via a FUNCDECL-depth counter in the visitor ctx — semantically
 * equivalent to get_parent(FUNCDECL) is None (depth == 0), and
 * independent of the unimplemented parent linkage. (The absent
 * AstNode.parent is a wider fidelity gap; S2.4's OptimizerVisitor uses
 * get_parent heavily and must confront it then — out of S2.2 scope.)
 *
 * Idempotence (Python UniqueVisitor, optimize.py:79-89) is required for
 * recursive call graphs: a visited AstNode* set guards the FUNCDECL/
 * FUNCCALL/CALL cycle re-entry points. The inner call-collection walk
 * carries its own visited set (Python filter_inorder, ast.py:63-69).
 */
#include "passes/functiongraph.h"
#include "visitor.h"
#include "vec.h"

#include <stdbool.h>

/* One named vector type — VEC(AstNode *) is an anonymous struct, so a
 * single typedef is required for it to be a shared, passable type. */
typedef VEC(AstNode *) AstPtrVec;

typedef struct {
    AstPtrVec visited;   /* UniqueVisitor.self.visited (node identity) */
    int funcdecl_depth;  /* >0 == inside a FUNCDECL (== get_parent(FUNCDECL) != None) */
} FgCtx;

static bool ptr_seen_or_add(AstPtrVec *set, AstNode *n) {
    for (int i = 0; i < set->len; i++)
        if (set->data[i] == n)
            return true;
    vec_push(*set, n);
    return false;
}

/* Python _get_calls_from_children + the .entry.accessed mark
 * (optimize.py:164-171/183-184): for every CALL/FUNCCALL anywhere under
 * `node`, mark its callee (children[0] — the shared symbol-table entry)
 * accessed. Own visited set (Python filter_inorder's, ast.py:63-69).
 *
 * TRANSITIVE CASCADE (the load-bearing part). Python's
 * _get_calls_from_children is filter_inorder(node, token in
 * {CALL,FUNCCALL}) (optimize.py:164-165). A CALL/FUNCCALL Symbol's
 * children are [FUNCTION_entry, ARGLIST] (call.py:57-83), and the
 * FUNCTION entry (a SymbolID) exposes children == [PARAMLIST, body]
 * (funcref.py:27,50-60 — `.ref.body` IS children[1]). So filter_inorder,
 * descending node.children[::-1], walks INTO the callee FUNCTION entry
 * and through its body, reaching every transitively-reachable call. A
 * single global call therefore marks the whole reachable call-graph
 * accessed (verified: pararray11 `CALL func3` -> [func3,func2,func1]).
 *
 * In the C AST a CALL/FUNCCALL's children[0] is the callee AST_ID; the
 * body is not an ID child but the entry-owned u.id.body handle (set in
 * parse_sub_or_func_decl, the faithful analogue of Python
 * entry.ref.body). To port filter_inorder's descent faithfully, when a
 * CALL/FUNCCALL is reached, after marking the callee accessed also
 * descend the callee FUNCTION entry's two "children" — its PARAMLIST
 * (u.id.params) and its body (u.id.body) — exactly the [PARAMLIST,
 * body] pair Python's FUNCTION SymbolID exposes. The shared visited set
 * bounds the recursive/mutual call graph (Python filter_inorder's own
 * `visited`). */
static void collect_and_mark(AstNode *node, AstPtrVec *seen) {
    if (!node || ptr_seen_or_add(seen, node))
        return;
    if (node->tag == AST_CALL || node->tag == AST_FUNCCALL) {
        AstNode *callee = node->child_count > 0 ? node->children[0] : NULL;
        if (callee && callee->tag == AST_ID) {
            callee->u.id.accessed = true; /* symbol.entry.accessed = True */
            /* Descend the callee FUNCTION entry's children == [PARAMLIST,
             * body] (Python funcref.py:27,50-60) — the filter_inorder
             * step that makes the accessed-mark transitive. */
            collect_and_mark(callee->u.id.params, seen);
            collect_and_mark(callee->u.id.body, seen);
        }
    }
    for (int i = 0; i < node->child_count; i++)
        collect_and_mark(node->children[i], seen);
}

static void mark_calls_under(AstNode *node) {
    AstPtrVec seen;
    vec_init(seen);
    collect_and_mark(node, &seen);
    vec_free(seen);
}

/* visit_FUNCCALL (optimize.py:173-175) / visit_CALL (177-179): at global
 * scope, mark callees of every call under this node accessed; recurse. */
static AstNode *fg_visit_call(Visitor *v, AstNode *node) {
    FgCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;
    if (c->funcdecl_depth == 0) /* get_parent(FUNCDECL) is None */
        mark_calls_under(node);
    return visitor_generic(v, node);
}

/* visit_FUNCDECL (optimize.py:181-186): transitive propagation — if this
 * function's entry is already accessed, mark every call in its body;
 * then recurse (children visited at depth+1). */
static AstNode *fg_visit_funcdecl(Visitor *v, AstNode *node) {
    FgCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;
    AstNode *entry = node->child_count > 0 ? node->children[0] : NULL; /* node.entry */
    if (entry && entry->u.id.accessed)
        mark_calls_under(node);
    c->funcdecl_depth++;
    AstNode *r = visitor_generic(v, node);
    c->funcdecl_depth--;
    return r;
}

/* visit_GOTO (optimize.py:188-192) / visit_GOSUB (194-195, delegates):
 * at global scope, mark the target label accessed. The C parser builds
 * a detached AST_ID for the label (not the table entry), so resolve via
 * symboltable_access_label and mark the returned shared entry — faithful
 * to Python's args[0] == access_label(...) result. */
static AstNode *fg_visit_goto(Visitor *v, AstNode *node) {
    FgCtx *c = v->ctx;
    if (c->funcdecl_depth == 0 && node->child_count > 0 && node->children[0]) {
        AstNode *lbl = node->children[0];
        AstNode *e = symboltable_access_label(v->cs->symbol_table, v->cs,
                                              lbl->u.id.name, lbl->lineno);
        if (e)
            e->u.id.accessed = true;
    }
    return visitor_generic(v, node);
}

void functiongraph_run(CompilerState *cs, AstNode *ast) {
    Visitor v;
    visitor_init(&v, cs);

    FgCtx ctx;
    vec_init(ctx.visited);
    ctx.funcdecl_depth = 0;
    v.ctx = &ctx;

    visitor_on_tag(&v, AST_FUNCCALL, fg_visit_call);
    visitor_on_tag(&v, AST_CALL, fg_visit_call);
    visitor_on_tag(&v, AST_FUNCDECL, fg_visit_funcdecl);
    visitor_on_sentence(&v, "GOTO", fg_visit_goto);
    visitor_on_sentence(&v, "GOSUB", fg_visit_goto); /* visit_GOSUB == visit_GOTO */

    visitor_visit(&v, ast);

    vec_free(ctx.visited);
}
