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

/* S5.7d — the param/local IX-relative IC wrappers are defined below their
 * first callers (tr_visit_var :220ish, tr_emit_var_assign :246ish);
 * forward-declare so those callers see prototypes. */
static void tr_ic_pload(Translator *tr, const TypeInfo *type_,
                        const char *t1, const char *offset);
static void tr_ic_pstore(Translator *tr, const TypeInfo *type_,
                         const char *offset, const char *t);

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

/* Python repr of a list[str]: "['a', 'b']" — Quad str()-coerces a list
 * arg to exactly this (interface/quad.py:27). Single quotes, ", " sep;
 * the S5.x element strings never contain quotes/backslashes (hex bytes,
 * labels, "#lit"). Shared by var_translator.c and the S5.7d local-init
 * IC wrappers so both produce byte-identical reprs. */
char *tr_py_list_repr(Translator *tr, char **items, int n) {
    size_t total = 3; /* "[" + "]" + NUL */
    for (int i = 0; i < n; i++)
        total += strlen(items[i]) + 2 /* quotes */ + (i ? 2 : 0) /* ", " */;
    char *buf = arena_alloc(&tr->cs->arena, total);
    size_t w = 0;
    buf[w++] = '[';
    for (int i = 0; i < n; i++) {
        if (i) { buf[w++] = ','; buf[w++] = ' '; }
        buf[w++] = '\'';
        size_t l = strlen(items[i]);
        memcpy(buf + w, items[i], l); w += l;
        buf[w++] = '\'';
    }
    buf[w++] = ']';
    buf[w] = '\0';
    return buf;
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

    /* S5.7d — SCOPE.parameter / SCOPE.local => ic_pload (translator.py
     * :122-128). p = "*" if byref else "". parameter => +offset, local
     * => -offset. node.t is the destination temp; offset is the signed
     * IX-relative stack slot the offset model assigned. */
    {
        const char *pfx = node->u.id.byref ? "*" : "";
        long off = node->u.id.offset;
        if (node->u.id.scope == SCOPE_local) off = -off;
        char ofs[24];
        snprintf(ofs, sizeof(ofs), "%s%ld", pfx, off);
        const char *dst = (node->t && node->t[0]) ? node->t
                        : compiler_new_temp(tr->cs);
        node->t = (char *)dst;
        tr_ic_pload(tr, node->type_, dst, ofs);
    }
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
    /* S5.7d — parameter/local pstore (translator.py:989-992). p = "*" if
     * byref else "". parameter => +offset, local => -offset. */
    {
        const char *pfx = var->u.id.byref ? "*" : "";
        long off = var->u.id.offset;
        if (var->u.id.scope == SCOPE_local) off = -off;
        char ofs[24];
        snprintf(ofs, sizeof(ofs), "%s%ld", pfx, off);
        tr_ic_pstore(tr, var->type_, ofs, t);
    }
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

/* ic_load (translator_inst_visitor.py:160-161): emit(f"load{TSUFFIX}",t1,t2).
 * Used by visit_ARGUMENT's byref-global arm: ic_load(uinteger,t,"#"+mangled). */
static void tr_ic_load(Translator *tr, const TypeInfo *type_,
                       const char *t1, const char *t2) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "load%s", suf);
    const char *args[2] = { t1, t2 };
    tr_emit_quad(tr, base, 2, args);
}
/* S5.7d — ic_paddr / ic_pload / ic_pstore (translator_inst_visitor.py
 * :201-202, :214-215, :218-219). The inside-function parameter/local
 * IX-relative address+load+store surface. Now landed with their first
 * real callers: the offset model + visit_VAR/emit_var_assign param/local
 * branches (the corpus's body-read/write fixtures). Backend _paddr /
 * _pload* / _pstore* are S5.7c-shipped & dispatch-reachable. `offset` is
 * the already-signed string ("*"-prefixed for byref): param => +offset,
 * local => -offset (translator.py:124-128). */
static void tr_ic_pload(Translator *tr, const TypeInfo *type_,
                        const char *t1, const char *offset) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "pload%s", suf);
    const char *args[2] = { t1, offset };
    tr_emit_quad(tr, base, 2, args);
}
static void tr_ic_pstore(Translator *tr, const TypeInfo *type_,
                         const char *offset, const char *t) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "pstore%s", suf);
    const char *args[2] = { offset, t };
    tr_emit_quad(tr, base, 2, args);
}
/* ic_paddr (translator_inst_visitor.py:201-202): emit("paddr", t1, t2).
 * S5.7d wires its first real caller — visit_ARGUMENT's byref
 * parameter/local arms (translator.py:240-245), a clean offset-model
 * sibling consumer. Python passes (offset:int, t); the offset is
 * str()-coerced by Quad. backend _paddr is S5.7c-shipped. */
static void tr_ic_paddr(Translator *tr, long offset, const char *t) {
    char ofs[24];
    snprintf(ofs, sizeof(ofs), "%ld", offset);
    const char *args[2] = { ofs, t };
    tr_emit_quad(tr, "paddr", 2, args);
}

/* ic_larrd / ic_lvard / ic_lvarx (translator_inst_visitor.py:148-149,
 * :166-170). The FunctionTranslator :58-116 local-init walk's emitters.
 * Backend _larrd/_lvard/_lvarx are S5.7c-shipped & dispatch-reachable.
 * The list-shaped args are str()-coerced by Quad to a Python list repr —
 * reuse var_translator.c's py_list_repr via the public emitter, building
 * the repr identically. */
static void tr_ic_lvard(Translator *tr, int offset, char **data, int n) {
    char off[16];
    snprintf(off, sizeof(off), "%d", offset);
    const char *args[2] = { off, tr_py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "lvard", 2, args);
}
static void tr_ic_lvarx(Translator *tr, const TypeInfo *type_, int offset,
                        char **data, int n) {
    char off[16];
    snprintf(off, sizeof(off), "%d", offset);
    const char *args[3] = { off, tr_tsuffix(type_),
                            tr_py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "lvarx", 3, args);
}
/* ic_larrd(offset, arg1, size, arg2, bound_ptrs) — arg1=idx-table list,
 * arg2=init-image list, bound_ptrs=[lbound,ubound] str list. */
static void tr_ic_larrd(Translator *tr, int offset, char **idx, int idxn,
                        long size, char **init, int initn,
                        char **bptrs, int bptrn) {
    char off[16], sz[16];
    snprintf(off, sizeof(off), "%d", offset);
    snprintf(sz, sizeof(sz), "%ld", size);
    const char *args[5] = { off, tr_py_list_repr(tr, idx, idxn), sz,
                            tr_py_list_repr(tr, init, initn),
                            tr_py_list_repr(tr, bptrs, bptrn) };
    tr_emit_quad(tr, "larrd", 5, args);
}

/* runtime_call (translator_visitor.py:119-123): ic_call(label,num); the
 *
 * runtime_call (translator_visitor.py:119-123): ic_call(label,num); the
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

/* ==================================================================== *
 *  S5.7b — caller-side param/ABI (shared Translator handlers)          *
 *                                                                      *
 *  visit_CALL / visit_FUNCCALL / visit_ARGLIST / visit_ARGUMENT        *
 *  (translator.py:200-263, :470-477). Faithful to the Python           *
 *  generators' EXACT traversal: the handler fully controls which       *
 *  children are visited (Python's NodeVisitor.visit drives the         *
 *  generator; there is NO automatic generic_visit of a node that has   *
 *  a visit_<token>). In particular visit_CALL/visit_FUNCCALL           *
 *  `yield node.args` ONLY — they NEVER visit node.entry (the callee     *
 *  ID). The callee ID for a self-/mutually-recursive function is the   *
 *  SAME shared symbol-table node the FUNCDECL carries; walking it (or   *
 *  letting generic descent reach it) is exactly the unbounded re-entry  *
 *  the first S5.7b attempt hit on warn_unreach0. We therefore visit     *
 *  ONLY child[1] (the ARGLIST), never child[0].                        *
 * ==================================================================== */

/* visit_ARGUMENT (translator.py:214-263). C ARGUMENT: child[0]=value
 * expr, u.argument.byref. ARGUMENT.t==value.t, .type_==value.type_
 * (argument.py:28-50). The dynamic-`$`-string heap-dup (`:216-222`) and
 * the byref arms that read an enclosing param/local `node.value.offset`
 * (`:238-245`) need the inside-function stack-offset model that S5.7b
 * explicitly defers — emit a loud stderr residue note and fall back to
 * the faithful byval value+param shape rather than emit wrong code. The
 * byval-scalar arm (the corpus-dominant case) is exact. */
static AstNode *tr_visit_argument(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *value = node->child_count > 0 ? node->children[0] : NULL;
    bool byref = node->u.argument.byref;
    const TypeInfo *uint_t =
        tr->cs->symbol_table->basic_types[TYPE_uinteger]; /* gl.PTR_TYPE */

    if (!byref) {
        /* :216-222 dynamic `$`-string VAR heap-dup — needs value.offset
         * (param/local). Deferred (S5.8 string ABI). Loud, not silent. */
        if (value && value->tag == AST_ID && value->type_ &&
            value->type_->basic_type == TYPE_string &&
            value->u.id.scope != SCOPE_global) {
            fprintf(stderr,
                "zxbc: visit_ARGUMENT dynamic-$string byval (scope %d) "
                "param/local offset — S5.8 string ABI residue\n",
                (int)value->u.id.scope);
        }
        /* :224 yield node.value */
        if (value) visitor_visit(v, value);
        /* :225 ic_param(node.type_, node.t) — type_/t are value's. */
        tr_ic_param(tr, value ? value->type_ : NULL,
                    (value && value->t) ? value->t : "");
        return node;
    }

    /* ByRef. :229-248 non-array byref: address of the argument. The
     * global arm (`:236-237` ic_load(uinteger,t,"#"+mangled)) is exact;
     * the parameter/local arms (`:238-245`) read node.value.offset (the
     * inside-function stack model S5.7b defers) — loud residue + the
     * faithful global-shaped fallback (never wrong code). */
    if (value && value->tag == AST_ID && value->u.id.scope == SCOPE_global) {
        const char *mg = value->u.id.mangled ? value->u.id.mangled : "";
        size_t ml = strlen(mg);
        char *hm = arena_alloc(&tr->cs->arena, ml + 2);
        hm[0] = '#';
        memcpy(hm + 1, mg, ml + 1);
        const char *t = (value->t && value->t[0]) ? value->t
                       : compiler_new_temp(tr->cs);
        tr_ic_load(tr, uint_t, t, hm);          /* :237 */
        tr_ic_param(tr, uint_t, t);             /* :247 */
        return node;
    }

    /* S5.7d — byref parameter / local arm (translator.py:238-245). The
     * offset model now feeds value.offset. Non-array only (ARRAYLOAD /
     * ARRAYACCESS @array ADDRESS is the S5.8 residue, still loud below).
     * Python: t = node.t if node.t[0] != "_" else optemps.new_t().
     * parameter & not byref => offset adj = 1 for (u)byte else 0,
     *   ic_paddr(value.offset + adj, t); parameter & byref =>
     *   ic_pload(PTR_TYPE, t, str(value.offset)); local =>
     *   ic_paddr(-value.offset, t). Then ic_param(uinteger, t). */
    if (value && value->tag == AST_ID &&
        (value->u.id.scope == SCOPE_parameter ||
         value->u.id.scope == SCOPE_local)) {
        const char *nt = node->t;
        const char *t = (nt && nt[0] && nt[0] != '_')
                       ? nt : compiler_new_temp(tr->cs);
        if (value->u.id.scope == SCOPE_parameter) {
            if (!value->u.id.byref) {
                long adj = 0;
                const TypeInfo *vt = value->type_;
                if (vt) {
                    const TypeInfo *f = vt->final_type ? vt->final_type : vt;
                    if (f->basic_type == TYPE_byte ||
                        f->basic_type == TYPE_ubyte)
                        adj = 1;
                }
                tr_ic_paddr(tr, (long)value->u.id.offset + adj, t);
            } else {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "%d", value->u.id.offset);
                tr_ic_pload(tr, uint_t, t, ofs);
            }
        } else { /* SCOPE_local */
            tr_ic_paddr(tr, -(long)value->u.id.offset, t);
        }
        tr_ic_param(tr, uint_t, t);
        return node;
    }

    /* byref array (`:250-263` ADDRESS UNARY) — needs the @array ADDRESS
     * lowering (S5.8). Loud residue; do NOT emit wrong code. */
    fprintf(stderr,
        "zxbc: visit_ARGUMENT byref array — @array ADDRESS is S5.8 "
        "residue\n");
    if (value) visitor_visit(v, value);
    return node;
}

/* visit_ARGLIST (translator.py:210-212): visit children in REVERSE
 * order (last arg pushed first → first arg ends on top, matching the
 * callee's IX+offset layout). */
static AstNode *tr_visit_arglist(Visitor *v, AstNode *node) {
    for (int i = node->child_count - 1; i >= 0; i--)
        visitor_visit(v, node->children[i]);
    return node;
}

/* Shared CALL / FUNCCALL lowering. is_funccall selects the Python
 * variant: visit_CALL (translator.py:200-208) calls with size 0 (discard
 * return) and frees a returned string; visit_FUNCCALL
 * (translator.py:470-477) calls with node.entry.size (return bytes).
 *
 * CALLEE-CLASS GATE: Python only ever builds CALL/FUNCCALL for an actual
 * callable. The C parser also tags string-slice / unresolved forms as
 * FUNCCALL; emitting an ic_call for those regressed const_str1 (first
 * attempt). Gate strictly on the callee being CLASS_function/CLASS_sub;
 * otherwise fall back to generic descent (faithful no-op for codegen,
 * the value paths own those nodes). */
static AstNode *tr_visit_call_common(Visitor *v, AstNode *node,
                                     bool is_funccall) {
    Translator *tr = v->ctx;
    AstNode *callee = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *args   = node->child_count > 1 ? node->children[1] : NULL;

    if (!callee || callee->tag != AST_ID ||
        (callee->u.id.class_ != CLASS_function &&
         callee->u.id.class_ != CLASS_sub)) {
        /* Not a callable. The C parser tags string-slice `s(i)` on a
         * CONST/VAR string (and the unresolved no-parens sentence-CALL
         * id) as FUNCCALL/CALL too; Python never builds a CALL/FUNCCALL
         * visitor invocation for those (they are STRSLICE / value-path /
         * later-slice owned). Emitting param pushes + ic_call here is the
         * const_str1 regression. The faithful choice is to emit NOTHING
         * and NOT descend (descending would fire tr_visit_argument and
         * push orphan params with no matching call). Loud residue note,
         * no wrong code — and crucially the callee (child[0]) is never
         * walked, so no unbounded re-entry. */
        fprintf(stderr,
            "zxbc: %s callee class %d not CLASS_function/sub "
            "(string-slice / unresolved) — codegen residue (no IC)\n",
            is_funccall ? "FUNCCALL" : "CALL",
            (callee && callee->tag == AST_ID) ? (int)callee->u.id.class_ : -1);
        return node;
    }

    /* :201 / :471  yield node.args  — visit ONLY the ARGLIST. The callee
     * (child[0]) is NEVER visited (Python's generator never yields it);
     * this is the unbounded-re-entry fix. */
    if (args) visitor_visit(v, args);

    /* :202-204 / :473-475  fastcall first-arg → ic_fparam. node.args[0]
     * is the first ARGUMENT; its type_ == value.type_. */
    if (callee->u.id.convention == CONV_fastcall && args &&
        args->child_count > 0) {
        AstNode *a0 = args->children[0];
        AstNode *a0v = (a0 && a0->child_count > 0) ? a0->children[0] : NULL;
        tr_ic_fparam(tr, a0v ? a0v->type_ : NULL,
                     compiler_new_temp(tr->cs)); /* optemps.new_t() */
    }

    const char *mg = callee->u.id.mangled ? callee->u.id.mangled : "";

    if (is_funccall) {
        /* :477 ic_call(entry.mangled, entry.size) — size == type_.size
         * (FunctionRef.size, funcref.py:70-74). */
        int sz = callee->type_ ? type_size(callee->type_) : 0;
        tr_ic_call(tr, mg, sz);
    } else {
        /* :206 ic_call(entry.mangled, 0) — procedure, discard return. */
        tr_ic_call(tr, mg, 0);
        /* :207-208 discard a returned string. */
        if (callee->u.id.class_ == CLASS_function && callee->type_ &&
            callee->type_->basic_type == TYPE_string) {
            tr_runtime_call(tr, ".core.__MEM_FREE", 0);
        }
    }
    return node;
}

static AstNode *tr_visit_call(Visitor *v, AstNode *node) {
    return tr_visit_call_common(v, node, false);
}
static AstNode *tr_visit_funccall(Visitor *v, AstNode *node) {
    return tr_visit_call_common(v, node, true);
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

/* S5.7b — processed-function guard. Returns true (and records it) the
 * FIRST time `entry` is seen; true thereafter means "already emitted —
 * skip". Mirrors Python's de-facto invariant that each function body is
 * emitted exactly once (one FUNCDECL per function reaches gl.FUNCTIONS,
 * drained once). Same ptr-identity-set shape as the optimizer's
 * ptr_seen_or_add (optimizer.c:122-128) / FunctionGraph's visited set —
 * an unconditional bound on visit_FUNCTION re-entry regardless of how a
 * function-entry got (re-)queued (declare+def, caller paths, nested). */
static bool tr_func_already_emitted(Translator *tr, AstNode *entry) {
    for (int i = 0; i < tr->emitted_len; i++)
        if (tr->emitted_funcs[i] == entry)
            return true;
    if (tr->emitted_len >= tr->emitted_cap) {
        int nc = tr->emitted_cap ? tr->emitted_cap * 2 : 16;
        AstNode **np = realloc(tr->emitted_funcs, (size_t)nc * sizeof(*np));
        if (!np) return false;
        tr->emitted_funcs = np;
        tr->emitted_cap = nc;
    }
    tr->emitted_funcs[tr->emitted_len++] = entry;
    return false;
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
    /* S5.7b — a C-port-only forward-declaration FUNCDECL (synthesised for
     * `DECLARE`) is NOT enqueued: Python's p_funcdeclforward
     * (zxbparser.py:2918-2930) returns None so no such node ever reaches
     * gl.FUNCTIONS — only the real definition's FUNCDECL does. Skipping it
     * here makes the C pending queue exactly Python's gl.FUNCTIONS set
     * (definition only), and prevents the shared function-entry node from
     * being queued twice (declare + def) — the unbounded re-entry the
     * first S5.7b attempt hit on warn_unreach0. */
    if (node->u.funcdecl.is_forward)
        return node;
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
    /* S5.7b — caller-side param/ABI. CALL/FUNCCALL appear as tag nodes
     * (parser.c make_call / the FUNCCALL→CALL statement rewrite,
     * parser.c:1530-1531) and the no-parens sub-call SENTENCE "CALL"
     * (parser.c:1592). All three share child[0]=callee ID,
     * child[1]=ARGLIST and the same lowering (tr_visit_call_common gates
     * on the callee class and is shape-agnostic). ARGLIST/ARGUMENT are
     * tag nodes. */
    visitor_on_tag(v, AST_CALL, tr_visit_call);
    visitor_on_tag(v, AST_FUNCCALL, tr_visit_funccall);
    visitor_on_tag(v, AST_ARGLIST, tr_visit_arglist);
    visitor_on_tag(v, AST_ARGUMENT, tr_visit_argument);
    visitor_on_sentence(v, "CALL", tr_visit_call);
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
    tr->emitted_funcs = NULL;
    tr->emitted_len = 0;
    tr->emitted_cap = 0;

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
 *   :171-172 bound_tables flush -> ic_vard(label, data) (S5.7d).
 */

/* SymbolBOUND geometry: a BOUND child is [lower, upper] (bound.py:33-37);
 * the S5.7d numeric corpus uses NUMBER / CONSTEXPR(NUMBER) literals.
 * Mirrors var_translator.c's vt_bound_val (kept local to avoid a
 * cross-TU dependency churn). */
static long tr_bound_val(AstNode *n) {
    if (!n) return 0;
    if (n->tag == AST_NUMBER) return (long)n->u.number.value;
    if (n->tag == AST_CONSTEXPR && n->child_count > 0)
        return tr_bound_val(n->children[0]);
    return 0;
}

/* Translator.traverse_const, NUMBER/CONST leaf (translator_visitor.py
 * :177-184) — the scalar-default CONSTEXPR case; corpus leaves are
 * NUMBER/CONST, same shape as var_translator.c's vt_traverse_const. */
static const char *tr_traverse_const(Translator *tr, AstNode *node) {
    if (!node) return "";
    if (node->tag == AST_CONSTEXPR && node->child_count > 0)
        return tr_traverse_const(tr, node->children[0]);
    if (node->tag == AST_NUMBER) {
        if (node->t == NULL) {
            double value = node->u.number.value;
            char b[64];
            if (value == (double)(int64_t)value)
                snprintf(b, sizeof(b), "%lld", (long long)(int64_t)value);
            else
                snprintf(b, sizeof(b), "%g", value);
            node->t = arena_strdup(&tr->cs->arena, b);
        }
        return node->t ? node->t : "";
    }
    if (node->tag == AST_ID) /* CONST entry */
        return node->t ? node->t : "";
    return node->t ? node->t : "";
}

/* ic_vard (translator_inst_visitor.py:244-245): emit("vard", name, data).
 * The :171-172 bound-table flush. data is a Python list -> list repr. */
static void tr_ic_vard(Translator *tr, const char *name,
                       char **data, int n) {
    const char *args[2] = { name, tr_py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "vard", 2, args);
}

static void tr_visit_function(Translator *tr, AstNode *fdecl) {
    AstNode *entry = fdecl->child_count > 0 ? fdecl->children[0] : NULL;
    AstNode *body  = fdecl->child_count > 2 ? fdecl->children[2] : NULL;
    if (!entry) return;

    /* S5.7b — emit each function body at most once (Python invariant).
     * Belt-and-braces with tr_visit_funcdecl's is_forward skip: even if
     * an entry were (re-)queued via any path, visit_FUNCTION runs once
     * per function-entry, so the prologue/`__leave` label can never be
     * emitted twice and no caller/queue re-entry is unbounded. */
    if (tr_func_already_emitted(tr, entry))
        return;

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

    /* :58-116 local-var/param/local-array init walk (function_translator
     * .py:58-116). Iterate the body scope's insertion-ordered entries
     * (Python node.local_symbol_table.values()). Skip const + parameter
     * (caller pushed params). Local array => ic_larrd(offset, idx-table,
     * size, init-image, bound_ptrs) with the :90-94 always-`00` high-byte
     * quirk reproduced VERBATIM. Scalar local with a non-zero default =>
     * ic_lvarx (CONSTEXPR) / ic_lvard (literal). bound_tables (the
     * LBOUND/UBOUND tables) are flushed after the body via ic_vard. */
    char *bound_tables[16][2]; /* {label, list-repr} */
    char *bt_data[16][64];
    int bt_n[16];
    int bound_tables_n = 0;
    for (int li = 0; li < entry->u.id.local_entries_count; li++) {
        AstNode *lv = entry->u.id.local_entries[li];
        if (!lv) continue;
        SymbolClass c = lv->u.id.class_;
        /* :65-66 skip const OR parameter */
        if (c == CLASS_const || lv->u.id.scope == SCOPE_parameter)
            continue;

        if (c == CLASS_array && lv->u.id.scope == SCOPE_local) {
            /* :68-100 local-array init. */
            const char *mg = lv->u.id.mangled ? lv->u.id.mangled : "";
            size_t mgl = strlen(mg);
            char *lbl_lb = arena_alloc(&tr->cs->arena, mgl + 12);
            snprintf(lbl_lb, mgl + 12, "%s.__LBOUND__", mg);
            char *lbl_ub = arena_alloc(&tr->cs->arena, mgl + 12);
            snprintf(lbl_ub, mgl + 12, "%s.__UBOUND__", mg);

            bool lbound_needed = !lv->u.id.is_zero_based &&
                (lv->u.id.is_dynamically_accessed || lv->u.id.lbound_used);
            char *bptrs[2];
            bptrs[0] = lbound_needed ? lbl_lb : (char *)"0";
            bptrs[1] = lv->u.id.ubound_used ? lbl_ub : (char *)"0";

            AstNode *bl = lv->u.id.arr_boundlist;
            int ndims = bl ? bl->child_count : 0;

            if (lbound_needed && bound_tables_n < 16) {
                int k = 0;
                for (int i = 0; i < ndims && k < 64; i++) {
                    AstNode *bd = bl->children[i];
                    long lo = tr_bound_val(bd && bd->child_count > 0
                                               ? bd->children[0] : NULL);
                    char b[8];
                    snprintf(b, sizeof(b), "%04X", (unsigned)(lo & 0xFFFF));
                    bt_data[bound_tables_n][k++] = arena_strdup(&tr->cs->arena, b);
                }
                bound_tables[bound_tables_n][0] = lbl_lb;
                bt_n[bound_tables_n] = k;
                bound_tables_n++;
            }
            if (lv->u.id.ubound_used && bound_tables_n < 16) {
                int k = 0;
                for (int i = 0; i < ndims && k < 64; i++) {
                    AstNode *bd = bl->children[i];
                    long hi = tr_bound_val(bd && bd->child_count > 1
                                               ? bd->children[1] : NULL);
                    char b[8];
                    snprintf(b, sizeof(b), "%04X", (unsigned)(hi & 0xFFFF));
                    bt_data[bound_tables_n][k++] = arena_strdup(&tr->cs->arena, b);
                }
                bound_tables[bound_tables_n][0] = lbl_ub;
                bt_n[bound_tables_n] = k;
                bound_tables_n++;
            }

            /* :90-94 idx table: l = [len(bounds)-1] + [b.count for
             * bounds[1:]]; then per x: q.append("%02X"%(x&0xFF));
             * q.append("%02X"%((x&0xFF)>>8))  — the high byte is a value
             * already masked to 8 bits then >>8, i.e. ALWAYS 0x00. This
             * is an upstream Boriel quirk; reproduce it, do NOT "fix". */
            char *q[130];
            int qn = 0;
            long lvec[64];
            int lvn = 0;
            lvec[lvn++] = (long)ndims - 1;
            for (int i = 1; i < ndims && lvn < 64; i++) {
                AstNode *bd = bl->children[i];
                long lo = tr_bound_val(bd && bd->child_count > 0
                                           ? bd->children[0] : NULL);
                long hi = tr_bound_val(bd && bd->child_count > 1
                                           ? bd->children[1] : NULL);
                lvec[lvn++] = (hi - lo + 1);     /* bound.count */
            }
            for (int i = 0; i < lvn && qn < 128; i++) {
                long x = lvec[i];
                char b[8];
                snprintf(b, sizeof(b), "%02X", (unsigned)(x & 0xFF));
                q[qn++] = arena_strdup(&tr->cs->arena, b);
                snprintf(b, sizeof(b), "%02X",
                         (unsigned)(((unsigned)(x & 0xFF)) >> 8)); /* ==00 */
                q[qn++] = arena_strdup(&tr->cs->arena, b);
            }
            { /* q.append("%02X" % type_.size) */
                char b[8];
                snprintf(b, sizeof(b), "%02X",
                         (unsigned)(type_size(lv->type_) & 0xFF));
                q[qn++] = arena_strdup(&tr->cs->arena, b);
            }

            /* r = array_default_value(type_, default_value) if any. */
            char *r[1024];
            int rn = 0;
            if (lv->u.id.arr_init != NULL)
                rn = tr_array_default_value(tr, lv->type_,
                                            lv->u.id.arr_init, r, 0, 1024);

            /* local_var.size = count * elem (arrayref.py:39-42). */
            long count = 1;
            for (int i = 0; i < ndims; i++) {
                AstNode *bd = bl->children[i];
                long lo = tr_bound_val(bd && bd->child_count > 0
                                           ? bd->children[0] : NULL);
                long hi = tr_bound_val(bd && bd->child_count > 1
                                           ? bd->children[1] : NULL);
                count *= (hi - lo + 1);
            }
            long asize = count * type_size(lv->type_);

            tr_ic_larrd(tr, lv->u.id.offset, q, qn, asize,
                        r, rn, bptrs, 2);
        } else {
            /* :102-115 scalar-local: only if token != FUNCTION and
             * default_value not None and != 0. The C ID carries the
             * initializer as default_value_expr (a make_typecast of a
             * static expr). CONSTEXPR => ic_lvarx, literal => ic_lvard. */
            AstNode *dv = lv->u.id.default_value_expr;
            if (c != CLASS_function && c != CLASS_sub && dv != NULL &&
                !(dv->tag == AST_NUMBER && dv->u.number.value == 0.0)) {
                if (dv->tag == AST_CONSTEXPR) {
                    char *one[1];
                    one[0] = (char *)tr_traverse_const(tr, dv);
                    tr_ic_lvarx(tr, lv->type_, lv->u.id.offset, one, 1);
                } else {
                    char *data[8];
                    int dn = tr_default_value(tr, lv->type_, dv, data, 8);
                    tr_ic_lvard(tr, lv->u.id.offset, data, dn);
                }
            }
        }
    }

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

    /* :171-172 bound_tables flush: for bt in bound_tables:
     * ic_vard(bt.label, bt.data). Emitted AFTER ic_leave, exactly as
     * Python's generator yields them last. */
    for (int i = 0; i < bound_tables_n; i++)
        tr_ic_vard(tr, bound_tables[i][0], bt_data[i], bt_n[i]);
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
    free(tr->emitted_funcs);
    tr->emitted_funcs = NULL;
    tr->emitted_len = tr->emitted_cap = 0;
}
