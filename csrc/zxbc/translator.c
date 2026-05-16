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
#include <string.h>
#include <stdlib.h>

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

/* Public emit (var_translator.c reuses it). See translator.h. */
void tr_emit_quad(Translator *tr, const char *instr, int nargs,
                  const char *const *args) {
    tr_emit(tr, instr, nargs, args);
}

/* ---- TranslatorInstVisitor.TSUFFIX (translator_inst_visitor.py:27-51) --
 * Resolve a TypeInfo (TYPE/TYPEREF/BASICTYPE) through .final to its
 * BASICTYPE, then map to the DataType string. _TSUFFIX dict at :31-42:
 *   byte->i8 ubyte->u8 integer->i16 uinteger->u16 long->i32 ulong->u32
 *   fixed->f16 float->f string->str boolean->bool
 * (common.py:34-44 DataType values). */
const char *tr_tsuffix(const TypeInfo *type_) {
    BasicType bt = TYPE_unknown;
    if (type_ != NULL) {
        const TypeInfo *f = type_->final_type ? type_->final_type : type_;
        bt = f->basic_type;
    }
    switch (bt) {
        case TYPE_byte:     return "i8";
        case TYPE_ubyte:    return "u8";
        case TYPE_integer:  return "i16";
        case TYPE_uinteger: return "u16";
        case TYPE_long:     return "i32";
        case TYPE_ulong:    return "u32";
        case TYPE_fixed:    return "f16";
        case TYPE_float:    return "f";
        case TYPE_string:   return "str";
        case TYPE_boolean:  return "bool";
        default:
            /* Python asserts TYPE.is_valid; an invalid type here is a real
             * codegen bug — fail loud, do not silently emit garbage. */
            fprintf(stderr, "zxbc: TSUFFIX on invalid type (basic=%d)\n",
                    (int)bt);
            return "u8";
    }
}

/* TranslatorInstVisitor._no_bool (translator_inst_visitor.py:53-56): the
 * suffix, except bool maps to u8. (Used by and/or/xor/not/fparam/jzero. */
static const char *tr_no_bool(const TypeInfo *type_) {
    const char *s = tr_tsuffix(type_);
    return (s[0] == 'b') ? "u8" : s; /* "bool" -> "u8" */
}

/* {base}{TSUFFIX} instruction-name builder (the ic_add/ic_sub/... shape). */
static char *tr_ins_name(Translator *tr, const char *base,
                         const TypeInfo *type_) {
    const char *suf = tr_tsuffix(type_);
    size_t bl = strlen(base), sl = strlen(suf);
    char *r = arena_alloc(&tr->cs->arena, bl + sl + 1);
    memcpy(r, base, bl);
    memcpy(r + bl, suf, sl + 1);
    return r;
}

/* ic_store (translator_inst_visitor.py:235-236): emit(f"store{TSUFFIX}",t1,t2) */
static void tr_ic_store(Translator *tr, const TypeInfo *type_,
                        const char *t1, const char *t2) {
    const char *args[2] = { t1, t2 };
    tr_emit(tr, tr_ins_name(tr, "store", type_), 2, args);
}

/* ic_add/ic_sub/ic_mul/ic_div (translator_inst_visitor.py:64,238,178,100):
 * emit(f"{op}{TSUFFIX(type_)}", t, t1, t2) — type_ is node.left.type_. */
static void tr_ic_arith(Translator *tr, const char *op,
                        const TypeInfo *type_, const char *t,
                        const char *t1, const char *t2) {
    const char *args[3] = { t, t1, t2 };
    tr_emit(tr, tr_ins_name(tr, op, type_), 3, args);
}

/* ic_cast (translator_inst_visitor.py:91-92):
 *   emit("cast", t1, TSUFFIX(type1), TSUFFIX(type2), t2) */
static void tr_ic_cast(Translator *tr, const char *t1,
                       const TypeInfo *type1, const TypeInfo *type2,
                       const char *t2) {
    const char *args[4] = { t1, tr_tsuffix(type1), tr_tsuffix(type2), t2 };
    tr_emit(tr, "cast", 4, args);
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

/* visit_VAR (translator.py:112-128). Global-var rvalue fast path: when
 * node.t == node.mangled and scope == global_, Python returns immediately
 * (the value is read directly from memory by the consuming op via its
 * mangled label — no load IC). parameter/local pload paths are S5.5+
 * (out of the S5.3 integer-scalar-global scope); reaching them here is a
 * real gap, so fail loud rather than emit nothing silently. */
static AstNode *tr_visit_var(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const char *t = node->t;
    const char *mangled = node->u.id.mangled;
    if (t && mangled && strcmp(t, mangled) == 0 &&
        node->u.id.scope == SCOPE_global)
        return node;                       /* global rvalue: no IC */

    if (node->u.id.scope == SCOPE_global)
        return node;  /* non-mangled global (shouldn't happen for scalars) */

    /* SCOPE.parameter / SCOPE.local => ic_pload (translator.py:124-128).
     * Not in the S5.3 slice. */
    fprintf(stderr,
            "zxbc: visit_VAR non-global scope (%d) not in S5.3 scope\n",
            (int)node->u.id.scope);
    (void)tr;
    return node;
}

/* emit_var_assign (translator.py:975-992) + emit_let_left_part (:994-1001).
 * For the S5.3 global-scalar slice: O>1 & not accessed -> return;
 * global -> ic_store(var.type_, var.mangled, t). */
static void tr_emit_var_assign(Translator *tr, AstNode *var, const char *t) {
    if (tr->cs->opts.optimization_level > 1 && !var->u.id.accessed)
        return;
    if (var->u.id.scope == SCOPE_global) {
        tr_ic_store(tr, var->type_, var->u.id.mangled, t);
        return;
    }
    /* parameter/local pstore — S5.5+. Loud (not silent) outside S5.3. */
    fprintf(stderr,
            "zxbc: emit_var_assign non-global scope (%d) not in S5.3 scope\n",
            (int)var->u.id.scope);
}

/* visit_LET (translator.py:83-88). children[0]=VAR lvalue, [1]=expr.
 * If O<2 or lvalue.accessed or expr.token=="CONSTEXPR": yield expr (visit
 * RHS) — else skip it (Python's generator simply doesn't yield it). Then
 * emit_let_left_part(node) -> emit_var_assign(var, expr.t). */
static AstNode *tr_visit_let(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *var  = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *expr = node->child_count > 1 ? node->children[1] : NULL;
    if (!var || !expr) return node;

    bool expr_is_constexpr = (expr->tag == AST_CONSTEXPR);
    if (tr->cs->opts.optimization_level < 2 ||
        var->u.id.accessed || expr_is_constexpr)
        visitor_visit(v, expr);

    /* emit_let_left_part: t = expr.t (translator.py:998-999) */
    tr_emit_var_assign(tr, var, expr->t ? expr->t : "");
    return node;
}

/* visit_BINARY (translator.py:161-188). GENERIC dispatch (no per-op
 * visitors): yield left; yield right; then
 *   ins = {"PLUS":"add","MINUS":"sub"}.get(op, op.lower())
 *   ins_t[ins](left.type_, node.t, str(left.t), str(right.t))
 * The C BINARY operator strings are token names (parser.c operator_name);
 * note C "MULT" == Python token "MUL" — both lower to ic base "mul".
 * Map every operator faithfully; only add/sub/mul/div have S5.3 emitters
 * (others are out-of-S5.3-scope and would loudly KeyError, as in Python
 * if its ins_t lacked them — they never appear in the S5.3 corpus). */
static const char *tr_binary_ic_base(const char *op) {
    if (strcmp(op, "PLUS")  == 0) return "add";
    if (strcmp(op, "MINUS") == 0) return "sub";
    if (strcmp(op, "MULT")  == 0) return "mul"; /* Python token "MUL" */
    if (strcmp(op, "MUL")   == 0) return "mul";
    if (strcmp(op, "DIV")   == 0) return "div";
    if (strcmp(op, "MOD")   == 0) return "mod";
    if (strcmp(op, "POW")   == 0) return "pow";
    if (strcmp(op, "EQ")    == 0) return "eq";
    if (strcmp(op, "NE")    == 0) return "ne";
    if (strcmp(op, "GT")    == 0) return "gt";
    if (strcmp(op, "LT")    == 0) return "lt";
    if (strcmp(op, "LE")    == 0) return "le";
    if (strcmp(op, "GE")    == 0) return "ge";
    if (strcmp(op, "OR")    == 0) return "or";
    if (strcmp(op, "AND")   == 0) return "and";
    if (strcmp(op, "XOR")   == 0) return "xor";
    if (strcmp(op, "BOR")   == 0) return "bor";
    if (strcmp(op, "BAND")  == 0) return "band";
    if (strcmp(op, "BXOR")  == 0) return "bxor";
    if (strcmp(op, "SHL")   == 0) return "shl";
    if (strcmp(op, "SHR")   == 0) return "shr";
    return NULL;
}

static AstNode *tr_visit_binary(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *left  = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *right = node->child_count > 1 ? node->children[1] : NULL;
    if (left)  visitor_visit(v, left);
    if (right) visitor_visit(v, right);

    /* node.t (translator.py:188): Symbol.t lazily allocates optemps.new_t()
     * on first access (symbol_.py:72-76). visit_BINARY is that first
     * access for a BINARY result; mirror the lazy allocation here so the
     * temp number matches Python (the counter was pre-seeded in codegen
     * to Python's pre-Translator value). */
    if (node->t == NULL)
        node->t = compiler_new_temp(tr->cs);

    const char *op = node->u.binary.operator ? node->u.binary.operator : "";
    const char *base = tr_binary_ic_base(op);
    if (base == NULL) {
        fprintf(stderr, "zxbc: visit_BINARY unknown operator '%s'\n", op);
        return node;
    }
    /* ins_t[ins](node.left.type_, node.t, str(node.left.t),
     *            str(node.right.t)).  ic_and/ic_or/ic_xor use _no_bool
     *  (translator_inst_visitor.py:70,193,250); the rest use TSUFFIX. */
    const TypeInfo *ltype = left ? left->type_ : NULL;
    const char *lt = (left  && left->t)  ? left->t  : "";
    const char *rt = (right && right->t) ? right->t : "";
    const char *t  = node->t ? node->t : "";

    bool uses_no_bool = (strcmp(base, "or")  == 0 ||
                         strcmp(base, "xor") == 0 ||
                         strcmp(base, "and") == 0);
    if (uses_no_bool) {
        const char *suf = tr_no_bool(ltype);
        size_t bl = strlen(base), sl = strlen(suf);
        char *ins = arena_alloc(&tr->cs->arena, bl + sl + 1);
        memcpy(ins, base, bl);
        memcpy(ins + bl, suf, sl + 1);
        const char *args[3] = { t, lt, rt };
        tr_emit(tr, ins, 3, args);
    } else {
        tr_ic_arith(tr, base, ltype, t, lt, rt);
    }
    return node;
}

/* visit_TYPECAST (translator.py:190-194): yield operand;
 *   ic_cast(node.t, operand.type_, node.type_, operand.t) */
static AstNode *tr_visit_typecast(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *operand = node->child_count > 0 ? node->children[0] : NULL;
    if (operand) visitor_visit(v, operand);
    /* node.t — lazy optemps.new_t() (symbol_.py:72-76); visit_TYPECAST
     * is its first access (translator.py:194). */
    if (node->t == NULL)
        node->t = compiler_new_temp(tr->cs);
    tr_ic_cast(tr, node->t ? node->t : "",
               operand ? operand->type_ : NULL, node->type_,
               (operand && operand->t) ? operand->t : "");
    return node;
}

void translator_visit(Translator *tr, AstNode *ast) {
    Visitor v;
    visitor_init(&v, tr->cs);
    v.ctx = tr;
    visitor_on_tag(&v, AST_BLOCK, tr_visit_block);
    visitor_on_tag(&v, AST_NUMBER, tr_visit_number);
    visitor_on_tag(&v, AST_BINARY, tr_visit_binary);
    visitor_on_tag(&v, AST_TYPECAST, tr_visit_typecast);
    visitor_on_tag(&v, AST_ID, tr_visit_var);  /* VAR == ID node */
    visitor_on_sentence(&v, "LET", tr_visit_let);
    visitor_on_sentence(&v, "END", tr_visit_end);
    visitor_visit(&v, ast);
}
