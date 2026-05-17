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

    if (node->u.id.scope == SCOPE_global) {
        /* Python VarRef.t for a global == parent.mangled, unconditionally
         * (varref.py:34-40). The C port only stamps .t at a DIM site; an
         * implicitly-declared global (e.g. a FOR iterator, S5.5) reaches
         * here with .t == NULL. Resolve it to mangled — the faithful
         * VarRef.t — so the consuming op reads "_name", not "". (No S5.3
         * regression: a DIM'd global already has .t == mangled.) */
        if (node->t == NULL && mangled != NULL)
            node->t = (char *)mangled;
        return node;  /* global rvalue: no IC */
    }

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

/* ==================================================================== *
 *  S5.5 — Control Flow Sentences (translator.py:531-748)                *
 * ==================================================================== */

/* The flow-control ic_* helpers (translator_inst_visitor.py:88-227).
 * ic_jzero/jnzero/jgezero use _no_bool(type_); ic_ret uses TSUFFIX. */
static void tr_ic_jump(Translator *tr, const char *label) {
    const char *args[1] = { label };
    tr_emit_quad(tr, "jump", 1, args);
}
static void tr_ic_label(Translator *tr, const char *label) {
    const char *args[1] = { label };
    tr_emit_quad(tr, "label", 1, args);
}
static char *tr_flow_name(Translator *tr, const char *base,
                          const TypeInfo *type_) {
    /* {base}{_no_bool(type_)} (jzero/jnzero/jgezero opcode). */
    const char *suf = tr_no_bool(type_);
    size_t bl = strlen(base), sl = strlen(suf);
    char *r = arena_alloc(&tr->cs->arena, bl + sl + 1);
    memcpy(r, base, bl);
    memcpy(r + bl, suf, sl + 1);
    return r;
}
static void tr_ic_jzero(Translator *tr, const TypeInfo *type_,
                        const char *t, const char *label) {
    const char *args[2] = { t, label };
    tr_emit_quad(tr, tr_flow_name(tr, "jzero", type_), 2, args);
}
static void tr_ic_jnzero(Translator *tr, const TypeInfo *type_,
                         const char *t, const char *label) {
    const char *args[2] = { t, label };
    tr_emit_quad(tr, tr_flow_name(tr, "jnzero", type_), 2, args);
}
static void tr_ic_jgezero(Translator *tr, const TypeInfo *type_,
                          const char *t, const char *label) {
    const char *args[2] = { t, label };
    tr_emit_quad(tr, tr_flow_name(tr, "jgezero", type_), 2, args);
}
static void tr_ic_call(Translator *tr, const char *label, int num) {
    char ns[16];
    snprintf(ns, sizeof(ns), "%d", num);
    const char *args[2] = { label, ns };
    tr_emit_quad(tr, "call", 2, args);
}
static void tr_ic_return(Translator *tr, const char *addr) {
    const char *args[1] = { addr };
    tr_emit_quad(tr, "ret", 1, args);
}
static void tr_ic_ret(Translator *tr, const TypeInfo *type_,
                      const char *t, const char *addr) {
    /* emit(f"ret{TSUFFIX(type_)}", t, addr) */
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "ret%s", suf);
    const char *args[2] = { t, addr };
    tr_emit_quad(tr, base, 2, args);
}
static void tr_ic_leave(Translator *tr, const char *convention) {
    const char *args[1] = { convention };
    tr_emit_quad(tr, "leave", 1, args);
}
/* ic_enter (translator_inst_visitor.py:106-107): emit("enter", arg).
 * arg is the string "__fastcall__" or the integer locals_size coerced
 * to a string (Quad.__init__ str()s every arg). */
static void tr_ic_enter(Translator *tr, const char *arg) {
    const char *args[1] = { arg };
    tr_emit_quad(tr, "enter", 1, args);
}
/* ic_param (translator_inst_visitor.py:208-209): emit(f"param{TSUFFIX}",t) */
static void tr_ic_param(Translator *tr, const TypeInfo *type_, const char *t) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "param%s", suf);
    const char *args[1] = { t };
    tr_emit_quad(tr, base, 1, args);
}
/* ic_fparam (translator_inst_visitor.py:115-116): emit(f"fparam{_no_bool}",t) */
static void tr_ic_fparam(Translator *tr, const TypeInfo *type_, const char *t) {
    const char *args[1] = { t };
    tr_emit_quad(tr, tr_flow_name(tr, "fparam", type_), 1, args);
}

/* runtime_call (translator_visitor.py:119-123): ic_call(label,num); the
 * label->REQUIRES module is wired by the backend's s_runtime_call when the
 * "call" Quad is emitted (REQUIRES set there, mirroring Python adding to
 * backend.REQUIRES). Faithful: the Quad("call",label,num) carries the
 * runtime label; backend.c maps it to its asm module. */
static void tr_runtime_call(Translator *tr, const char *label, int num) {
    tr_ic_call(tr, label, num);
}

/* LabelRef mangling (labelref.py:20): LABELS_NAMESPACE + "." + MANGLE_CHR
 * + name == ".LABEL._<name>". The C parser builds raw AST_ID label nodes
 * (class_==CLASS_label) without the LabelRef-mangled form; compute it
 * here so visit_LABEL/GOTO/GOSUB/ON_* emit the same labels as Python.
 * (gl.LABELS_NAMESPACE=".LABEL" global_.py:139; MANGLE_CHR="_" :167.) */
static const char *tr_label_mangled(Translator *tr, AstNode *id) {
    if (!id) return "";
    const char *name = (id->tag == AST_ID && id->u.id.name) ? id->u.id.name
                     : (id->t ? id->t : "");
    size_t nl = strlen(name);
    char *r = arena_alloc(&tr->cs->arena, nl + 9 /* ".LABEL._" + NUL */);
    memcpy(r, ".LABEL._", 8);
    memcpy(r + 8, name, nl + 1);
    return r;
}

/* loop_exit_label / loop_cont_label (translator.py:1009-1027): scan the
 * LOOPS stack inner->outer for the matching loop_type. */
static const char *tr_loop_exit_label(Translator *tr, const char *kind) {
    for (int i = tr->loops_len - 1; i >= 0; i--)
        if (strcmp(kind, tr->loops[i].kind) == 0)
            return tr->loops[i].end_label;
    fprintf(stderr, "zxbc: InvalidLoopError (%s)\n", kind);
    return "";
}
static const char *tr_loop_cont_label(Translator *tr, const char *kind) {
    for (int i = tr->loops_len - 1; i >= 0; i--)
        if (strcmp(kind, tr->loops[i].kind) == 0)
            return tr->loops[i].cont_label;
    fprintf(stderr, "zxbc: InvalidLoopError (%s)\n", kind);
    return "";
}
static void tr_loops_push(Translator *tr, const char *kind,
                          const char *end_l, const char *cont_l) {
    if (tr->loops_len >= (int)(sizeof(tr->loops) / sizeof(tr->loops[0]))) {
        fprintf(stderr, "zxbc: LOOPS stack overflow\n");
        return;
    }
    tr->loops[tr->loops_len].kind = kind;
    tr->loops[tr->loops_len].end_label = end_l;
    tr->loops[tr->loops_len].cont_label = cont_l;
    tr->loops_len++;
}
static void tr_loops_pop(Translator *tr) {
    if (tr->loops_len > 0) tr->loops_len--;
}

/* emit_let_left_part (translator.py:994-1001): var=children[0],
 * expr=children[1]; t default = expr.t; emit_var_assign(var, t). */
static void tr_emit_let_left_part(Translator *tr, AstNode *node,
                                  const char *t) {
    AstNode *var  = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *expr = node->child_count > 1 ? node->children[1] : NULL;
    if (!var) return;
    if (t == NULL) t = (expr && expr->t) ? expr->t : "";
    tr_emit_var_assign(tr, var, t);
}

/* visit_LABEL (translator.py:106-107): ic_label(node.mangled). The C
 * LABEL sentence's child[0] is the label id. */
static AstNode *tr_visit_label(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *id = node->child_count > 0 ? node->children[0] : NULL;
    tr_ic_label(tr, tr_label_mangled(tr, id));
    return node;
}

/* visit_GOTO (translator.py:651-652): ic_jump(children[0].mangled). */
static AstNode *tr_visit_goto(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *id = node->child_count > 0 ? node->children[0] : NULL;
    tr_ic_jump(tr, tr_label_mangled(tr, id));
    return node;
}

/* visit_GOSUB (translator.py:654-655): ic_call(children[0].mangled, 0). */
static AstNode *tr_visit_gosub(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *id = node->child_count > 0 ? node->children[0] : NULL;
    tr_ic_call(tr, tr_label_mangled(tr, id), 0);
    return node;
}

/* visit_RETURN (translator.py:699-706). The C top-level RETURN (GOSUB
 * return) carries 0 children (no value) or 1 child (a value expr) — it
 * never carries the enclosing-function ref child Python's in-function
 * p_return_expr builds (functions are FunctionTranslator's domain). So:
 *   1 child  (value)  -> Python's len==2 path needs a function mangled
 *                        addr; at top level there is none -> fall to the
 *                        no-child branch shape is wrong. The faithful
 *                        Python p_return at top level produces 0 children
 *                        => visit_RETURN's `else: ic_leave("__fastcall__")`.
 * The C parser attaches a value if present; with no enclosing function
 * the only faithful lowering for top-level RETURN is ic_leave fastcall
 * (mirrors Python p_return's 0-child sentence at global scope). */
static AstNode *tr_visit_return(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    if (node->child_count == 2) {
        /* RETURN value from a SUB/FUNCTION (function-scope; child[0]=func
         * ref, child[1]=value). Faithful to Python len==2. */
        AstNode *func = node->children[0];
        AstNode *val  = node->children[1];
        visitor_visit(v, val);
        const char *fm = func ? tr_label_mangled(tr, func) : "";
        size_t fl = strlen(fm);
        char *addr = arena_alloc(&tr->cs->arena, fl + 8);
        memcpy(addr, fm, fl);
        memcpy(addr + fl, "__leave", 8);
        tr_ic_ret(tr, val ? val->type_ : NULL,
                  (val && val->t) ? val->t : "", addr);
    } else if (node->child_count == 1 &&
               node->children[0] && node->children[0]->tag == AST_ID &&
               node->children[0]->u.id.class_ != CLASS_label) {
        /* len==1: a bare function-ref child -> ic_return(func__leave). */
        AstNode *func = node->children[0];
        const char *fm = tr_label_mangled(tr, func);
        size_t fl = strlen(fm);
        char *addr = arena_alloc(&tr->cs->arena, fl + 8);
        memcpy(addr, fm, fl);
        memcpy(addr + fl, "__leave", 8);
        tr_ic_return(tr, addr);
    } else {
        /* Top-level RETURN (GOSUB return) — Python p_return with no
         * FUNCTION_LEVEL emits a 0-child sentence -> ic_leave fastcall. */
        tr_ic_leave(tr, "__fastcall__");
    }
    return node;
}

/* visit_END (translator.py:65-68) — already ported above as tr_visit_end. */

/* visit_STOP (translator.py:76-81): yield child; ic_fparam(ubyte,child.t);
 * runtime_call(STOP,0); ic_end(0). */
static AstNode *tr_visit_stop(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *code = node->child_count > 0 ? node->children[0] : NULL;
    if (code) visitor_visit(v, code);
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    tr_ic_fparam(tr, ubyte, (code && code->t) ? code->t : "0");
    tr_runtime_call(tr, ".core.__STOP", 0);
    tr_ic_end(tr, "0");
    return node;
}

/* visit_ERROR (translator.py:70-74): yield child; ic_fparam(ubyte,child.t);
 * runtime_call(ERROR,0). */
static AstNode *tr_visit_error(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *code = node->child_count > 0 ? node->children[0] : NULL;
    if (code) visitor_visit(v, code);
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    tr_ic_fparam(tr, ubyte, (code && code->t) ? code->t : "0");
    tr_runtime_call(tr, ".core.__ERROR", 0);
    return node;
}

/* visit_CHKBREAK (translator.py:673-677): if PREV_TOKEN != node.token:
 * ic_inline("push hl"); ic_fparam(PTR_TYPE,child.t); runtime_call(
 * CHECK_BREAK,0). PREV_TOKEN is never assigned (stays None) so the guard
 * is always true. gl.PTR_TYPE == uinteger in the C port. */
static AstNode *tr_visit_chkbreak(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    if (tr->prev_token == NULL ||
        strcmp(tr->prev_token, "CHKBREAK") != 0) {
        AstNode *child = node->child_count > 0 ? node->children[0] : NULL;
        translator_ic_inline(tr, "push hl");
        const TypeInfo *ptr = tr->cs->symbol_table->basic_types[TYPE_uinteger];
        tr_ic_fparam(tr, ptr, (child && child->t) ? child->t : "0");
        tr_runtime_call(tr, ".core.CHECK_BREAK", 0);
    }
    return node;
}

/* visit_IF (translator.py:679-697). children[0]=cond, [1]=THEN block,
 * [2]=ELSE block (optional). The condition type selects the width/float
 * jzero emitter (the nef.bas float-compare-in-IF residual). */
static AstNode *tr_visit_if(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    int n = node->child_count;
    AstNode *cond = n > 0 ? node->children[0] : NULL;
    AstNode *then_b = n > 1 ? node->children[1] : NULL;
    AstNode *else_b = n > 2 ? node->children[2] : NULL;

    if (cond) visitor_visit(v, cond);
    char *if_else  = backend_tmp_label(tr->backend);
    char *if_endif = backend_tmp_label(tr->backend);

    if (n == 3)
        tr_ic_jzero(tr, cond ? cond->type_ : NULL,
                    (cond && cond->t) ? cond->t : "", if_else);
    else
        tr_ic_jzero(tr, cond ? cond->type_ : NULL,
                    (cond && cond->t) ? cond->t : "", if_endif);

    if (then_b) visitor_visit(v, then_b);

    if (n == 3) {
        tr_ic_jump(tr, if_endif);
        tr_ic_label(tr, if_else);
        if (else_b) visitor_visit(v, else_b);
    }
    tr_ic_label(tr, if_endif);
    return node;
}

/* visit_FOR (translator.py:591-649). C FOR children:
 * [0]=iterator VAR, [1]=start, [2]=limit2(TO), [3]=step, [4]=body —
 * identical to Python's child layout (capture §1f). */
static AstNode *tr_visit_for(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *var  = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *start= node->child_count > 1 ? node->children[1] : NULL;
    AstNode *lim2 = node->child_count > 2 ? node->children[2] : NULL;
    AstNode *step = node->child_count > 3 ? node->children[3] : NULL;
    AstNode *body = node->child_count > 4 ? node->children[4] : NULL;
    const TypeInfo *type_ = var ? var->type_ : NULL;

    char *loop_label_start = backend_tmp_label(tr->backend);
    char *loop_label_gt    = backend_tmp_label(tr->backend);
    char *end_loop         = backend_tmp_label(tr->backend);
    char *loop_body        = backend_tmp_label(tr->backend);
    char *loop_continue    = backend_tmp_label(tr->backend);

    tr_loops_push(tr, "FOR", end_loop, loop_continue);

    if (start) visitor_visit(v, start);          /* lower limit */
    tr_emit_let_left_part(tr, node, NULL);       /* store start into var */
    tr_ic_jump(tr, loop_label_start);

    tr_ic_label(tr, loop_body);
    if (body) visitor_visit(v, body);

    tr_ic_label(tr, loop_continue);

    /* VAR = VAR + STEP */
    if (var)  visitor_visit(v, var);
    if (step) visitor_visit(v, step);
    char *t = compiler_new_temp(tr->cs);         /* optemps.new_t() */
    tr_ic_arith(tr, "add", type_, t,
                (var && var->t) ? var->t : "",
                (step && step->t) ? step->t : "");
    tr_emit_let_left_part(tr, node, t);

    tr_ic_label(tr, loop_label_start);

    /* check.is_number(step) or check.is_unsigned(step.type_).
     * is_unsigned(TYPE) hits the bare-except -> always False (capture
     * §1o quirk). So direct = is_number(step). */
    bool direct;
    if (check_is_number(step)) {
        direct = true;
    } else {
        direct = false;
        if (step) visitor_visit(v, step);        /* Step (again) */
        tr_ic_jgezero(tr, type_, (step && step->t) ? step->t : "",
                      loop_label_gt);
    }

    /* step.value (NUMBER literal) for the negative/positive split. */
    double stepval = (step && step->tag == AST_NUMBER)
                   ? step->u.number.value : 0.0;
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];

    if (!direct || stepval < 0) {                /* negative steps */
        if (var)  visitor_visit(v, var);
        if (lim2) visitor_visit(v, lim2);
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_arith(tr, "lt", type_, node->t,
                    (var && var->t) ? var->t : "",
                    (lim2 && lim2->t) ? lim2->t : "");
        tr_ic_jzero(tr, ubyte, node->t, loop_body);
    }

    if (!direct) {
        tr_ic_jump(tr, end_loop);
        tr_ic_label(tr, loop_label_gt);
    }

    if (!direct || stepval >= 0) {               /* positive steps */
        if (var)  visitor_visit(v, var);
        if (lim2) visitor_visit(v, lim2);
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_arith(tr, "gt", type_, node->t,
                    (var && var->t) ? var->t : "",
                    (lim2 && lim2->t) ? lim2->t : "");
        tr_ic_jzero(tr, ubyte, node->t, loop_body);
    }

    tr_ic_label(tr, end_loop);
    tr_loops_pop(tr);
    return node;
}

/* visit_WHILE (translator.py:729-743). C WHILE children: [0]=cond,
 * [1]=body. */
static AstNode *tr_visit_while(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *cond = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *body = node->child_count > 1 ? node->children[1] : NULL;

    char *loop_label = backend_tmp_label(tr->backend);
    char *end_loop   = backend_tmp_label(tr->backend);
    tr_loops_push(tr, "WHILE", end_loop, loop_label);

    tr_ic_label(tr, loop_label);
    if (cond) visitor_visit(v, cond);
    tr_ic_jzero(tr, cond ? cond->type_ : NULL,
                (cond && cond->t) ? cond->t : "", end_loop);

    if (node->child_count > 1 && body) visitor_visit(v, body);

    tr_ic_jump(tr, loop_label);
    tr_ic_label(tr, end_loop);
    tr_loops_pop(tr);
    return node;
}

/* visit_DO_LOOP (translator.py:535-547). C DO_LOOP children: [0]=body
 * (may be absent for an empty loop). */
static AstNode *tr_visit_do_loop(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    char *loop_label = backend_tmp_label(tr->backend);
    char *end_loop   = backend_tmp_label(tr->backend);
    tr_loops_push(tr, "DO", end_loop, loop_label);

    tr_ic_label(tr, loop_label);
    if (node->child_count > 0 && node->children[0])
        visitor_visit(v, node->children[0]);

    tr_ic_jump(tr, loop_label);
    tr_ic_label(tr, end_loop);
    tr_loops_pop(tr);
    return node;
}

/* visit_DO_WHILE / visit_WHILE_DO (translator.py:552-571). Python child
 * layout: children[0]=condition, children[1]=body(optional). token
 * "WHILE_DO" jumps to continue first (pre-test). The C parser kinds:
 *   "DO_WHILE"   = DO WHILE cond ... LOOP   -> Python token WHILE_DO
 *                  (pre-condition; children [pre_cond, body])
 *   "LOOP_WHILE" = DO ... LOOP WHILE cond   -> Python token DO_WHILE
 *                  (post-condition; children [body, post_cond])
 * is_while_do == Python token == "WHILE_DO". cond/body resolved by role
 * to match Python's children[0]=cond, children[1]=body. */
static AstNode *tr_do_while_impl(Visitor *v, AstNode *node,
                                 AstNode *cond, AstNode *body,
                                 bool is_while_do) {
    Translator *tr = v->ctx;
    char *loop_label    = backend_tmp_label(tr->backend);
    char *end_loop      = backend_tmp_label(tr->backend);
    char *continue_loop = backend_tmp_label(tr->backend);

    if (is_while_do)
        tr_ic_jump(tr, continue_loop);

    tr_ic_label(tr, loop_label);
    tr_loops_push(tr, "DO", end_loop, continue_loop);

    if (body) visitor_visit(v, body);            /* Python children[1] */

    tr_ic_label(tr, continue_loop);
    if (cond) visitor_visit(v, cond);            /* Python children[0] */
    tr_ic_jnzero(tr, cond ? cond->type_ : NULL,
                 (cond && cond->t) ? cond->t : "", loop_label);
    tr_ic_label(tr, end_loop);
    tr_loops_pop(tr);
    return node;
}

/* visit_UNTIL_DO / visit_DO_UNTIL (translator.py:549,708-727). Python
 * child layout children[0]=cond, children[1]=body(optional); token
 * "UNTIL_DO" jumps to continue first. C parser kinds:
 *   "DO_UNTIL"   = DO UNTIL cond ... LOOP  -> Python token UNTIL_DO
 *                  (pre-condition; children [pre_cond, body])
 *   "LOOP_UNTIL" = DO ... LOOP UNTIL cond  -> Python token DO_UNTIL
 *                  (post-condition; children [body, post_cond]) */
static AstNode *tr_until_do_impl(Visitor *v, AstNode *node,
                                 AstNode *cond, AstNode *body,
                                 bool is_until_do) {
    Translator *tr = v->ctx;
    char *loop_label    = backend_tmp_label(tr->backend);
    char *end_loop      = backend_tmp_label(tr->backend);
    char *continue_loop = backend_tmp_label(tr->backend);

    if (is_until_do)
        tr_ic_jump(tr, continue_loop);

    tr_ic_label(tr, loop_label);
    tr_loops_push(tr, "DO", end_loop, continue_loop);

    if (body) visitor_visit(v, body);            /* Python children[1] */

    tr_ic_label(tr, continue_loop);
    if (cond) visitor_visit(v, cond);            /* Python children[0] */
    tr_ic_jzero(tr, cond ? cond->type_ : NULL,
                (cond && cond->t) ? cond->t : "", loop_label);
    tr_ic_label(tr, end_loop);
    tr_loops_pop(tr);
    return node;
}

/* Dispatcher for the C parser's DO-family sentence kinds. The C parser
 * collapses pre/post-condition DO loops into distinct kinds + child
 * orders; map each to the faithful Python visitor + token. */
static AstNode *tr_visit_do(Visitor *v, AstNode *node) {
    const char *kind = node->u.sentence.kind;
    int n = node->child_count;
    if (strcmp(kind, "DO_LOOP") == 0)
        return tr_visit_do_loop(v, node);
    if (strcmp(kind, "DO_WHILE") == 0) {
        /* DO WHILE cond ... LOOP : children [pre_cond, body].
         * Python token WHILE_DO (pre-test). */
        AstNode *cond = n > 0 ? node->children[0] : NULL;
        AstNode *body = n > 1 ? node->children[1] : NULL;
        return tr_do_while_impl(v, node, cond, body, true);
    }
    if (strcmp(kind, "LOOP_WHILE") == 0) {
        /* DO ... LOOP WHILE cond : children [body, post_cond].
         * Python token DO_WHILE (post-test). */
        AstNode *body = n > 0 ? node->children[0] : NULL;
        AstNode *cond = n > 1 ? node->children[1] : NULL;
        return tr_do_while_impl(v, node, cond, body, false);
    }
    if (strcmp(kind, "DO_UNTIL") == 0) {
        /* DO UNTIL cond ... LOOP : children [pre_cond, body].
         * Python token UNTIL_DO (pre-test). */
        AstNode *cond = n > 0 ? node->children[0] : NULL;
        AstNode *body = n > 1 ? node->children[1] : NULL;
        return tr_until_do_impl(v, node, cond, body, true);
    }
    if (strcmp(kind, "LOOP_UNTIL") == 0) {
        /* DO ... LOOP UNTIL cond : children [body, post_cond].
         * Python token DO_UNTIL (post-test). */
        AstNode *body = n > 0 ? node->children[0] : NULL;
        AstNode *cond = n > 1 ? node->children[1] : NULL;
        return tr_until_do_impl(v, node, cond, body, false);
    }
    fprintf(stderr, "zxbc: visit_DO unknown kind '%s'\n", kind);
    return node;
}

/* EXIT/CONTINUE family (translator.py:573-589). No fixtures in the
 * functional corpus (capture §6) — ported but exercised only by hand. */
static AstNode *tr_visit_exit_do(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx; tr_ic_jump(tr, tr_loop_exit_label(tr, "DO"));
    return node;
}
static AstNode *tr_visit_exit_while(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx; tr_ic_jump(tr, tr_loop_exit_label(tr, "WHILE"));
    return node;
}
static AstNode *tr_visit_exit_for(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx; tr_ic_jump(tr, tr_loop_exit_label(tr, "FOR"));
    return node;
}
static AstNode *tr_visit_continue_do(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx; tr_ic_jump(tr, tr_loop_cont_label(tr, "DO"));
    return node;
}
static AstNode *tr_visit_continue_while(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx; tr_ic_jump(tr, tr_loop_cont_label(tr, "WHILE"));
    return node;
}
static AstNode *tr_visit_continue_for(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx; tr_ic_jump(tr, tr_loop_cont_label(tr, "FOR"));
    return node;
}

/* visit_ON_GOTO / visit_ON_GOSUB (translator.py:657-671). children[0]=
 * index expr, children[1:]=target LABEL ids. Pushes a JumpTable emitted
 * later by emit_jump_tables. gl.PTR_TYPE == uinteger. */
static AstNode *tr_visit_on_jump(Visitor *v, AstNode *node, bool gosub) {
    Translator *tr = v->ctx;
    AstNode *idx = node->child_count > 0 ? node->children[0] : NULL;
    char *table_label = backend_tmp_label(tr->backend);
    const TypeInfo *ptr = tr->cs->symbol_table->basic_types[TYPE_uinteger];

    /* ic_param(PTR_TYPE, "#" + table_label) */
    {
        size_t tl = strlen(table_label);
        char *hashed = arena_alloc(&tr->cs->arena, tl + 2);
        hashed[0] = '#';
        memcpy(hashed + 1, table_label, tl + 1);
        tr_ic_param(tr, ptr, hashed);
    }
    if (idx) visitor_visit(v, idx);
    tr_ic_fparam(tr, idx ? idx->type_ : NULL,
                 (idx && idx->t) ? idx->t : "0");
    tr_runtime_call(tr, gosub ? ".core.__ON_GOSUB" : ".core.__ON_GOTO", 0);

    /* JUMP_TABLES.append(JumpTable(table_label, children[1:])) */
    if (tr->jump_tables_len <
        (int)(sizeof(tr->jump_tables) / sizeof(tr->jump_tables[0]))) {
        JumpTableEntry *jt = &tr->jump_tables[tr->jump_tables_len++];
        jt->label = table_label;
        int cnt = node->child_count - 1;
        if (cnt < 0) cnt = 0;
        jt->addresses = arena_alloc(&tr->cs->arena,
                                    (size_t)(cnt ? cnt : 1) * sizeof(AstNode *));
        for (int i = 0; i < cnt; i++)
            jt->addresses[i] = node->children[i + 1];
        jt->naddresses = cnt;
    }
    return node;
}
static AstNode *tr_visit_on_goto(Visitor *v, AstNode *node) {
    return tr_visit_on_jump(v, node, false);
}
static AstNode *tr_visit_on_gosub(Visitor *v, AstNode *node) {
    return tr_visit_on_jump(v, node, true);
}

/* emit_jump_tables (translator_visitor.py:160-162):
 *   for t in JUMP_TABLES:
 *     ic_vard(t.label, [f"#{len}"] + [f"##{x.mangled}" for x in t.addresses])
 */
void translator_emit_jump_tables(Translator *tr) {
    for (int i = 0; i < tr->jump_tables_len; i++) {
        JumpTableEntry *jt = &tr->jump_tables[i];
        int n = jt->naddresses;
        /* py_list_repr-style ['#<n>', '##<m>', ...] */
        char head[16];
        snprintf(head, sizeof(head), "#%d", n);
        size_t total = 3 + strlen(head) + 2;
        const char **toks = arena_alloc(&tr->cs->arena,
                                        (size_t)(n + 1) * sizeof(char *));
        toks[0] = arena_strdup(&tr->cs->arena, head);
        for (int k = 0; k < n; k++) {
            const char *m = tr_label_mangled(tr, jt->addresses[k]);
            size_t ml = strlen(m);
            char *hh = arena_alloc(&tr->cs->arena, ml + 3);
            hh[0] = '#'; hh[1] = '#';
            memcpy(hh + 2, m, ml + 1);
            toks[k + 1] = hh;
            total += ml + 4;
        }
        for (int k = 0; k <= n; k++) total += strlen(toks[k]) + 4;
        char *buf = arena_alloc(&tr->cs->arena, total + (size_t)(n + 1) * 4);
        size_t w = 0;
        buf[w++] = '[';
        for (int k = 0; k <= n; k++) {
            if (k) { buf[w++] = ','; buf[w++] = ' '; }
            buf[w++] = '\'';
            size_t l = strlen(toks[k]);
            memcpy(buf + w, toks[k], l); w += l;
            buf[w++] = '\'';
        }
        buf[w++] = ']';
        buf[w] = '\0';
        const char *args[2] = { jt->label, buf };
        tr_emit_quad(tr, "vard", 2, args);
    }
}

/* ==================================================================== *
 *  S5.7a — FunctionTranslator core (function_translator.py)            *
 *                                                                      *
 *  Python's FunctionTranslator subclasses Translator: every non-       *
 *  FUNCDECL sentence/expr handler is shared identically; only          *
 *  visit_FUNCDECL differs (main Translator defers/enqueues             *
 *  top-level; FunctionTranslator enqueues nested) and FunctionTrans-   *
 *  lator adds visit_FUNCTION (the prologue/body/epilogue). The C port  *
 *  mirrors that by registering the same handler table for both, with   *
 *  the FUNCDECL tag pointing at the appropriate enqueue handler.       *
 * ==================================================================== */

/* The pending-function queue push (gl.FUNCTIONS.append, both
 * translator.py:198 and function_translator.py:176). Arena-free growable
 * pointer list scoped to the Translator. */
static void tr_pending_push(Translator *tr, AstNode *fdecl) {
    if (tr->pending_len >= tr->pending_cap) {
        int nc = tr->pending_cap ? tr->pending_cap * 2 : 16;
        AstNode **np = realloc(tr->pending_funcs, (size_t)nc * sizeof(*np));
        if (!np) return;
        tr->pending_funcs = np;
        tr->pending_cap = nc;
    }
    tr->pending_funcs[tr->pending_len++] = fdecl;
}

/* Translator.visit_FUNCDECL (translator.py:196-198) AND
 * FunctionTranslator.visit_FUNCDECL (function_translator.py:174-176):
 * both just append to the (aliased) gl.FUNCTIONS list and emit nothing
 * inline — the function body is NOT walked here (this is exactly why a
 * top-level FUNCDECL produces no inline code, and why an O>1-pruned
 * (NOP'd) function never enters the queue: the optimizer replaced the
 * AST_FUNCDECL with AST_NOP upstream so this handler never fires for
 * it). Identical body for the main-walk and the drain-walk — Python's
 * two methods are byte-equivalent (append node.entry). */
static AstNode *tr_visit_funcdecl(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_pending_push(tr, node);
    return node;
}

/* Shared handler registration (Translator + FunctionTranslator).
 * FUNCDECL always maps to tr_visit_funcdecl (deferral/enqueue) for both
 * roles — Python's Translator.visit_FUNCDECL and
 * FunctionTranslator.visit_FUNCDECL are body-identical. */
static void tr_register_handlers(Visitor *v) {
    visitor_on_tag(v, AST_BLOCK, tr_visit_block);
    visitor_on_tag(v, AST_NUMBER, tr_visit_number);
    visitor_on_tag(v, AST_BINARY, tr_visit_binary);
    visitor_on_tag(v, AST_TYPECAST, tr_visit_typecast);
    visitor_on_tag(v, AST_ID, tr_visit_var);  /* VAR == ID node */
    visitor_on_tag(v, AST_FUNCDECL, tr_visit_funcdecl);
    visitor_on_sentence(v, "LET", tr_visit_let);
    visitor_on_sentence(v, "END", tr_visit_end);
    /* S5.5 control-flow sentence handlers. */
    visitor_on_sentence(v, "IF", tr_visit_if);
    visitor_on_sentence(v, "FOR", tr_visit_for);
    visitor_on_sentence(v, "WHILE", tr_visit_while);
    visitor_on_sentence(v, "DO_LOOP", tr_visit_do);
    visitor_on_sentence(v, "DO_WHILE", tr_visit_do);
    visitor_on_sentence(v, "DO_UNTIL", tr_visit_do);
    visitor_on_sentence(v, "LOOP_WHILE", tr_visit_do);
    visitor_on_sentence(v, "LOOP_UNTIL", tr_visit_do);
    visitor_on_sentence(v, "GOTO", tr_visit_goto);
    visitor_on_sentence(v, "GOSUB", tr_visit_gosub);
    visitor_on_sentence(v, "RETURN", tr_visit_return);
    visitor_on_sentence(v, "LABEL", tr_visit_label);
    visitor_on_sentence(v, "ON_GOTO", tr_visit_on_goto);
    visitor_on_sentence(v, "ON_GOSUB", tr_visit_on_gosub);
    visitor_on_sentence(v, "STOP", tr_visit_stop);
    visitor_on_sentence(v, "ERROR", tr_visit_error);
    visitor_on_sentence(v, "CHKBREAK", tr_visit_chkbreak);
    visitor_on_sentence(v, "EXIT_DO", tr_visit_exit_do);
    visitor_on_sentence(v, "EXIT_WHILE", tr_visit_exit_while);
    visitor_on_sentence(v, "EXIT_FOR", tr_visit_exit_for);
    visitor_on_sentence(v, "CONTINUE_DO", tr_visit_continue_do);
    visitor_on_sentence(v, "CONTINUE_WHILE", tr_visit_continue_while);
    visitor_on_sentence(v, "CONTINUE_FOR", tr_visit_continue_for);
}

void translator_visit(Translator *tr, AstNode *ast) {
    /* reset() analogue (translator_visitor.py:53-61 + common.init): the
     * C Translator is stack-scoped per compile; clear its class-state
     * mirror here. tmp_labels/LABEL_COUNTER live on the Backend and are
     * reset by backend_init (common.init) — not re-zeroed here. */
    tr->loops_len = 0;
    tr->jump_tables_len = 0;
    tr->prev_token = NULL;
    tr->curr_token = NULL;
    tr->pending_funcs = NULL;
    tr->pending_len = 0;
    tr->pending_cap = 0;
    tr->pending_head = 0;

    Visitor v;
    visitor_init(&v, tr->cs);
    v.ctx = tr;
    tr_register_handlers(&v);
    visitor_visit(&v, ast);
}

/* visit_FUNCTION (function_translator.py:49-172) — CORE slice only.
 *
 * Python receives the FUNCTION *entry* (gl.FUNCTIONS holds entries whose
 * .ref carries body/params); the C queue holds the AST_FUNCDECL node
 * (child[0]=entry ID, child[1]=PARAMLIST, child[2]=body) which is the
 * equivalent carrier. node.mangled == entry.mangled ("_<name>").
 *
 * Faithful core sequencing (no-param / no-local-array / non-stdcall):
 *   :52   ic_label(node.mangled)
 *   :53-56 fastcall -> ic_enter("__fastcall__"); else ic_enter(locals_size)
 *   :58-116 local-var/param/local-array init walk  -> OUT OF SCOPE
 *           (S5.7b params, S5.7c local arrays). For the core no-param
 *           no-local function this walk is empty in Python too.
 *   :117-118 for i in node.ref.body: yield i   (body emission)
 *   :120  ic_label("%s__leave" % node.mangled)
 *   :122-164 stdcall-only local string/array teardown -> OUT OF SCOPE
 *           (S5.7b/c); core funcs are fastcall (optimize.py:314-315
 *           forces zero-param/zero-local to fastcall at O>1) or have no
 *           local strings/arrays.
 *   :166-169 fastcall -> ic_leave("__fastcall__"); else
 *            ic_leave(params.size)
 *   :171-172 bound_tables flush -> OUT OF SCOPE (S5.7c local arrays).
 */
static void tr_visit_function(Translator *tr, AstNode *fdecl) {
    AstNode *entry = fdecl->child_count > 0 ? fdecl->children[0] : NULL;
    AstNode *body  = fdecl->child_count > 2 ? fdecl->children[2] : NULL;
    if (!entry) return;

    const char *mangled = entry->u.id.mangled ? entry->u.id.mangled : "";
    bool fastcall = (entry->u.id.convention == CONV_fastcall);

    /* :52 ic_label(node.mangled) */
    tr_ic_label(tr, mangled);

    /* :53-56 ic_enter */
    if (fastcall) {
        tr_ic_enter(tr, "__fastcall__");
    } else {
        char ls[16];
        snprintf(ls, sizeof(ls), "%d", entry->u.id.local_size);
        tr_ic_enter(tr, ls);
    }

    /* :58-116 local-var/param init walk — OUT OF SCOPE for the core
     * no-param no-local path (empty in Python too). S5.7b/c. */

    /* :117-118 body emission — walk each body sentence through the
     * shared handler table (visit_LET/visit_RETURN/visit_CALL/... fire
     * exactly as inherited from Translator). A nested FUNCDECL in the
     * body hits tr_visit_funcdecl -> appended to the same pending queue
     * mid-drain (function_translator.py:174-176 semantics). */
    if (body) {
        Visitor v;
        visitor_init(&v, tr->cs);
        v.ctx = tr;
        tr_register_handlers(&v);
        visitor_visit(&v, body);
    }

    /* :120 ic_label("%s__leave") */
    {
        size_t ml = strlen(mangled);
        char *lv = arena_alloc(&tr->cs->arena, ml + 8);
        memcpy(lv, mangled, ml);
        memcpy(lv + ml, "__leave", 8);
        tr_ic_label(tr, lv);
    }

    /* :122-164 stdcall teardown — OUT OF SCOPE (S5.7b/c). */

    /* :166-169 ic_leave */
    if (fastcall) {
        tr_ic_leave(tr, "__fastcall__");
    } else {
        char ps[16];
        snprintf(ps, sizeof(ps), "%d", entry->u.id.param_size);
        tr_ic_leave(tr, ps);
    }

    /* :171-172 bound_tables flush — OUT OF SCOPE (S5.7c). */
}

void translator_function_start(Translator *tr) {
    /* FunctionTranslator.start() (function_translator.py:43-47):
     *   while self.functions: f = self.functions.pop(0); self.visit(f)
     * FIFO drain. visit_FUNCTION is a Python generator whose body walk
     * appends nested FUNCDECLs to the same list mid-iteration, so the
     * loop continues until the transitive closure is exhausted. The C
     * port uses a head cursor over the growable list so nested pushes
     * (which may realloc) are observed by the still-running loop. */
    while (tr->pending_head < tr->pending_len) {
        AstNode *fdecl = tr->pending_funcs[tr->pending_head++];
        if (fdecl)
            tr_visit_function(tr, fdecl);
    }
    free(tr->pending_funcs);
    tr->pending_funcs = NULL;
    tr->pending_len = tr->pending_cap = tr->pending_head = 0;
}
