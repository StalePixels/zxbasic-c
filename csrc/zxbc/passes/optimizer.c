/*
 * passes/optimizer.c — Phase 2 pass 3: OptimizerVisitor
 *
 * Faithful port of Python src/api/optimize.py:198-490 (OptimizerVisitor)
 * on the S2.1 visitor framework. See optimizer.h for the meter-neutrality
 * note (all five class-body diagnostics are warnings, never error()).
 *
 * ----------------------------------------------------------------------
 * Structural Python-vs-C adaptations (each is the same class of gap the
 * S2.2/S2.3 captures established — Python symbol-properties vs the C
 * tagged-union AST; documented here, not invented behaviour):
 *
 *  - The master O-gate. Python's visit() override (optimize.py:201-205)
 *    intercepts EVERY visit and returns the node unvisited when
 *    O_LEVEL < 1. The C framework has no per-pass visit override; the
 *    faithful analogue (per the port map §2) is to no-op the whole pass
 *    in optimizer_run() when cs->opts.optimization_level < 1.
 *
 *  - FUNCDECL. Python SymbolFUNCDECL has a single child (the FUNCTION
 *    ID entry, funcdecl.py:25-31); params_size/locals_size and
 *    convention live on entry.ref. The C parser builds AST_FUNCDECL as
 *    children[0]=ID, [1]=PARAMLIST, [2]=body (zxbc.h:186) and stores
 *    sizes/convention on children[0]->u.id (param_size/local_size/
 *    convention). Same FUNCDECL-vs-Python-ID adaptation as S2.2/S2.3:
 *    key on AST_FUNCDECL, read entry = children[0], recurse the body.
 *
 *  - is_dynamic (visit_ADDRESS:209). Python chk.is_dynamic(operand),
 *    check.py:370-379:
 *      try: return not any(i.scope == SCOPE.global_ and i.is_basic
 *                          and i.type_ != Type.string for i in p)
 *      except Exception: pass
 *      return False
 *    `i.is_basic` does NOT exist on the SymbolID/ref chain
 *    (SymbolID.__getattr__ forwards to .ref; every *Ref is __slots__-only
 *    with no is_basic and no __getattr__ — verified symbolref.py et al.)
 *    so evaluating it raises AttributeError. BUT Python `and`
 *    short-circuits: for a NON-global operand `i.scope == SCOPE.global_`
 *    is False, `i.is_basic` is never evaluated, no exception, the any()
 *    term is False ⇒ is_dynamic returns `not False` = TRUE. Only for a
 *    GLOBAL operand is `i.is_basic` reached ⇒ AttributeError ⇒ except ⇒
 *    return False. (An operand whose `.scope` is itself unresolvable
 *    likewise excepts ⇒ False.) Net for the single visit_ADDRESS
 *    operand: is_dynamic == (operand is a non-global-scoped ID). Hence
 *    `not is_dynamic` (⇒ CONSTEXPR wrap) iff the operand is global-scope
 *    (or non-ID/unscoped). NOT "always wrap", and NOT type_is_dynamic()
 *    — both diverge from Python. Faithfully ported below.
 *
 *  - visit_ADDRESS array branch (optimize.py:211-219). Python reads
 *    node.operand.offset — a SymbolARRAYACCESS cached_property
 *    (arrayaccess.py:68-91) computed from entry.bounds / b.count /
 *    b.lower. The C AST_ARRAYACCESS carries no offset and the entry ID
 *    has no reachable bound metadata (no ref/bounds linkage). The
 *    faithful analogue of an unavailable cached offset is None ⇒ the
 *    `elif operand.offset is not None` branch is never taken; an
 *    ARRAYACCESS operand falls through to `yield node` unchanged
 *    (exactly Python's behaviour for a non-constant access).
 *
 *  - "MUL" vs "MULT". Python emits operator "MUL" for multiply
 *    (zxbparser.py:2382); the C parser spells it "MULT" (parser.c:281),
 *    and the C compiler already treats "MULT" and "MUL" as the same
 *    operation (fold_numeric, compiler.c:543). Faithful to Python's
 *    *behaviour* means C's "MULT" multiply nodes get the same O>1
 *    canonicalise/re-associate/factor that Python applies to "MUL" —
 *    the same C-representation adaptation as FUNCDECL-vs-ID. The
 *    operator-membership check accepts "PLUS"/"MULT"/"MUL"
 *    (visit_BINARY); a literal "MUL"-only port would silently OMIT an
 *    optimisation Python performs ⇒ a latent Phase-5 AST divergence.
 *
 *  - CHR$ args. Python CHR's operand is an ARGLIST of ARGUMENT nodes;
 *    `for arg in node.operand` iterates ARGUMENTs and `arg.value` is the
 *    ARGUMENT's expr child (zxbparser.py:3427-3444). The C parser builds
 *    AST_BUILTIN with the argument expressions as DIRECT children (no
 *    ARGLIST/ARGUMENT wrapper, parser.c:363-370). So `arg.value` ≡ the
 *    child itself; `x.value.value & 0xFF` reads the NUMBER payload.
 *
 *  - FUNCCALL/CALL bound-status helper
 *    (_check_if_any_arg_is_an_array_and_needs_lbound_or_ubound,
 *    optimize.py:455-489). Pure codegen-offset bookkeeping over
 *    entry.ref.params / arg.value.ref.lbound_used / arg.requires /
 *    scope_ref.owner.locals_size / compute_offsets — none of which
 *    exist in the C AST. Capture §1/§4: meter-neutral at parse-only.
 *    The recursion (node.args only) is ported; the helper is a
 *    structural no-op because the data it reads/writes is absent (same
 *    gap class as S2.2/S2.3).
 *
 *  - RETURN. Python RETURN = make_sentence("RETURN", func, value):
 *    children[0]=enclosing-function backref (skipped to avoid infinite
 *    recursion), children[1]=value (zxbparser.py:1765/2110). The C
 *    parser builds RETURN with only the value as children[0] (no func
 *    backref, parser.c:1052-1059). Python `if len==2: visit children[1]`
 *    ⇒ C `if child_count==1: visit children[0]` (the value); no
 *    backref to skip, no recursion risk.
 *
 * Idempotence (Python UniqueVisitor, optimize.py:84-89) is required:
 * OptimizerVisitor deliberately re-enters nodes (FUNCDECL re-visits the
 * entry; BINARY recurses 1-2×; recursive call graphs revisit shared
 * symbol IDs). The visited AstNode* set guards the cycle re-entry
 * handlers (FUNCDECL/FUNCCALL/CALL/BINARY) exactly as S2.2/S2.3 do. The
 * shared NOP mirrors Python self.NOP.
 * ----------------------------------------------------------------------
 */
#include "passes/optimizer.h"
#include "visitor.h"
#include "errmsg.h"
#include "vec.h"
#include "arena.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Single named vector type (VEC(T) is an anonymous struct — see
 * functiongraph.c / unreachable.c). Pass-isolated copy of the
 * established S2.2/S2.3 pattern; consolidating the three passes'
 * visited-sets is a post-Phase-2 simplify candidate, not in-scope. */
typedef VEC(AstNode *) AstPtrVec;

typedef struct {
    AstPtrVec visited;  /* UniqueVisitor.self.visited (node identity) */
    AstNode *nop;       /* shared NOP for all prunes (Python self.NOP) */
    bool bound_status_changed; /* any in-visit opt_bound_status_helper flip */
} OptCtx;

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

/* chk.is_number(a, b) / (a, b, c): ALL args are NUMBER or numeric
 * CONST. check_is_number(single) already exists (compiler.c:313). */
static bool is_number2(const AstNode *a, const AstNode *b) {
    return check_is_number(a) && check_is_number(b);
}
static bool is_number3(const AstNode *a, const AstNode *b, const AstNode *c) {
    return check_is_number(a) && check_is_number(b) && check_is_number(c);
}

/* chk.is_callable (check.py:382-384): True if the node is a FUNCTION.
 * The C parser has no FUNCTION-ID-as-token; a callable is an
 * AST_FUNCDECL (or a FUNCTION-class ID entry). Used only as the
 * is_block_accessed filter (skip callables). */
static bool opt_is_callable(const AstNode *n) {
    if (!n)
        return false;
    if (n->tag == AST_FUNCDECL)
        return true;
    if (n->tag == AST_ID &&
        (n->u.id.class_ == CLASS_function || n->u.id.class_ == CLASS_sub))
        return true;
    return false;
}

/* chk.is_block_accessed (check.py:387-396): True if `block` is a LABEL
 * that is accessed, OR any non-callable child is recursively accessed.
 * Mirror of uc_is_null's recursive shape (unreachable.c:77-87).
 *
 * C-vs-Python adaptation: in Python a LABEL Symbol IS the symbol-table
 * entry (block.accessed forwards via __getattr__ to LabelRef.accessed,
 * which FunctionGraphVisitor.visit_GOTO/GOSUB sets on the entry). In the
 * C AST the LABEL sentence's children[0] is a fresh, detached AST_ID
 * (parser.c:1394-1397, 1404-1407) that does NOT share identity with the
 * symbol-table entry — and FunctionGraph (functiongraph.c:128-138) sets
 * u.id.accessed on the ENTRY returned by symboltable_access_label. So
 * the in-tree LABEL child's .accessed stays false even when the label
 * IS jump-targeted; reading off it under-reports and Python's keep-on-
 * referenced-label predicate misfires (the dropped IF/WHILE bodies the
 * S1-DIVERGE ifemptylabel1/2 and whilefalse1 cluster reports). Resolve the
 * label by NAME via symboltable_lookup (read-only) to read .accessed
 * off the shared entry — faithful to Python's "block.accessed". By
 * optimizer-pass time parsing is complete, labels live in global scope,
 * and the lookup is purely observational. */
static bool is_block_accessed_ctx(CompilerState *cs, const AstNode *block) {
    if (!block)
        return false;
    if (kind_is(block, "LABEL")) {
        AstNode *lbl = block->child_count > 0 ? block->children[0] : NULL;
        if (lbl) {
            if (lbl->u.id.accessed)
                return true;
            if (cs && cs->symbol_table && lbl->u.id.name) {
                AstNode *entry =
                    symboltable_lookup(cs->symbol_table, lbl->u.id.name);
                if (entry && entry != lbl && entry->u.id.accessed)
                    return true;
            }
        }
    }
    for (int i = 0; i < block->child_count; i++) {
        AstNode *ch = block->children[i];
        if (!opt_is_callable(ch) && is_block_accessed_ctx(cs, ch))
            return true;
    }
    return false;
}

/* Python list node.children.pop(): delete children[j], shift tail. */
static void block_pop(AstNode *node, int j) {
    for (int i = j; i < node->child_count - 1; i++)
        node->children[i] = node->children[i + 1];
    node->child_count--;
}

/* Wrap `node` in a fresh CONSTEXPR (Python symbols.CONSTEXPR(node,
 * lineno)). Mirrors the CONSTEXPR construction in make_binary_node
 * (compiler.c:656-659): AST_CONSTEXPR with children[0]=node, type_
 * inherited. */
static AstNode *make_constexpr(CompilerState *cs, AstNode *node, int lineno) {
    AstNode *ce = ast_new(cs, AST_CONSTEXPR, lineno);
    ast_add_child(cs, ce, node);
    ce->type_ = node ? node->type_ : NULL;
    return ce;
}

/* chk.is_dynamic(operand) for the single visit_ADDRESS operand
 * (check.py:370-379) — see file header. Net faithful semantics:
 * is_dynamic is TRUE iff the operand is a non-global-scoped ID
 * (non-global short-circuits before the AttributeError-raising
 * is_basic); GLOBAL ID, or any operand whose scope isn't an ID scope,
 * yields FALSE (Python's except → False). */
static bool addr_operand_is_dynamic(const AstNode *op) {
    return op && op->tag == AST_ID && op->u.id.scope != SCOPE_global;
}

/* ----------------------------------------------------------------
 * visit_ADDRESS (optimize.py:207-220)
 *
 * Non-ARRAYACCESS operand: wrap the @-address in a CONSTEXPR iff
 * `not chk.is_dynamic(operand)` — i.e. iff the operand is global-scope
 * (or non-ID/unscoped); a non-global-scoped ID is is_dynamic ⇒ NOT
 * wrapped (faithful to Python's short-circuit, see file header).
 *
 * ARRAYACCESS operand with const offset & global scope (Bug A): Python
 * rewrites @arr(c1,c2,...) into BINARY(PLUS,
 *   UNARY("ADDRESS", entry, type_=PTR_TYPE),
 *   NUMBER(offset, type_=PTR_TYPE)).
 * The C AST DOES carry `arrayaccess.offset` / `arrayaccess.is_const`
 * (zxbc.h:314-323, parser.c:3804-3846 compute_arrayaccess_offset). For
 * the rewrite to produce the right asm (`ld hl, _<arr>.__DATA__; ld de,
 * N; add hl, de`) we need the UNARY-ADDRESS scalar branch
 * (translator.c:1956) to see `operand->t == "_<arr>.__DATA__"` — the
 * data_label, which is Python's ArrayRef.t for a global array
 * (arrayref.py:69-78). The C array entry's .t is unset, so build a
 * detached scalar-shaped AST_ID with .t set to the data_label here.
 * The original entry in the symbol table is untouched.
 *
 * Parameter / local array offsets are None in Python (parameter array
 * is never constant-foldable, arrayaccess.py:77-78; the C parser
 * mirrors that, parser.c:3811-3812), so this branch only fires for
 * global arrays.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_address_array_const_global(
        Visitor *v, AstNode *node, AstNode *acc) {
    AstNode *entry = acc->child_count > 0 ? acc->children[0] : NULL;
    if (!entry || entry->tag != AST_ID || !entry->u.id.mangled)
        return node;
    CompilerState *cs = v->cs;
    TypeInfo *ptr_t = cs->symbol_table->basic_types[TYPE_uinteger];

    /* Detached scalar-shaped AST_ID with t = "_<mangled>.__DATA__".
     * Reuses name/mangled (shared via arena) but is otherwise fresh: the
     * UNARY-ADDRESS scalar branch reads only .scope/.t (translator.c
     * :1957-1962), so this carries exactly the data_label string. */
    AstNode *scalar = ast_new(cs, AST_ID, node->lineno);
    scalar->u.id.name = entry->u.id.name;
    scalar->u.id.mangled = entry->u.id.mangled;
    scalar->u.id.class_ = CLASS_var;
    scalar->u.id.scope = SCOPE_global;
    scalar->u.id.declared = true;
    scalar->u.id.has_address = true;
    scalar->type_ = ptr_t;
    size_t ml = strlen(entry->u.id.mangled);
    char *dlabel = arena_alloc(&cs->arena, ml + 10);
    snprintf(dlabel, ml + 10, "%s.__DATA__", entry->u.id.mangled);
    scalar->t = dlabel;

    AstNode *u = make_unary_node(cs, "ADDRESS", scalar, node->lineno);
    if (!u) return node;
    u->type_ = ptr_t;

    AstNode *num = ast_number(cs, (double)acc->u.arrayaccess.offset,
                              acc->lineno);
    if (!num) return node;
    num->type_ = ptr_t;

    AstNode *plus = make_binary_node(cs, "PLUS", u, num, node->lineno, ptr_t);
    return plus ? plus : node;
}

static AstNode *opt_visit_address(Visitor *v, AstNode *node) {
    AstNode *operand = node->child_count > 0 ? node->children[0] : NULL;
    if (operand && operand->tag != AST_ARRAYACCESS) {
        if (!addr_operand_is_dynamic(operand))
            node = make_constexpr(v->cs, node, node->lineno);
    } else if (operand && operand->u.arrayaccess.is_const) {
        AstNode *entry = operand->child_count > 0 ? operand->children[0]
                                                  : NULL;
        if (entry && entry->tag == AST_ID &&
            entry->u.id.scope == SCOPE_global) {
            node = opt_visit_address_array_const_global(v, node, operand);
        }
    }
    return node;
}

/* ----------------------------------------------------------------
 * visit_UNARY (optimize.py:376-380): ADDRESS delegates to
 * visit_ADDRESS; everything else generic-recurses.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_unary(Visitor *v, AstNode *node) {
    if (node->u.unary.operator &&
        strcmp(node->u.unary.operator, "ADDRESS") == 0)
        return opt_visit_address(v, node);
    return visitor_generic(v, node);
}

/* ----------------------------------------------------------------
 * visit_BINARY (optimize.py:222-275) — the heavy handler.
 *
 * Always generic_visit first (may fold CONSTEXPR/NUMBER children). At
 * O>1, for commutative PLUS/MUL: (a) number-left canonicalisation
 * swap, (b) re-associate a constant out of a same-operator left child,
 * (c) factor two same-operator children. Final line unconditionally
 * re-runs make_binary_node to retry constant folding.
 *
 * Idempotence-guarded (cycle re-entry: recurses 1-2×). C make_binary_node
 * has no `func` arg (zxbc.h:319) — Python's node.func is the
 * constant-fold lambda, re-derived inside make_binary_node from the
 * operator string in the C port, so it is intentionally not threaded.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_binary(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;

    node = visitor_generic(v, node);
    if (!node || node->tag != AST_BINARY)
        return node; /* generic_visit may have folded it to NUMBER/etc. */

    const char *op = node->u.binary.operator;
    /* Python checks `node.operator in ("PLUS","MUL")` (optimize.py:225).
     * The C parser spells multiply "MULT" (parser.c:281) where Python
     * uses "MUL"; the C compiler already treats the two as the same
     * operation (fold_numeric, compiler.c:543). Faithful to Python's
     * *behaviour* (not its literal string), C's "MULT" multiply nodes
     * must get the same canonicalisation/re-association/factoring Python
     * applies to "MUL" — same C-representation adaptation as the
     * FUNCDECL-vs-ID one. Accept both spellings. */
    bool commutative = op && (strcmp(op, "PLUS") == 0 ||
                              strcmp(op, "MULT") == 0 ||
                              strcmp(op, "MUL") == 0);

    if (v->cs->opts.optimization_level > 1 && commutative) {
        AstNode *left = node->child_count > 0 ? node->children[0] : NULL;
        AstNode *right = node->child_count > 1 ? node->children[1] : NULL;

        /* (a) number-left canonicalisation: x is number, y is not. */
        if (check_is_number(left) && !check_is_number(right)) {
            node->children[0] = right;
            node->children[1] = left;
            node = visitor_generic(v, node);
            if (!node || node->tag != AST_BINARY)
                return node;
            left = node->child_count > 0 ? node->children[0] : NULL;
            right = node->child_count > 1 ? node->children[1] : NULL;
        }

        /* (b) re-associate (x ∘ c1) ∘ c2 → x ∘ (c1 ∘ c2). */
        if (left && left->tag == AST_BINARY && left->u.binary.operator &&
            strcmp(left->u.binary.operator, op) == 0 &&
            check_is_number(right)) {
            AstNode *l_left = left->child_count > 0 ? left->children[0] : NULL;
            AstNode *l_right = left->child_count > 1 ? left->children[1] : NULL;
            AstNode *new_left = NULL, *ll = NULL;
            if (check_is_number(l_right)) {
                new_left = l_left;
                ll = l_right;
            } else if (check_is_number(l_left)) {
                new_left = l_right;
                ll = l_left;
            }
            if (new_left != NULL) {
                AstNode *nr = make_binary_node(v->cs, op, ll, right,
                                               node->lineno, NULL);
                nr = visitor_visit(v, nr);
                node->children[0] = new_left;
                node->children[1] = nr;
                left = new_left;
                right = nr;
            }
        }

        /* (c) factor (a ∘ c1) ∘ (b ∘ c2) → (a ∘ b) ∘ (c1 ∘ c2). */
        if (left && right && left->tag == AST_BINARY &&
            right->tag == AST_BINARY && left->u.binary.operator &&
            right->u.binary.operator &&
            strcmp(op, left->u.binary.operator) == 0 &&
            strcmp(op, right->u.binary.operator) == 0) {
            AstNode *l_left = left->child_count > 0 ? left->children[0] : NULL;
            AstNode *l_right = left->child_count > 1 ? left->children[1] : NULL;
            AstNode *r_left = right->child_count > 0 ? right->children[0] : NULL;
            AstNode *r_right = right->child_count > 1 ? right->children[1] : NULL;
            if (is_number2(l_right, r_right)) {
                AstNode *nl = make_binary_node(v->cs, op, l_left, r_left,
                                               left->lineno, NULL);
                nl = visitor_visit(v, nl);
                AstNode *nr = make_binary_node(v->cs, op, l_right, r_right,
                                               right->lineno, NULL);
                nr = visitor_visit(v, nr);
                AstNode *nb = make_binary_node(v->cs, op, nl, nr,
                                               node->lineno, NULL);
                node = visitor_visit(v, nb);
                if (!node || node->tag != AST_BINARY)
                    return node;
            }
        }
    }

    /* Retry folding (optimize.py:275) — unconditional at any O≥1
     * reaching here. make_binary_node is the folding engine; the C sig
     * has no func arg, type_ is threaded as node->type_. */
    {
        AstNode *l = node->child_count > 0 ? node->children[0] : NULL;
        AstNode *r = node->child_count > 1 ? node->children[1] : NULL;
        AstNode *res = make_binary_node(v->cs, node->u.binary.operator,
                                        l, r, node->lineno, node->type_);
        return res ? res : node; /* make_binary_node NULL ⇒ keep node
                                  * (error already emitted at parse time;
                                  * see capture Q-A caveat). */
    }
}

/* ----------------------------------------------------------------
 * visit_CHR (optimize.py:284-290) — reached via the BUILTIN delegate.
 * Recurse, then if every arg is static fold the whole CHR$ to a
 * compile-time STRING. C BUILTIN args are direct children (no
 * ARGLIST/ARGUMENT) so arg.value ≡ the child; x.value.value is the
 * NUMBER payload. Faithful: require every child static for the gate
 * and AST_NUMBER for the actual char value (Python's `.value.value`
 * is the numeric payload — only a NUMBER carries it in the C AST).
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_chr(Visitor *v, AstNode *node) {
    node = visitor_generic(v, node);
    if (!node || node->tag != AST_BUILTIN)
        return node;

    bool all_static = node->child_count > 0;
    for (int i = 0; i < node->child_count; i++) {
        if (!check_is_static(node->children[i])) {
            all_static = false;
            break;
        }
    }
    /* Python folds only when every arg is foldable to a concrete char
     * code; in the C AST that concrete value lives solely on AST_NUMBER
     * (a static CONST/CONSTEXPR without a resolved NUMBER cannot yield
     * chr(x.value.value) here). */
    if (all_static) {
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]->tag != AST_NUMBER) {
                all_static = false;
                break;
            }
        }
    }
    if (all_static) {
        char *buf = arena_alloc(&v->cs->arena, (size_t)node->child_count + 1);
        for (int i = 0; i < node->child_count; i++) {
            int code = (int)node->children[i]->u.number.value;
            buf[i] = (char)(code & 0xFF);
        }
        buf[node->child_count] = '\0';
        AstNode *s = ast_new(v->cs, AST_STRING, node->lineno);
        s->u.string.value = buf;
        s->u.string.length = node->child_count;
        s->type_ = v->cs->symbol_table->basic_types[TYPE_string];
        return s;
    }
    return node;
}

/* ----------------------------------------------------------------
 * visit_BUILTIN (optimize.py:277-282): dispatch to visit_<FNAME> if
 * one exists, else generic recurse. In this class the only match is
 * visit_CHR (so fname == "CHR" routes there); everything else generic.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_builtin(Visitor *v, AstNode *node) {
    if (node->u.builtin.fname &&
        strcmp(node->u.builtin.fname, "CHR") == 0)
        return opt_visit_chr(v, node);
    return visitor_generic(v, node);
}

/* ----------------------------------------------------------------
 * visit_CONSTEXPR (optimize.py:292-296): unwrap a CONSTEXPR whose
 * inner expr is already a NUMBER/CONST. No recurse.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_constexpr(Visitor *v, AstNode *node) {
    (void)v;
    AstNode *expr = node->child_count > 0 ? node->children[0] : NULL;
    if (check_is_number(expr) || check_is_const(expr))
        return expr;
    return node;
}

/* The PARAMLIST ARGUMENT carries only name/byref/is_array; the actual
 * lbound_used/ubound_used/is_dynamically_accessed flags a `UBOUND(a,..)`
 * / `LBOUND(a,..)` / dynamic access inside the body sets live on the
 * BODY-SCOPE symbol for that parameter (parser.c:4669 declares a
 * separate CLASS_array ID in the function scope; parser.c:432-455 flags
 * THAT node). Those body symbols are captured onto the function entry as
 * u.id.local_entries (parser.c:4738-4748). Locate the param's body
 * symbol by name. */
static AstNode *opt_param_body_sym(const AstNode *callee, const char *pname) {
    if (!callee || callee->tag != AST_ID || !pname)
        return NULL;
    for (int i = 0; i < callee->u.id.local_entries_count; i++) {
        AstNode *e = callee->u.id.local_entries[i];
        if (e && e->tag == AST_ID && e->u.id.scope == SCOPE_parameter &&
            e->u.id.name && strcmp(e->u.id.name, pname) == 0)
            return e;
    }
    return NULL;
}

/* _check_if_any_arg_is_an_array_and_needs_lbound_or_ubound +
 * _update_bound_status (optimize.py:455-489). For each (arg, param) of
 * zip(args, params): a byref array param whose ref's lbound/ubound are
 * not BOTH already set propagates its bound-usage onto the argument
 * array's shared symbol entry. Python plumbs this through arg.requires
 * (call.py:48 add_required_symbol(param) makes the global arg array
 * require the param; _update_bound_status ORs every requirer's
 * p.ref.lbound_used / p.ref.ubound_used into arg.ref). The C has no
 * requires graph, but the net effect is exactly "OR the byref array
 * param's lbound_used/ubound_used into the argument array entry" — the
 * documented faithful analogue (CLAUDE.md: derive from Python's
 * entry.ref-sharing resolution). is_dynamically_accessed is NOT touched
 * here (Python's _update_bound_status only ORs lbound/ubound; the
 * call.py:53-55 is_dynamically_accessed propagation is a separate
 * mechanism, already covered by parser.c:1043's unconditional set on
 * every constructed array access). The arg array entry is the SAME
 * shared symbol-table node VarTranslator.visit_ARRAYDECL reads
 * (var_translator.c:496-505) — so the descriptor's __LBOUND__/__UBOUND__
 * slot + trailing bound table now emit for lbound13's global _x. */
/* Returns true if any flag was newly set (lbound_used / ubound_used /
 * is_dynamically_accessed) on any argument array entry.  Caller uses this
 * to drive the post-pass fixed-point iteration that mirrors Python's
 * transitive `requires` graph propagation (symbol_.py:50-58). */
static bool opt_bound_status_helper_changed(AstNode *node) {
    bool changed = false;
    if (!node || node->child_count < 2)
        return false;
    AstNode *callee = node->children[0];
    AstNode *arglist = node->children[1];
    if (!callee || callee->tag != AST_ID || !arglist ||
        arglist->tag != AST_ARGLIST)
        return false;
    AstNode *params = callee->u.id.params;
    if (!params || params->tag != AST_PARAMLIST)
        return false;

    int zipn = arglist->child_count < params->child_count
                   ? arglist->child_count : params->child_count;
    for (int z = 0; z < zipn; z++) {
        AstNode *arg = arglist->children[z];
        AstNode *param = params->children[z];
        if (!arg || arg->tag != AST_ARGUMENT || !param ||
            param->tag != AST_ARGUMENT)
            continue;
        /* if not param.byref or param.class_ != CLASS.array: continue */
        if (!param->u.argument.byref || !param->u.argument.is_array)
            continue;
        AstNode *argval = arg->child_count > 0 ? arg->children[0] : NULL;
        if (!argval || argval->tag != AST_ID)
            continue;
        /* if arg.value.ref.lbound_used and arg.value.ref.ubound_used:
         *     continue */
        if (argval->u.id.lbound_used && argval->u.id.ubound_used)
            continue;
        AstNode *psym = opt_param_body_sym(callee, param->u.argument.name);
        if (!psym)
            continue;
        /* _update_bound_status: arg_ref.lbound_used |= p.ref.lbound_used;
         *                       arg_ref.ubound_used |= p.ref.ubound_used */
        if (psym->u.id.lbound_used && !argval->u.id.lbound_used) {
            argval->u.id.lbound_used = true;
            changed = true;
        }
        if (psym->u.id.ubound_used && !argval->u.id.ubound_used) {
            argval->u.id.ubound_used = true;
            changed = true;
        }
        /* call.py:49-55 propagation of is_dynamically_accessed. Python
         * does this at CALL construction (FuncRef args/params zipped).
         * The C symbol-table construction does not see the param body's
         * arrayaccess (set on `arr` only after the SUB body is parsed),
         * so the propagation is mechanised here alongside the bound-
         * status helper. array13.bas: SUB test(arr() As UByte) does
         * arr(i) with variable i → is_dynamically_accessed set on `arr`;
         * caller `test array` then must propagate that onto the global
         * `array` so var_translator (var_translator.c:521) emits the
         * `_array.__LBOUND__` descriptor slot + label. */
        if (psym->u.id.is_dynamically_accessed &&
            !argval->u.id.is_dynamically_accessed) {
            argval->u.id.is_dynamically_accessed = true;
            changed = true;
        }
    }
    return changed;
}

static void opt_bound_status_helper(AstNode *node) {
    (void)opt_bound_status_helper_changed(node);
}

/* ----------------------------------------------------------------
 * visit_FUNCCALL (optimize.py:298-301) / visit_CALL (303-306):
 * recurse into node.args ONLY (children[1], the ARGLIST) — not the
 * entry (children[0]) to avoid infinite recursion through the symbol
 * table — then run the byref-array bound-status helper. Idempotence-
 * guarded.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_call(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;
    if (node->child_count > 1)
        node->children[1] = visitor_visit(v, node->children[1]);
    if (opt_bound_status_helper_changed(node))
        c->bound_status_changed = true;
    return node;
}

/* ----------------------------------------------------------------
 * visit_FUNCDECL (optimize.py:308-318): at O>1, an un-accessed
 * function entry (FunctionGraph S2.2 set this) → warning + prune to
 * NOP; zero-param/zero-local funcs get fastcall. Always recurse the
 * body. Keyed on AST_FUNCDECL; entry = children[0] (S2.2/S2.3
 * adaptation). Idempotence-guarded.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_funcdecl(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    if (ptr_seen_or_add(&c->visited, node))
        return node;

    AstNode *entry = node->child_count > 0 ? node->children[0] : NULL;

    if (v->cs->opts.optimization_level > 1 && entry &&
        !entry->u.id.accessed) {
        warn_func_never_called(v->cs, entry->lineno,
                               entry->u.id.name ? entry->u.id.name : "");
        return c->nop;
    }

    if (v->cs->opts.optimization_level > 1 && entry &&
        entry->u.id.param_size == 0 && entry->u.id.local_size == 0)
        entry->u.id.convention = CONV_fastcall;

    /* Python: node.children[1] = visit(node.entry) — re-visit the
     * entry (which carries the body). The C FUNCDECL body is
     * children[2]; recurse it (the entry is children[0], no body on
     * the ID). Faithful to "recurse the function's contents once". */
    if (node->child_count > 2)
        node->children[2] = visitor_visit(v, node->children[2]);
    return node;
}

/* Python visit_LET side-effect extraction (optimize.py:324-336): the
 * filter_inorder collector — gather FUNCCALL/BUILTIN nodes under the
 * RHS (not descending into FUNCTION/FUNCDECL), keeping FUNCCALLs and
 * builtins whose fname is IN/RND/USR. FUNCCALL→CALL conversion; for
 * LETARRAY only FUNCCALLs are kept. Own visited set (Python
 * filter_inorder's, ast.py:63-69) — mirror functiongraph.c:collect. */
static void collect_side_effects(Visitor *v, AstNode *node,
                                 AstPtrVec *seen, AstPtrVec *out,
                                 bool builtins_too) {
    CompilerState *cs = v->cs;
    if (!node || ptr_seen_or_add(seen, node))
        return;
    if (opt_is_callable(node))
        return; /* filter_inorder stop predicate: token != "FUNCTION" */

    if (node->tag == AST_FUNCCALL) {
        /* filter_inorder (ast.py:64-65): `if filter_func(node): yield
         * self.visit(node)` — the matched FUNCCALL is *visited* (not
         * merely collected). visit_FUNCCALL (optimize.py:298-301)
         * recurses node.args and runs
         * _check_if_any_arg_is_an_array_and_needs_lbound_or_ubound,
         * which is the load-bearing bound-status propagation for an
         * unused-lvalue LET whose RHS is a byref-array call
         * (lbound13: `LET y = maxValue(x)`, y unused). Without this
         * visit the global array x's __UBOUND__ descriptor is never
         * emitted. Idempotence-guarded inside opt_visit_call. */
        node = visitor_visit(v, node);
        /* symbols.CALL(x.entry, x.args, ...) — convert FUNCCALL→CALL.
         * Same node, retag (C CALL/FUNCCALL share layout, parser.c:1494).
         * Python applies CALL() to the *visited* node. */
        AstNode *call = ast_new(cs, AST_CALL, node->lineno);
        call->type_ = node->type_;
        for (int i = 0; i < node->child_count; i++)
            ast_add_child(cs, call, node->children[i]);
        vec_push(*out, call);
    } else if (builtins_too && node->tag == AST_BUILTIN) {
        const char *fn = node->u.builtin.fname;
        if (fn && (strcmp(fn, "IN") == 0 || strcmp(fn, "RND") == 0 ||
                   strcmp(fn, "USR") == 0)) {
            /* node_.discard_result = True (optimize.py:335) — consumed
             * by visit_BUILTIN (translator.py:154-155). */
            node->u.builtin.discard_result = true;
            vec_push(*out, node);
        }
    }
    for (int i = 0; i < node->child_count; i++)
        collect_side_effects(v, node->children[i], seen, out, builtins_too);
}

static AstNode *build_side_effect_block(Visitor *v, AstNode *rhs,
                                        int lineno, bool builtins_too) {
    AstPtrVec seen, out;
    vec_init(seen);
    vec_init(out);
    collect_side_effects(v, rhs, &seen, &out, builtins_too);
    AstNode *block = ast_new(v->cs, AST_BLOCK, lineno);
    for (int i = 0; i < out.len; i++)
        ast_add_child(v->cs, block, out.data[i]);
    vec_free(seen);
    vec_free(out);
    return block;
}

/* ----------------------------------------------------------------
 * visit_LET (optimize.py:320-340): at O>1, if the assigned lvalue is
 * never accessed → warning + replace with a BLOCK of only the
 * side-effecting calls extracted from the RHS; else generic recurse.
 * C LET: children[0]=lvalue ID, children[1]=RHS.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_let(Visitor *v, AstNode *node) {
    AstNode *lvalue = node->child_count > 0 ? node->children[0] : NULL;
    if (v->cs->opts.optimization_level > 1 && lvalue &&
        !lvalue->u.id.accessed) {
        warn_not_used(v->cs, lvalue->lineno,
                      lvalue->u.id.name ? lvalue->u.id.name : "",
                      "Variable"); /* Python default kind= "Variable" */
        AstNode *rhs = node->child_count > 1 ? node->children[1] : NULL;
        return build_side_effect_block(v, rhs, node->lineno, true);
    }
    return visitor_generic(v, node);
}

/* ----------------------------------------------------------------
 * visit_LETARRAY (optimize.py:342-358): same shape as visit_LET but
 * lvalue = node.args[0].entry and only FUNCCALLs are extracted (no
 * builtins). C LETARRAY: children[0]=ARRAYACCESS (its children[0] is
 * the entry ID), children[1]=RHS.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_letarray(Visitor *v, AstNode *node) {
    AstNode *acc = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *lvalue = (acc && acc->child_count > 0) ? acc->children[0] : NULL;
    /* Python's node.args[0].entry is ALWAYS a SymbolID (arrayaccess.py:40
     * asserts token=="VARARRAY"). The C parser, lacking Python's distinct
     * LETSUBSTR/LETARRAYSUBSTR productions for a postfix `a$(i)(j)`,
     * builds a single LETARRAY whose ARRAYACCESS child[0] is a NESTED
     * ARRAYACCESS (not an AST_ID) — reading u.id.* off that is a wild
     * type-pun. The entry-not-accessed prune only applies to a direct
     * array ID; for the nested shape fall through to generic recurse
     * (the faithful default — no prune, exactly Python's effect since
     * Python never reaches this branch for that grammar). */
    if (v->cs->opts.optimization_level > 1 && lvalue &&
        lvalue->tag == AST_ID &&
        !lvalue->u.id.accessed) {
        warn_not_used(v->cs, lvalue->lineno,
                      lvalue->u.id.name ? lvalue->u.id.name : "",
                      "Variable"); /* Python default kind= "Variable" */
        AstNode *rhs = node->child_count > 1 ? node->children[1] : NULL;
        return build_side_effect_block(v, rhs, node->lineno, false);
    }
    return visitor_generic(v, node);
}

/* ----------------------------------------------------------------
 * visit_LETSUBSTR (optimize.py:360-365): unused substring assignment
 * → warning + prune to NOP; else recurse. C LETSUBSTR: children[0] is
 * the target ID.
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_letsubstr(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    AstNode *target = node->child_count > 0 ? node->children[0] : NULL;
    if (v->cs->opts.optimization_level > 1 && target &&
        !target->u.id.accessed) {
        warn_not_used(v->cs, target->lineno,
                      target->u.id.name ? target->u.id.name : "",
                      "Variable"); /* Python default kind= "Variable" */
        return c->nop;
    }
    return visitor_generic(v, node);
}

/* ----------------------------------------------------------------
 * visit_RETURN (optimize.py:367-374): visit only the value. Python
 * children[0]=func backref (skipped), [1]=value, gated `len==2`. The
 * C RETURN has only the value as children[0] (no func backref) — so
 * `child_count==1` ⇒ visit children[0]. (See file header.)
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_return(Visitor *v, AstNode *node) {
    if (node->child_count == 1)
        node->children[0] = visitor_visit(v, node->children[0]);
    return node;
}

/* ----------------------------------------------------------------
 * visit_IF (optimize.py:382-408): visit the 3 (or 2) children
 * explicitly. At O>=1: empty then+else → warning + NOP; constant
 * condition with non-jump-targeted body → keep only the taken branch;
 * empty else → drop it. Else write children back.
 * C IF: children[0]=cond, [1]=then, [2]=else? (parser.c:1740-1743).
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_if(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    AstNode *expr_ = node->child_count > 0
                         ? visitor_visit(v, node->children[0]) : NULL;
    AstNode *then_ = node->child_count > 1
                         ? visitor_visit(v, node->children[1]) : NULL;
    AstNode *else_ = (node->child_count == 3)
                         ? visitor_visit(v, node->children[2]) : c->nop;

    if (v->cs->opts.optimization_level >= 1) {
        /* chk.is_null(then_, else_): True if BOTH are null. */
        if (check_is_null(then_) && check_is_null(else_)) {
            warn_empty_if(v->cs, node->lineno);
            return c->nop;
        }

        bool block_accessed =
            is_block_accessed_ctx(v->cs, then_) ||
            is_block_accessed_ctx(v->cs, else_);
        if (!block_accessed && check_is_number(expr_)) {
            /* constant condition: keep only the taken branch. */
            if (expr_->u.number.value != 0)
                return then_;
            return else_;
        }

        if (check_is_null(else_) && node->child_count == 3) {
            block_pop(node, 2); /* remove empty else */
            return node;
        }
    }

    /* Write the (possibly visited) children back. */
    if (node->child_count > 0)
        node->children[0] = expr_;
    if (node->child_count > 1)
        node->children[1] = then_;
    if (node->child_count > 2)
        node->children[2] = else_;
    return node;
}

/* ----------------------------------------------------------------
 * visit_WHILE (optimize.py:410-422): recurse; at O>=1, WHILE 0 with
 * non-jump-targeted body → prune to NOP. C WHILE: children[0]=cond,
 * [1]=body (parser.c:1974-1976).
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_while(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    node = visitor_generic(v, node);
    if (!node || node->tag != AST_SENTENCE)
        return node;
    AstNode *expr_ = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *body_ = node->child_count > 1 ? node->children[1] : NULL;

    if (v->cs->opts.optimization_level >= 1) {
        if (check_is_number(expr_) && expr_->u.number.value == 0 &&
            !is_block_accessed_ctx(v->cs, body_))
            return c->nop;
    }
    return node;
}

/* ----------------------------------------------------------------
 * visit_FOR (optimize.py:424-440): recurse; at O>0, a constant FOR
 * whose bounds never execute the body (and body not jump-targeted) →
 * prune to NOP. C FOR: children[0]=var, [1]=from, [2]=to, [3]=step,
 * [4]=body (parser.c:1912-1932) — matches Python children[1..4].
 * ---------------------------------------------------------------- */
static AstNode *opt_visit_for(Visitor *v, AstNode *node) {
    OptCtx *c = v->ctx;
    node = visitor_generic(v, node);
    if (!node || node->tag != AST_SENTENCE)
        return node;

    AstNode *from_ = node->child_count > 1 ? node->children[1] : NULL;
    AstNode *to_ = node->child_count > 2 ? node->children[2] : NULL;
    AstNode *step_ = node->child_count > 3 ? node->children[3] : NULL;
    AstNode *body_ = node->child_count > 4 ? node->children[4] : NULL;

    if (v->cs->opts.optimization_level > 0 &&
        is_number3(from_, to_, step_) &&
        !is_block_accessed_ctx(v->cs, body_)) {
        double fv = from_->u.number.value;
        double tv = to_->u.number.value;
        double sv = step_->u.number.value;
        if (fv > tv && sv > 0)
            return c->nop;
        if (fv < tv && sv < 0)
            return c->nop;
    }
    return node;
}

/* _visit_LABEL (optimize.py:443-447) is DEAD CODE in Python (leading
 * underscore — the dispatcher looks up visit_LABEL, never dispatched).
 * Deliberately NOT registered: LABEL falls through to generic recurse
 * (capture §1 / mismatch #3). VariableVisitor (post-490) is NOT
 * Phase-2 pass 3 — out of scope (capture §1 / mismatch #5). */

/* Post-pass collector: gather all AST_CALL/AST_FUNCCALL and AST_FUNCDECL
 * nodes (depth-first, unique). The fixed-point bound-status sweep needs
 * to re-touch every call node after the main visit, and the
 * locals_size recompute walks every funcdecl. */
static void opt_collect_calls_decls(AstNode *n, AstPtrVec *calls,
                                    AstPtrVec *decls, AstPtrVec *seen) {
    if (!n) return;
    if (ptr_seen_or_add(seen, n)) return;
    if (n->tag == AST_CALL || n->tag == AST_FUNCCALL)
        vec_push(*calls, n);
    if (n->tag == AST_FUNCDECL)
        vec_push(*decls, n);
    /* Do NOT descend into AST_ID children — they are symbol-table nodes
     * shared across uses; descending hits cycles via funcref->body
     * cycling through the same nodes.  The function bodies are reachable
     * from AST_FUNCDECL->children[2] (body) without traversing the ID. */
    for (int i = 0; i < n->child_count; i++) {
        AstNode *c = n->children[i];
        if (!c) continue;
        if (n->tag == AST_FUNCDECL && i == 0) continue; /* skip entry ID */
        opt_collect_calls_decls(c, calls, decls, seen);
    }
}

/* Post-pass locals_size recompute for a single FUNCDECL.  Mirrors
 * Python optimize.py:486-489's `arg.scope_ref.owner.locals_size =
 * compute_offsets(arg.scope_ref)` side-effect of _update_bound_status:
 * when a LOCAL CLASS_array's lbound_used/ubound_used flips during
 * optimization, its memsize (arrayref.py:230-233: ptr_size *
 * (3 + ubound_used)) changes, and the function's frame size with it.
 * The C parser computed locals_size at end-of-body when those flags
 * were still false; recompute now using a synthetic Scope_ whose
 * `ordered` is the FUNCDECL's captured local_entries (parser.c:5189-
 * 5194, same source compute_offsets used at parse time). */
static void opt_recompute_locals_size(CompilerState *cs, AstNode *decl) {
    if (!decl || decl->tag != AST_FUNCDECL || decl->child_count < 1)
        return;
    AstNode *entry = decl->children[0];
    if (!entry || entry->tag != AST_ID)
        return;
    if (entry->u.id.local_entries_count <= 0)
        return;
    /* fastcall functions emit ic_enter("__fastcall__") and ignore
     * local_size; skip them (translator.c:4046-4053). */
    if (entry->u.id.convention == CONV_fastcall)
        return;
    Scope_ syn;
    memset(&syn, 0, sizeof(syn));
    syn.ordered = entry->u.id.local_entries;
    syn.ordered_count = entry->u.id.local_entries_count;
    int new_size = symboltable_compute_offsets(cs->symbol_table, &syn,
                                               cs->opts.optimization_level);
    entry->u.id.local_size = new_size;
}

void optimizer_run(CompilerState *cs, AstNode *ast) {
    /* Master O-gate — Python visit() override (optimize.py:201-205):
     * at O0 every node is returned unvisited ⇒ the whole pass is a
     * no-op (capture §2). Faithful analogue: no-op optimizer_run. */
    if (cs->opts.optimization_level < 1)
        return;

    Visitor v;
    visitor_init(&v, cs);

    OptCtx ctx;
    vec_init(ctx.visited);
    ctx.nop = ast_new(cs, AST_NOP, ast ? ast->lineno : 0);
    ctx.bound_status_changed = false;
    v.ctx = &ctx;

    visitor_on_tag(&v, AST_UNARY, opt_visit_unary);
    visitor_on_tag(&v, AST_BINARY, opt_visit_binary);
    visitor_on_tag(&v, AST_BUILTIN, opt_visit_builtin);
    visitor_on_tag(&v, AST_CONSTEXPR, opt_visit_constexpr);
    visitor_on_tag(&v, AST_FUNCCALL, opt_visit_call);
    visitor_on_tag(&v, AST_CALL, opt_visit_call); /* visit_CALL == FUNCCALL body */
    visitor_on_tag(&v, AST_FUNCDECL, opt_visit_funcdecl);
    visitor_on_sentence(&v, "LET", opt_visit_let);
    visitor_on_sentence(&v, "LETARRAY", opt_visit_letarray);
    visitor_on_sentence(&v, "LETSUBSTR", opt_visit_letsubstr);
    visitor_on_sentence(&v, "RETURN", opt_visit_return);
    visitor_on_sentence(&v, "IF", opt_visit_if);
    visitor_on_sentence(&v, "WHILE", opt_visit_while);
    visitor_on_sentence(&v, "FOR", opt_visit_for);

    visitor_visit(&v, ast);

    /* ----------------------------------------------------------------
     * Post-pass: transitive `requires`-graph closure of
     * _update_bound_status (api/optimize.py:471-489, driven by
     * symbol_.py:50-58 add_required_symbol's transitive update through
     * `other.requires`).  The single in-order visit above only
     * propagates one CALL deep — when a CALL's argval depends on a
     * param whose body symbol itself only gains its bound flags via a
     * NESTED CALL visited LATER (test3->test2->test1 pattern of
     * ubound11/12 / lbound12), the outer arg never sees the flag.
     *
     * The C has no symbol-graph `requires` field, so emulate it by
     * iterating opt_bound_status_helper_changed over every CALL/FUNCCALL
     * in the AST until no flag changes (monotonic: flags only flip
     * false->true, bounded by 3*|args|).  Then refresh the LOCALS_SIZE
     * on every FUNCDECL whose local CLASS_array's just gained lbound/
     * ubound (Python's "if scope == local and not byref: locals_size =
     * compute_offsets(scope_ref)" side-effect of _update_bound_status).
     * Globals do not need re-anything: var_translator reads the flag
     * directly when emitting the ARRAYDECL descriptor.
     * ---------------------------------------------------------------- */
    AstPtrVec calls;     vec_init(calls);
    AstPtrVec decls;     vec_init(decls);
    AstPtrVec coll_seen; vec_init(coll_seen);
    opt_collect_calls_decls(ast, &calls, &decls, &coll_seen);

    bool any_changed = ctx.bound_status_changed;
    bool changed = true;
    int safety = 0;
    while (changed && safety++ < 1024) {
        changed = false;
        for (int i = 0; i < calls.len; i++) {
            if (opt_bound_status_helper_changed(calls.data[i])) {
                changed = true;
                any_changed = true;
            }
        }
    }

    if (any_changed) {
        for (int i = 0; i < decls.len; i++)
            opt_recompute_locals_size(cs, decls.data[i]);
    }

    vec_free(calls);
    vec_free(decls);
    vec_free(coll_seen);

    vec_free(ctx.visited);
}
