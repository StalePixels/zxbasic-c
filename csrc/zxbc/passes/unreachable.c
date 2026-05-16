/*
 * passes/unreachable.c — Phase 2 pass: UnreachableCodeVisitor
 *
 * Faithful port of Python src/api/optimize.py:92-158 on the S2.1
 * visitor framework. See unreachable.h for the meter-neutrality note
 * (both emitted diagnostics are warnings).
 *
 * Python dispatches visit_FUNCTION on the symbol-table ID node
 * (.token == "FUNCTION"); the C parser sets neither u.id.body nor
 * FUNCTION-ID children, so this port keys on AST_FUNCDECL and reads
 * children[2] as the body / children[0] as the entry / node->type_ as
 * the return type — the same Python-ID-vs-C-FUNCDECL adaptation S2.2
 * established. visit_BLOCK maps to AST_BLOCK.
 *
 * No ast.c removal primitive exists; Python's node.pop(j) is mirrored
 * by an in-place children[] splice (block_pop). Empty-block collapse
 * uses the framework's return-a-replacement contract (the parent slot
 * receives the shared NOP via visitor_generic's write-back). The
 * visited-set mirrors Python UniqueVisitor (optimize.py:79-89) — it
 * prevents a shared FUNCTION-ID node being re-visited and double-
 * appending the sentinel ASM. The string-function sentinel ASM append
 * is implemented for fidelity but is codegen-only (Phase 5) and
 * meter-neutral here; AST_ASM has no is_sentinel field (flagged for
 * the Phase-5 capture).
 */
#include "passes/unreachable.h"
#include "visitor.h"
#include "errmsg.h"
#include "vec.h"
#include "arena.h"

#include <stdbool.h>
#include <string.h>

/* Single named vector type (VEC(T) is an anonymous struct — see
 * functiongraph.c). Pass-isolated copy of the established S2.2 pattern;
 * consolidating the three passes' visited-sets is a post-Phase-2
 * simplify candidate, not in-scope here. */
typedef VEC(AstNode *) AstPtrVec;

typedef struct {
    AstPtrVec visited;  /* UniqueVisitor.self.visited (node identity) */
    AstNode *nop;       /* shared NOP for empty-block collapse (Python self.NOP) */
} UcCtx;

static bool ptr_seen_or_add(AstPtrVec *set, AstNode *n) {
    for (int i = 0; i < set->len; i++)
        if (set->data[i] == n)
            return true;
    vec_push(*set, n);
    return false;
}

/* node is an AST_SENTENCE whose kind == k. */
static bool kind_is(const AstNode *n, const char *k) {
    return n && n->tag == AST_SENTENCE && n->u.sentence.kind &&
           strcmp(n->u.sentence.kind, k) == 0;
}

/* chk.is_ender (check.py:452-468): exactly these 11 SENTENCE kinds
 * (GOSUB is NOT an ender — it returns). */
static bool is_ender(const AstNode *n) {
    static const char *const enders[] = {
        "END", "ERROR", "CONTINUE_DO", "CONTINUE_FOR", "CONTINUE_WHILE",
        "EXIT_DO", "EXIT_FOR", "EXIT_WHILE", "GOTO", "RETURN", "STOP",
    };
    if (!n || n->tag != AST_SENTENCE || !n->u.sentence.kind)
        return false;
    for (size_t i = 0; i < sizeof(enders) / sizeof(enders[0]); i++)
        if (strcmp(n->u.sentence.kind, enders[i]) == 0)
            return true;
    return false;
}

/* chk.is_null (check.py:255-271): NULL, or NOP, or a BLOCK whose every
 * child is recursively null. */
static bool uc_is_null(const AstNode *n) {
    if (n == NULL || n->tag == AST_NOP)
        return true;
    if (n->tag == AST_BLOCK) {
        for (int i = 0; i < n->child_count; i++)
            if (!uc_is_null(n->children[i]))
                return false;
        return true;
    }
    return false;
}

/* Python list node.pop(j): delete children[j], shift the tail left. */
static void block_pop(AstNode *node, int j) {
    for (int i = j; i < node->child_count - 1; i++)
        node->children[i] = node->children[i + 1];
    node->child_count--;
}

static AstNode *make_asm(CompilerState *cs, int lineno, const char *code) {
    AstNode *n = ast_new(cs, AST_ASM, lineno);
    n->u.asm_block.code = arena_strdup(&cs->arena, code);
    return n;
}

/* visit_FUNCTION (optimize.py:95-110), keyed on AST_FUNCDECL in C. */
static AstNode *uc_visit_funcdecl(Visitor *v, AstNode *node) {
    UcCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;

    AstNode *idn = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *body = node->child_count > 2 ? node->children[2] : NULL;

    if (idn && idn->u.id.class_ == CLASS_function &&
        body && body->tag == AST_BLOCK) {
        int n = body->child_count;
        AstNode *last = n > 0 ? body->children[n - 1] : NULL;
        bool ends_terminal = last && (kind_is(last, "CHKBREAK") ||
                                      kind_is(last, "LABEL") ||
                                      kind_is(last, "RETURN"));
        if (n == 0 || !ends_terminal) {
            int lineno = (n == 0) ? node->lineno : last->lineno;
            warn_function_should_return(
                v->cs, lineno, idn->u.id.name ? idn->u.id.name : "");
            /* String functions must always return a value: append the
             * sentinel ASM (codegen-only, Phase 5; meter-neutral here).
             * AST_ASM has no is_sentinel field — Phase-5 follow-up. */
            if (node->type_ && type_is_string(node->type_))
                ast_add_child(v->cs, body,
                              make_asm(v->cs, lineno, "\nld hl, 0\n"));
        }
    }
    return visitor_generic(v, node);
}

/* visit_BLOCK (optimize.py:112-158). */
static AstNode *uc_visit_block(Visitor *v, AstNode *node) {
    UcCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;

    /* Phase 1 — drop CHKBREAK after a LABEL/RETURN (unconditional). */
    {
        int i = 0;
        while (i < node->child_count - 1) {
            AstNode *ch = node->children[i];
            if ((kind_is(ch, "LABEL") || kind_is(ch, "RETURN")) &&
                kind_is(node->children[i + 1], "CHKBREAK")) {
                block_pop(node, i + 1);
                continue; /* re-test same i (Python: no i+=1) */
            }
            i++;
        }
    }

    /* Phase 2 — prune dead code after an ender. */
    {
        bool warning_emitted = false;
        int i = 0;
        while (i < node->child_count) {
            if (is_ender(node->children[i])) {
                int j = i + 1;
                while (j < node->child_count) {
                    AstNode *nj = node->children[j];
                    if (kind_is(nj, "LABEL"))
                        break; /* reachable jump target */
                    if (nj && nj->tag == AST_FUNCDECL) {
                        j++;
                        continue; /* a definition, not dead code */
                    }
                    if (nj && nj->tag == AST_SENTENCE &&
                        nj->u.sentence.kind &&
                        strcmp(nj->u.sentence.kind, "END") == 0 &&
                        nj->u.sentence.sentinel) {
                        block_pop(node, j); /* synthetic END, removable */
                        continue;
                    }
                    if (nj && nj->tag == AST_ASM)
                        break; /* user inline ASM is never removed */
                    if (!warning_emitted &&
                        v->cs->opts.optimization_level > 0) {
                        warning_emitted = true;
                        warn_unreachable_code(v->cs, nj ? nj->lineno : 0);
                        if (v->cs->opts.optimization_level < 2)
                            break;
                    }
                    block_pop(node, j); /* delete the unreachable stmt */
                }
            }
            i++;
        }
    }

    /* Phase 3 — collapse a recursively-empty block to NOP (O>=1). */
    if (v->cs->opts.optimization_level >= 1 && uc_is_null(node))
        return c->nop; /* Python: yield self.NOP; return (no recurse) */

    return visitor_generic(v, node);
}

void unreachable_run(CompilerState *cs, AstNode *ast) {
    Visitor v;
    visitor_init(&v, cs);

    UcCtx ctx;
    vec_init(ctx.visited);
    ctx.nop = ast_new(cs, AST_NOP, ast ? ast->lineno : 0);
    v.ctx = &ctx;

    visitor_on_tag(&v, AST_FUNCDECL, uc_visit_funcdecl);
    visitor_on_tag(&v, AST_BLOCK, uc_visit_block);

    visitor_visit(&v, ast);

    vec_free(ctx.visited);
}
