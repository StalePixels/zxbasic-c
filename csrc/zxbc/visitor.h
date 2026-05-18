/*
 * visitor.h — AST visitor dispatch framework (Phase 2)
 *
 * Ported from the Python NodeVisitor model (src/ast/ast.py:28) +
 * GenericVisitor.generic_visit (src/api/optimize.py:72-76). A pass
 * registers only the handlers it needs; everything else falls through
 * to the generic recursive descent, mirroring Python's
 * `getattr(self, f"visit_{node.token}", self.generic_visit)`.
 *
 * Dispatch is keyed by AstTag for structural nodes, with a secondary
 * sub-dispatch on AST_SENTENCE's `u.sentence.kind` string (GOTO / LET /
 * FUNCTION / ...) because every statement shares tag == AST_SENTENCE.
 * This mirrors Python, whose `.token` resolves to the sentence kind for
 * statements — keying SENTENCE handlers by the kind string is the
 * faithful analogue, not a deviation from the plan's "keyed by AstTag".
 *
 * Built into zxbc_parser_lib so the production zxbc binary, the
 * zxbc-ast-dump test binary, and Phase 5's Translator all reuse one
 * dispatch implementation.
 */
#ifndef ZXBC_VISITOR_H
#define ZXBC_VISITOR_H

#include "zxbc.h"

typedef struct Visitor Visitor;

/*
 * A handler returns the node to put in the tree in place of `node`:
 * the same node (possibly mutated), a replacement node, or NULL to
 * prune it (the parent's child slot is set to NULL — downstream walkers
 * NULL-skip, matching Python keeping `None` in `node.children`). To
 * recurse into children, a handler calls visitor_generic(v, node)
 * (the analogue of Python `yield self.generic_visit(node)`); a handler
 * that does not is intentionally pruning/replacing the subtree.
 */
typedef AstNode *(*VisitFn)(Visitor *v, AstNode *node);

/* Max distinct AST_SENTENCE-kind handlers per pass. The Phase-2 passes
 * register a handful (FunctionGraph: GOTO/GOSUB; Optimizer:
 * LET/LETARRAY/LETSUBSTR/RETURN), but the Translator pass registers the
 * full BASIC statement set — Python's translator has ~80 visit_* methods
 * and the C routes the statement-kind ones through here (33 at S7.1b-ii;
 * POKE/OUT/BORDER/BEEP/PLOT/DRAW/CIRCLE/LOAD/SAVE/INPUT/… still to come
 * in Phase 7). Sized well above that ceiling so registration never
 * overflows the silent-drop in visitor_on_sentence (an overflow would be
 * a silently-missing handler = wrong codegen). Cheap: pointer arrays. */
#define VISITOR_MAX_SENTENCE_HANDLERS 128

struct Visitor {
    CompilerState *cs;

    /* Structural dispatch: one slot per AstTag. NULL => generic_visit.
     * AST_SENTENCE's slot is consulted only if no kind-specific handler
     * matched (see visitor_on_sentence). */
    VisitFn tag_handlers[AST_COUNT];

    /* AST_SENTENCE sub-dispatch by kind string (mirrors Python .token). */
    const char *sentence_kinds[VISITOR_MAX_SENTENCE_HANDLERS];
    VisitFn     sentence_handlers[VISITOR_MAX_SENTENCE_HANDLERS];
    int         sentence_count;

    /* Optional hooks run for every visited node (NULL = none). pre_hook
     * runs before dispatch, post_hook on the dispatch result. */
    VisitFn pre_hook;
    VisitFn post_hook;

    /* Per-pass scratch (e.g. a "visited" set for idempotent passes,
     * mirroring Python UniqueVisitor). Owned by the pass. */
    void *ctx;
};

/* Zero-initialise a visitor bound to a compiler state. */
void visitor_init(Visitor *v, CompilerState *cs);

/* Register a handler for a structural tag. Use visitor_on_sentence for
 * AST_SENTENCE statements (keying AST_SENTENCE here matches every
 * statement indiscriminately and is almost never what a pass wants). */
void visitor_on_tag(Visitor *v, AstTag tag, VisitFn fn);

/* Register a handler for an AST_SENTENCE of the given kind, e.g.
 * "GOTO", "LET", "FUNCTION". `kind` must be a stable string (string
 * literal or arena-owned); it is compared by strcmp, not copied. */
void visitor_on_sentence(Visitor *v, const char *kind, VisitFn fn);

/* Dispatch `node`: pre_hook, then the matching handler (sentence-kind,
 * else tag, else generic), then post_hook. Returns the (possibly
 * replaced/pruned) node. NULL node => NULL. */
AstNode *visitor_visit(Visitor *v, AstNode *node);

/* Generic recursive descent: visit each child in order, write the
 * (possibly replaced) child back into node->children[i], then return
 * node. Post-order, faithful to Python GenericVisitor.generic_visit
 * (src/api/optimize.py:72-76). Handlers call this to recurse. */
AstNode *visitor_generic(Visitor *v, AstNode *node);

/* Run the post-parse semantic passes on `ast`, in Python's order
 * (Unreachable -> FunctionGraph -> Optimizer; src/zxbc/zxbc.py:107-141).
 * Phase 2 groundwork (S2.1): this is the inert wire-in point — no
 * passes are registered yet; the three passes land in S2.2-S2.4. */
void visitor_run_passes(CompilerState *cs, AstNode *ast);

#endif /* ZXBC_VISITOR_H */
