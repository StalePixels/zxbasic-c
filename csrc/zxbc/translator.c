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
#include "errmsg.h"  /* S5.8d — err_no_data_defined (visit_READ/RESTORE) */
#include "z80asm.h"  /* z80h_pyfloat_repr — Python str(float) round-trip */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* S5.7d — the param/local IX-relative IC wrappers are defined below their
 * first callers (tr_visit_var :220ish, tr_emit_var_assign :246ish);
 * forward-declare so those callers see prototypes. */
static void tr_ic_pload(Translator *tr, const TypeInfo *type_,
                        const char *t1, const char *offset);
static void tr_ic_pstore(Translator *tr, const TypeInfo *type_,
                         const char *offset, const char *t);
/* tr_bound_val (defined further below) — used by tr_visit_arraycopy. */
static long tr_bound_val(AstNode *n);
/* tr_ic_vard is defined below (its first historical caller was the S5.7d
 * bound-table flush); translator_emit_strings calls it earlier. */
static void tr_ic_vard(Translator *tr, const char *name,
                       char **data, int n);
/* S5.8d — DATA helpers defined alongside emit_data_blocks (just before
 * translator_emit_strings) but first called by tr_visit_read /
 * tr_visit_restore higher up; tr_traverse_const is defined far below
 * (its historical first caller was the scalar-default path). Forward-
 * declare all three, same discipline as the S5.7d block above. */
static void tr_ic_data(Translator *tr, const TypeInfo *type_,
                       char **data, int n);
static int tr_data_type_code(const TypeInfo *type_);
static const char *tr_traverse_const(Translator *tr, AstNode *node);
static const char *tr_constref_t(Translator *tr, AstNode *value);
static const char *tr_array_data_label(Translator *tr, AstNode *entry);

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
        /* Python NUMBER.t == str(self.value). For ints kept as Python int
         * (HEXA/OCTAL/BIN, or where the lexer round-trips X.0 -> X) this
         * is "X"; for floats it is the shortest-roundtrip decimal incl.
         * the explicit ".0" on integer-valued floats. We approximate via
         * the node's type_: int types -> "%lld", float/fixed -> shortest
         * roundtrip (with "X.0" for integer-valued — matches str(1.0)
         * == '1.0'). number-typed nodes from arithmetic constant-folding
         * carry float type and so take the shortest-roundtrip branch
         * (fixes fporder / read9 / readokup / readokdown PI bytes). */
        TypeInfo *ty = node->type_;
        TypeInfo *ft = (ty && ty->final_type) ? ty->final_type : ty;
        bool is_float_repr =
            ft && (ft->basic_type == TYPE_float ||
                   ft->basic_type == TYPE_fixed);
        if (!is_float_repr && value == (double)(int64_t)value)
            snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)value);
        else
            z80h_pyfloat_repr(value, buf, (int)sizeof(buf));
        Translator *tr = v->ctx;
        node->t = arena_strdup(&tr->cs->arena, buf);
    }
    return node;
}

/* visit_STRING (translator.py:60-63):
 *   node.t = "#" + self.add_string_label(node.value); yield node.t
 * add_string_label folds by EXACT bytes via the Backend STRING_LABELS
 * store (string_labels.add_string_label). This is THE ONLY
 * backend_add_string_label call site (mirrors Python: the only
 * add_string_label callers are visit_STRING and emit_data_blocks; the
 * latter is S5.8d-deferred). NUL-safe via node.u.string.length. */
static AstNode *tr_visit_string(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const char *val = node->u.string.value ? node->u.string.value : "";
    int len = node->u.string.length;
    char *lbl = backend_add_string_label(tr->backend, val, len);
    size_t ll = strlen(lbl);
    char *t = arena_alloc(&tr->cs->arena, ll + 2);
    t[0] = '#';
    memcpy(t + 1, lbl, ll + 1);
    node->t = t;
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

/* ConstRef.t (src/symbols/id_/ref/constref.py:30-32): a CONST id's `.t`
 * is its stored value's `.t` — NOT the mangled label (that is VarRef.t,
 * varref.py:35-37, which the C parser wrongly stamps onto consts too at
 * the shared DIM/CONST `.t = mangled` line). Faithfully resolve the
 * value node's `.t` exactly as the Python Symbol `.t` property would:
 *   - NUMBER   -> str(value)                    (number.py:71-72)
 *   - CONSTEXPR -> "#" + traverse_const(expr)   (constexpr.py:43-45,
 *                  translator_visitor.py:240-241)
 *   - nested CONST id -> that const's ConstRef.t (constref.py:31-32,
 *                  recursive through its own stored value)
 *   - STRING   -> the string label (string_.py .t; the C parser gates
 *                  string consts out of default_value_expr so this is
 *                  defensive — kept for 1:1 fidelity with ConstRef.t).
 * Returns "" when the value is absent (matches the C "" fall-throughs
 * the consuming ops already tolerate). */
static const char *tr_constref_t(Translator *tr, AstNode *value) {
    if (!value)
        return "";
    if (value->tag == AST_NUMBER)
        return tr_traverse_const(tr, value); /* NUMBER.t == str(value) */
    if (value->tag == AST_CONSTEXPR) {
        /* SymbolCONSTEXPR.t == "#" + traverse_const(self); and
         * traverse_const(CONSTEXPR) recurses into .expr. */
        AstNode *inner = value->child_count > 0 ? value->children[0] : NULL;
        const char *r = tr_traverse_const(tr, inner ? inner : value);
        size_t rl = strlen(r);
        char *s = arena_alloc(&tr->cs->arena, rl + 2);
        s[0] = '#';
        memcpy(s + 1, r, rl + 1);
        return s;
    }
    if (value->tag == AST_ID && value->u.id.class_ == CLASS_const)
        return tr_constref_t(tr, value->u.id.default_value_expr);
    if (value->tag == AST_STRING) {
        const char *sv = value->u.string.value ? value->u.string.value : "";
        char *lbl = backend_add_string_label(tr->backend, sv,
                                              value->u.string.length);
        size_t ll = strlen(lbl);
        char *s = arena_alloc(&tr->cs->arena, ll + 2);
        s[0] = '#';
        memcpy(s + 1, lbl, ll + 1);
        return s;
    }
    /* Any other stored value: its already-resolved .t (mirrors the bare
     * Symbol.t fall-through). */
    return value->t ? value->t : "";
}

/* visit_VAR (translator.py:112-128). Global-var rvalue fast path: when
 * node.t == node.mangled and scope == global_, Python returns immediately
 * (the value is read directly from memory by the consuming op via its
 * mangled label — no load IC). parameter/local pload paths are S5.5+
 * (out of the S5.3 integer-scalar-global scope); reaching them here is a
 * real gap, so fail loud rather than emit nothing silently. */
static AstNode *tr_visit_var(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    /* Python dispatches by node.token: a CLASS.const id has token
     * "CONST" (constref.py:22-24), so it routes to visit_CONST
     * (translator.py:109-110) — `yield node.symbol` — NOT visit_VAR.
     * The C visitor keys on the AST_ID tag, so reproduce the token
     * split here. visit_CONST visits the stored value (so a STRING/
     * CONSTEXPR value emits its label), and the node's resolved `.t`
     * is ConstRef.t == the value's `.t` (constref.py:30-32) — never
     * the mangled label. */
    if (node->u.id.class_ == CLASS_const) {
        AstNode *value = node->u.id.default_value_expr;
        if (value)
            visitor_visit(v, value);          /* visit_CONST: yield symbol */
        node->t = (char *)tr_constref_t(tr, value); /* ConstRef.t */
        return node;
    }
    /* Python dispatches by node.token: a CLASS.array id has token
     * "VARARRAY" (arrayref.py:31-33) and routes to
     * Translator.visit_VARARRAY (translator.py:133-134) which is a bare
     * `pass` — NO IC, NO child descent. The C visitor keys on AST_ID, so
     * reproduce the split: a VARARRAY reached as a node (e.g. generic
     * descent of ARRAYDECL, or a whole-array ARGUMENT value) must emit
     * nothing and not recurse — emitting the scalar ic_pload below for a
     * local/parameter array slot is the spurious `ld a,(ix-N)/push af`. */
    if (node->u.id.class_ == CLASS_array)
        return node;                          /* visit_VARARRAY: pass */
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
        /* Python VarRef.t (varref.py:34-42): for a NON-global, NON-dynamic
         * scalar it is `self._t` — a per-symbol cached optemp, NOT the
         * mangled label. The C parser (parser.c:3827-3828) stamps
         * `.t = mangled` for EVERY id including locals/params; that global
         * mangle (e.g. `_i`) is wrong for a local — a later
         * ic_param(value.type_, value.t) would emit `ld hl,(_i)` for what
         * is really an IX slot. Mirror Python's `_t`: when .t is still the
         * parser's mangle stamp on a non-global scalar, mint a temp ONCE
         * and cache it on the shared symbol node (so every reference reads
         * the same _t, exactly like Python's cached property). */
        if (node->t && mangled && strcmp(node->t, mangled) == 0)
            node->t = (char *)compiler_new_temp(tr->cs);
        const char *dst = (node->t && node->t[0]) ? node->t
                        : compiler_new_temp(tr->cs);
        /* VarRef.t (varref.py:38-42): for a NON-global *dynamic* (string)
         * scalar/param the property returns f"${self._t}" — the leading
         * '$' is load-bearing (it makes _ploadstr/_fploadstr skip the
         * LOADSTR runtime_call, _pload.py:166-193, and is the heap-dup
         * sentinel visit_ARGUMENT:216 keys on). Cache the '$'-prefixed
         * form ONCE on the shared symbol node so every reference reads the
         * identical Python `.t` value. */
        if (node->type_ && type_is_dynamic(node->type_) && dst[0] != '$') {
            char *ds = arena_alloc(&tr->cs->arena, strlen(dst) + 2);
            ds[0] = '$';
            strcpy(ds + 1, dst);
            dst = ds;
        }
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
/* ic_exchg (translator_inst_visitor.py:112-113): emit("exchg"). */
static void tr_ic_exchg(Translator *tr) {
    tr_emit_quad(tr, "exchg", 0, NULL);
}
/* ic_fpload (translator_inst_visitor.py:118-119):
 * emit(f"fpload{TSUFFIX}", t, offset). */
static void tr_ic_fpload(Translator *tr, const TypeInfo *type_,
                         const char *t, const char *offset) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "fpload%s", suf);
    const char *args[2] = { t, offset };
    tr_emit_quad(tr, base, 2, args);
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
/* ic_lenstr (translator_inst_visitor.py:157-158): emit("lenstr",t1,t2). */
static void tr_ic_lenstr(Translator *tr, const char *t1, const char *t2) {
    const char *args[2] = { t1, t2 };
    tr_emit_quad(tr, "lenstr", 2, args);
}
/* ic_in (translator_inst_visitor.py:127-128): emit("in", t). */
static void tr_ic_in(Translator *tr, const char *t) {
    const char *args[1] = { t };
    tr_emit_quad(tr, "in", 1, args);
}
/* ic_out (translator_inst_visitor.py:196-197): emit("out", t1, t2). */
static void tr_ic_out(Translator *tr, const char *t1, const char *t2) {
    const char *args[2] = { t1, t2 };
    tr_emit_quad(tr, "out", 2, args);
}
/* ic_neg/ic_bnot/ic_abs (translator_inst_visitor.py:184-188,79-80,61-62):
 * emit(f"<op>{TSUFFIX}", t1, t2). ic_not uses _no_bool
 * (translator_inst_visitor.py:190-191). */
static void tr_ic_unop(Translator *tr, const char *base,
                       const TypeInfo *type_, const char *t1,
                       const char *t2, bool no_bool) {
    const char *suf = no_bool ? tr_no_bool(type_) : tr_tsuffix(type_);
    char ins[16];
    snprintf(ins, sizeof(ins), "%s%s", base, suf);
    const char *args[2] = { t1, t2 };
    tr_emit_quad(tr, ins, 2, args);
}
/* ic_aaddr (translator_inst_visitor.py:58-59): emit("aaddr", t1, t2). */
static void tr_ic_aaddr(Translator *tr, const char *t1, const char *t2) {
    const char *args[2] = { t1, t2 };
    tr_emit_quad(tr, "aaddr", 2, args);
}
/* ic_paaddr (translator_inst_visitor.py:199-200): emit("paaddr",t1,t2). */
static void tr_ic_paaddr(Translator *tr, const char *t1, const char *t2) {
    const char *args[2] = { t1, t2 };
    tr_emit_quad(tr, "paaddr", 2, args);
}
/* ic_aload (translator_inst_visitor.py:67-68): emit(f"aload{TSUFFIX}",
 * t1, mangle). Global array element read — mangle == entry.mangled. */
static void tr_ic_aload(Translator *tr, const TypeInfo *type_,
                        const char *t1, const char *mangle) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "aload%s", suf);
    const char *args[2] = { t1, mangle };
    tr_emit_quad(tr, base, 2, args);
}
/* ic_astore (translator_inst_visitor.py:73-74): emit(f"astore{TSUFFIX}",
 * addr, t). Global array element store. */
static void tr_ic_astore(Translator *tr, const TypeInfo *type_,
                         const char *addr, const char *t) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "astore%s", suf);
    const char *args[2] = { addr, t };
    tr_emit_quad(tr, base, 2, args);
}
/* ic_paload (translator_inst_visitor.py:205-206): emit(f"paload{TSUFFIX}",
 * t, offset). Parameter/local array element read (IX-relative). */
static void tr_ic_paload(Translator *tr, const TypeInfo *type_,
                         const char *t, const char *offset) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "paload%s", suf);
    const char *args[2] = { t, offset };
    tr_emit_quad(tr, base, 2, args);
}
/* ic_pastore (translator_inst_visitor.py:211-212):
 * emit(f"pastore{TSUFFIX}", offset, t). Parameter/local array store. */
static void tr_ic_pastore(Translator *tr, const TypeInfo *type_,
                          const char *offset, const char *t) {
    const char *suf = tr_tsuffix(type_);
    char base[16];
    snprintf(base, sizeof(base), "pastore%s", suf);
    const char *args[2] = { offset, t };
    tr_emit_quad(tr, base, 2, args);
}
/* ic_memcopy (translator_inst_visitor.py:172-173):
 * emit("memcopy", t1, t2, t3). */
static void tr_ic_memcopy(Translator *tr, const char *t1, const char *t2,
                          const char *t3) {
    const char *args[3] = { t1, t2, t3 };
    tr_emit_quad(tr, "memcopy", 3, args);
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

/* S7.1b-i — backend.c (non-static): registers label->asm module into
 * b->requires_, the faithful translator-time analogue of Python
 * common.runtime_call's backend.REQUIRES.add(LABEL_REQUIRED_MODULES
 * [label]) (translator_visitor.py:122-123). Forward-declared here because
 * backend.h is owned elsewhere; defined in backend.c. */
void backend_register_runtime_module(Backend *b, const char *label);

/* runtime_call (translator_visitor.py:119-123): ic_call(label,num) then
 * REQUIRES.add(LABEL_REQUIRED_MODULES[label]) AT TRANSLATOR TIME. The
 * backend's emit_call does NOT register REQUIRES for translator-emitted
 * call Quads, so the registration must happen here, exactly as Python's
 * common.runtime_call does it (right after ic_call, before the next IC). */
static void tr_runtime_call(Translator *tr, const char *label, int num) {
    tr_ic_call(tr, label, num);
    backend_register_runtime_module(tr->backend, label);
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
        /* Python visit_RETURN (translator.py:701): addr =
         * "%s__leave" % node.children[0].mangled — the *function*
         * mangled ("_<name>", _id.py:60), NOT a .LABEL._ label
         * mangle. The S5.7e parser now supplies the resolved
         * FUNCTION/SUB symbol-table entry as child[0], so read its
         * .mangled directly (instrumented Python ground truth:
         * 37.bas -> '_test__leave'; subrec.bas -> '_fact__leave'). */
        const char *fm = (func && func->u.id.mangled) ? func->u.id.mangled : "";
        size_t fl = strlen(fm);
        char *addr = arena_alloc(&tr->cs->arena, fl + 8);
        memcpy(addr, fm, fl);
        memcpy(addr + fl, "__leave", 8);
        tr_ic_ret(tr, val ? val->type_ : NULL,
                  (val && val->t) ? val->t : "", addr);
    } else if (node->child_count == 1 &&
               node->children[0] && node->children[0]->tag == AST_ID &&
               node->children[0]->u.id.class_ != CLASS_label) {
        /* len==1: a bare function-ref child -> ic_return(func__leave).
         * Python visit_RETURN (translator.py:704): addr =
         * "%s__leave" % node.children[0].mangled (the func mangled). */
        AstNode *func = node->children[0];
        const char *fm = func->u.id.mangled ? func->u.id.mangled : "";
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
        /* :215-226. node.t == ARGUMENT.t (argument.py:27-36): byval &
         * dynamic & a non-global VAR value => value.t with the leading
         * '$' stripped (value.t[1:]); otherwise value.t. node.type_ ==
         * value.type_.
         *
         * :216 the dynamic-`$`-string VAR heap-dup arm fires when the
         * value is a VAR (CLASS.var id) of dynamic (string) type whose
         * VarRef.t starts with '$' — i.e. a non-global string param/local
         * (varref.py:38-42). It does NOT yield the value; it ic_ploads
         * the *pointer* off the IX slot (param => +offset, local =>
         * -offset) into the $-stripped temp, then ic_params it. The
         * stripped (no-'$') temp makes _ploadstr emit the LOADSTR heap
         * duplicate (_pload.py:166-178) — the byval string copy. */
        bool var_is_dyn_dollar =
            value && value->tag == AST_ID &&
            value->u.id.class_ == CLASS_var &&
            value->type_ && type_is_dynamic(value->type_) &&
            value->u.id.scope != SCOPE_global;
        if (var_is_dyn_dollar) {
            /* node.t == value.t[1:]; value.t == VarRef.t == "$"+_t
             * (varref.py:38-42). The bare temp's exact number never
             * reaches output (ploadstr uses ins[2]=offset; the absent
             * '$' is the only load-bearing bit). */
            const char *base = value->t;
            if (!(base && base[0])) base = compiler_new_temp(tr->cs);
            const char *bare = (base[0] == '$') ? base + 1 : base;
            /* :219-222 scope offset: parameter => +offset, local =>
             * -offset (str()-coerced; no '*' — byval pointer copy). */
            long off = value->u.id.offset;
            if (value->u.id.scope == SCOPE_local) off = -off;
            char ofs[24];
            snprintf(ofs, sizeof(ofs), "%ld", off);
            tr_ic_pload(tr, value->type_, bare, ofs);
            tr_ic_param(tr, value->type_, bare);
            return node;
        }
        /* :224 yield node.value */
        if (value) visitor_visit(v, value);
        /* :225 ic_param(node.type_, node.t). node.t == ARGUMENT.t:
         * byval-dynamic-VAR strips a leading '$' from value.t; every
         * other shape passes value.t through unchanged. */
        const char *vt = (value && value->t) ? value->t : "";
        if (value && value->tag == AST_ID &&
            value->u.id.class_ == CLASS_var &&
            value->type_ && type_is_dynamic(value->type_) &&
            value->u.id.scope != SCOPE_global &&
            vt[0] == '$')
            vt = vt + 1;
        tr_ic_param(tr, value ? value->type_ : NULL, vt);
        return node;
    }

    /* ByRef. :229-248 non-array byref: address of the argument.
     * Python (:231-234): t = node.t if node.t[0] != "_" else optemps.new_t().
     * For a global-VAR byref ARGUMENT, node.t == value.t == mangled
     * ("_name") which starts with "_" → Python ALWAYS mints a fresh temp.
     * If t is left as the mangled name, the load IC's destination is the
     * mangled global label itself, so ic_load(PTR,"_v","#_v") lowers as
     * ld hl,_v / ld (_v),hl and the following ic_param("_v") then emits
     * ld hl,(_v); push hl — i.e. the extra value-deref byrefbyref.bas
     * exhibits. Mint a fresh temp like Python. (Also fixes the
     * parameter/local arm below which already used the correct check.) */
    if (value && value->tag == AST_ID && value->u.id.scope == SCOPE_global) {
        const char *mg = value->u.id.mangled ? value->u.id.mangled : "";
        size_t ml = strlen(mg);
        char *hm = arena_alloc(&tr->cs->arena, ml + 2);
        hm[0] = '#';
        memcpy(hm + 1, mg, ml + 1);
        const char *nt = node->t;
        const char *t = (nt && nt[0] && nt[0] != '_')
                       ? nt : compiler_new_temp(tr->cs);
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

    /* ByRef ARRAYLOAD/ARRAYACCESS argument — "Must compute Address of
     * @array(...)" (translator.py:250-263).
     *
     *   if value.scope == global_ and O_LEVEL > 1:
     *       value = BINARY PLUS(UNARY ADDRESS(entry), NUMBER(offset))
     *   else:
     *       value = UNARY ADDRESS(ARRAYACCESS.copy_from(value))
     *   yield value
     *
     * Both reduce to: push the index args, then take the element address
     * (ic_aaddr global / ic_paaddr param|local — visit_ADDRESS,
     * unary_op_translator.py:29-37). The O>1-global BINARY-PLUS form
     * folds, through the same peephole, to the identical static
     * `<data_label> (+N)` the else-form's aaddr collapses to for a const
     * index — so the single aaddr/paaddr lowering is faithful to both.
     * NB: visit_ADDRESS does NOT ic_param afterwards; aaddr/paaddr's
     * `push hl` IS the ByRef pointer arg. */
    if (value && value->tag == AST_ARRAYACCESS) {
        AstNode *aent = value->child_count > 0 ? value->children[0] : NULL;
        Scope ascope = aent ? aent->u.id.scope : SCOPE_global;

        /* :251-258  O>1 + global form. Python REPLACES node.value with
         *   BINARY PLUS( UNARY ADDRESS(entry, PTR), NUMBER(offset, PTR) )
         * and yields THAT. ADDRESS(global-array entry) is the *scalar*
         * branch (operand.token != "ARRAYACCESS") → ic_load(PTR, t,
         * "#"+entry.t); entry.t == ArrayRef.t(global) == data_label
         * (arrayref.py:69-78). NUMBER(offset).t == str(offset) (no IC).
         * BINARY PLUS → ic_add(PTR, t2, t1, str(offset)). The
         * const-index byte offset is ARRAYACCESS.offset (is_const). The
         * add of a static address + constant peephole-folds to
         * `ld hl, <data_label> / inc hl…` — byte-identical to Python's
         * O2 output. Verified Python IC: loadu16 t #_a.__DATA__ ;
         * addu16 t2 t 1. Non-const offset is outside the repro scope and
         * keeps the aaddr path below (loud-safe). */
        if (ascope == SCOPE_global &&
            tr->cs->opts.optimization_level > 1 &&
            value->u.arrayaccess.is_const) {
            const TypeInfo *uintt =
                tr->cs->symbol_table->basic_types[TYPE_uinteger];
            const char *dlabel = tr_array_data_label(tr, aent);
            size_t dl = strlen(dlabel);
            char *hsrc = arena_alloc(&tr->cs->arena, dl + 2);
            hsrc[0] = '#';
            memcpy(hsrc + 1, dlabel, dl + 1);
            const char *t1 = compiler_new_temp(tr->cs);
            tr_ic_load(tr, uintt, t1, hsrc);          /* ADDRESS(entry) */
            const char *t2 = (node->t && node->t[0]) ? node->t
                           : compiler_new_temp(tr->cs);
            char obs[24];
            snprintf(obs, sizeof(obs), "%ld",
                     value->u.arrayaccess.offset);
            tr_ic_arith(tr, "add", uintt, t2, t1, obs); /* + NUMBER */
            return node;
        }

        /* :259-263 else form — UNARY ADDRESS(ARRAYACCESS.copy_from). */
        /* visit_ADDRESS `yield node.operand` == visit_ARRAYACCESS:
         * push the index ARGUMENT children in reverse (NO aload). */
        for (int i = value->child_count - 1; i >= 1; i--)
            visitor_visit(v, value->children[i]);
        const char *at = (node->t && node->t[0]) ? node->t
                       : compiler_new_temp(tr->cs);
        if (ascope == SCOPE_global) {
            tr_ic_aaddr(tr, at, aent ? (aent->u.id.mangled
                                        ? aent->u.id.mangled : "") : "");
        } else if (ascope == SCOPE_parameter) {
            char ob[24];
            snprintf(ob, sizeof(ob), "*%ld",
                     aent ? (long)aent->u.id.offset : 0);
            tr_ic_paaddr(tr, at, ob);
        } else { /* local */
            char ob[24];
            snprintf(ob, sizeof(ob), "%ld",
                     aent ? -(long)aent->u.id.offset : 0);
            tr_ic_paaddr(tr, at, ob);
        }
        return node;
    }

    /* Other byref array shapes (whole-array etc.) — loud residue. */
    fprintf(stderr,
        "zxbc: visit_ARGUMENT byref array — non-ARRAYACCESS shape "
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
        /* :207-208 discard a returned string. Resolve through .final to
         * match Python's self.TYPE(TYPE.string) equality — a callee's
         * declared type_ can be a TYPEREF that wraps the underlying
         * BASICTYPE (translator.py FunctionRef.type_ -> TypeRef.final). */
        const TypeInfo *ft = callee->type_
            ? (callee->type_->final_type ? callee->type_->final_type
                                          : callee->type_)
            : NULL;
        if (callee->u.id.class_ == CLASS_function && ft &&
            ft->basic_type == TYPE_string) {
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
        /* Python relies on Symbol.t being a lazy property; the C port
         * computes node->t at visit time, so visit the child first to
         * materialise its t-text.  Number children just stringify their
         * value (tr_visit_number). */
        if (child) visitor_visit(v, child);
        const TypeInfo *ptr = tr->cs->symbol_table->basic_types[TYPE_uinteger];
        tr_ic_fparam(tr, ptr, (child && child->t) ? child->t : "0");
        tr_runtime_call(tr, ".core.CHECK_BREAK", 0);
    }
    return node;
}

/* visit_NOP (translator.py:50-51): pass — nothing to do. */
static AstNode *tr_visit_nop(Visitor *v, AstNode *node) {
    (void)v;
    return node;
}

/* visit_CLS (translator.py:53-54): runtime_call(CLS, 0). No children. */
static AstNode *tr_visit_cls(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_runtime_call(tr, ".core.CLS", 0);
    return node;
}

/* visit_BORDER (translator.py:886-889): yield child[0];
 * ic_fparam(TYPE.ubyte, child[0].t); runtime_call(BORDER, 0).
 * NB ic_fparam uses a hardcoded ubyte — the parser already
 * make_typecast'd the operand to ubyte (zxbparser.py:946). */
static AstNode *tr_visit_border(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *c = node->child_count > 0 ? node->children[0] : NULL;
    if (c) visitor_visit(v, c);
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    tr_ic_fparam(tr, ubyte, (c && c->t) ? c->t : "0");
    tr_runtime_call(tr, ".core.BORDER", 0);
    return node;
}

/* visit_PAUSE (translator.py:904-907): yield child[0];
 * ic_fparam(child[0].type_, child[0].t); runtime_call(PAUSE, 0).
 * The parser make_typecast's the operand to uinteger
 * (zxbparser.py:2159) so child[0].type_ is uinteger. */
static AstNode *tr_visit_pause(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *c = node->child_count > 0 ? node->children[0] : NULL;
    if (c) visitor_visit(v, c);
    tr_ic_fparam(tr, c ? c->type_ : NULL, (c && c->t) ? c->t : "0");
    tr_runtime_call(tr, ".core.__PAUSE", 0);
    return node;
}

/* visit_RANDOMIZE (translator.py:101-104): yield child[0];
 * ic_fparam(child[0].type_, child[0].t); runtime_call(RANDOMIZE, 0).
 * The parser supplies NUMBER(0,ulong) when no arg, else
 * make_typecast(ulong, expr) (zxbparser.py:1047-1054). */
static AstNode *tr_visit_randomize(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *c = node->child_count > 0 ? node->children[0] : NULL;
    if (c) visitor_visit(v, c);
    tr_ic_fparam(tr, c ? c->type_ : NULL, (c && c->t) ? c->t : "0");
    tr_runtime_call(tr, ".core.RANDOMIZE", 0);
    return node;
}

/* visit_ATTR_sentence (translator.py:921-931) — the ONE handler for the
 * 8 standalone attr statements (INK/PAPER/FLASH/BRIGHT/INVERSE/OVER/BOLD/
 * ITALIC, kind == node.token): yield child[0]; ic_fparam(TYPE.ubyte,
 * child[0].t); runtime_call({INK:.core.INK,...}[token],0). HAS_ATTR is
 * write-only in Python (never read — no output effect), so omitted. */
static AstNode *tr_visit_attr_sentence(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *c = node->child_count > 0 ? node->children[0] : NULL;
    if (c) visitor_visit(v, c);
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    tr_ic_fparam(tr, ubyte, (c && c->t) ? c->t : "0");
    const char *k = node->u.sentence.kind;
    const char *label =
        strcmp(k, "INK")     == 0 ? ".core.INK" :
        strcmp(k, "PAPER")   == 0 ? ".core.PAPER" :
        strcmp(k, "FLASH")   == 0 ? ".core.FLASH" :
        strcmp(k, "BRIGHT")  == 0 ? ".core.BRIGHT" :
        strcmp(k, "INVERSE") == 0 ? ".core.INVERSE" :
        strcmp(k, "OVER")    == 0 ? ".core.OVER" :
        strcmp(k, "BOLD")    == 0 ? ".core.BOLD" :
                                    ".core.ITALIC";  /* ITALIC */
    tr_runtime_call(tr, label, 0);
    return node;
}

/* src.arch.zx48k.beep.getDEHL (beep.py:41-63) — duration,pitch ->
 * (DE,HL) unsigned 16-bit. Faithful: Python int() truncates toward
 * zero (== C (long) cast); 2.0**B == pow. The corpus const path keeps
 * intPitch/duration in range so the BeepError raises are unreachable
 * (a const out-of-range is a parse-time error before translation). */
static void tr_beep_getDEHL(double duration, double pitch,
                            long *DE, long *HL) {
    static const double TABLE[12] = {
        261.625565290, 277.182631135, 293.664768100, 311.126983881,
        329.627557039, 349.228231549, 369.994422674, 391.995436072,
        415.304697513, 440.000000000, 466.163761616, 493.883301378,
    };
    long intPitch = (long)pitch;
    double fractPitch = pitch - (double)intPitch;
    double tmp = 1.0 + 0.0577622606 * fractPitch;
    long A = intPitch + 60;
    long B = -5 + (long)((double)A / 12.0);
    A %= 0xC;
    double frec = TABLE[A];
    double tmp2 = tmp * frec;
    double f = tmp2 * pow(2.0, (double)B);
    *DE = (long)(0.5 + f * duration - 1.0);
    *HL = (long)(0.5 + 437500.0 / f - 30.125);
}

/* visit_BEEP (translator.py:891-902). Both-const -> getDEHL fold +
 * ic_param(uint,HL)/ic_fparam(uint,DE)/runtime_call(BEEPER). Else ->
 * yield child[1]; ic_param(float,c1.t); yield child[0];
 * ic_fparam(float,c0.t); runtime_call(BEEP). Parser already
 * make_typecast'd both operands to float (zxbparser.py:1057-1063). */
static AstNode *tr_visit_beep(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    if (c0 && c1 && c0->tag == AST_NUMBER && c1->tag == AST_NUMBER) {
        long DE = 0, HL = 0;
        tr_beep_getDEHL(c0->u.number.value, c1->u.number.value, &DE, &HL);
        const TypeInfo *uint_t =
            tr->cs->symbol_table->basic_types[TYPE_uinteger];
        char hb[24], db[24];
        snprintf(hb, sizeof(hb), "%ld", HL);
        snprintf(db, sizeof(db), "%ld", DE);
        tr_ic_param(tr, uint_t, hb);
        tr_ic_fparam(tr, uint_t, db);
        tr_runtime_call(tr, ".core.__BEEPER", 0);
        return node;
    }
    const TypeInfo *float_t = tr->cs->symbol_table->basic_types[TYPE_float];
    if (c1) visitor_visit(v, c1);
    tr_ic_param(tr, float_t, (c1 && c1->t) ? c1->t : "0");
    if (c0) visitor_visit(v, c0);
    tr_ic_fparam(tr, float_t, (c0 && c0->t) ? c0->t : "0");
    tr_runtime_call(tr, ".core.BEEP", 0);
    return node;
}

/* ====================================================================
 * Drawing primitives — faithful ports of
 * src/arch/z80/visitor/translator.py:
 *   visit_PLOT  :754-762   visit_DRAW  :765-773
 *   visit_DRAW3 :776-787   visit_CIRCLE :789-800
 *
 * Common pattern (N = #positional children: PLOT/DRAW=2, DRAW3/CIRCLE=3):
 *   self.norm_attr()                       # runtime_call(COPY_ATTR,0)
 *   TMP_HAS_ATTR = self.check_attr(node,N) # children[N] iff len>N else None
 *   yield TMP_HAS_ATTR                     # visit attr_list block (NULL-safe)
 *   yield children[0]; ic_param (c0.type_, c0.t)
 *   ...
 *   yield children[N-1]; ic_fparam(cLast.type_, cLast.t)
 *   self.runtime_call(PLOT|DRAW|DRAW3|CIRCLE, 0)
 *   self.HAS_ATTR = TMP_HAS_ATTR is not None   # write-only in Python;
 *                                              # no output effect -> omit
 * norm_attr  == translator_visitor.py:172 (runtime_call COPY_ATTR,0).
 * check_attr == translator_visitor.py:251 (children[n] iff child_count>n).
 * ic_param / ic_fparam use the CHILD's OWN type_ — the parser already
 * make_typecast'd each positional (PLOT->ubyte, DRAW->integer/float,
 * CIRCLE->byte_). The attr_list child (when present) is the LAST child;
 * visiting it routes BLOCK->each "<NAME>_TMP" or the lone "<NAME>_TMP"
 * SENTENCE to tr_visit_attr_tmp, emitting the same attr IC as Python. */
static AstNode *tr_visit_plot(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_runtime_call(tr, ".core.COPY_ATTR", 0);          /* norm_attr() */
    AstNode *attr = node->child_count > 2 ? node->children[2] : NULL;
    visitor_visit(v, attr);                             /* yield TMP_HAS_ATTR */
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    if (c0) visitor_visit(v, c0);
    tr_ic_param(tr, c0 ? c0->type_ : NULL, (c0 && c0->t) ? c0->t : "0");
    if (c1) visitor_visit(v, c1);
    tr_ic_fparam(tr, c1 ? c1->type_ : NULL, (c1 && c1->t) ? c1->t : "0");
    tr_runtime_call(tr, ".core.PLOT", 0);
    return node;
}

static AstNode *tr_visit_draw(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_runtime_call(tr, ".core.COPY_ATTR", 0);          /* norm_attr() */
    AstNode *attr = node->child_count > 2 ? node->children[2] : NULL;
    visitor_visit(v, attr);                             /* yield TMP_HAS_ATTR */
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    if (c0) visitor_visit(v, c0);
    tr_ic_param(tr, c0 ? c0->type_ : NULL, (c0 && c0->t) ? c0->t : "0");
    if (c1) visitor_visit(v, c1);
    tr_ic_fparam(tr, c1 ? c1->type_ : NULL, (c1 && c1->t) ? c1->t : "0");
    tr_runtime_call(tr, ".core.DRAW", 0);
    return node;
}

static AstNode *tr_visit_draw3(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_runtime_call(tr, ".core.COPY_ATTR", 0);          /* norm_attr() */
    AstNode *attr = node->child_count > 3 ? node->children[3] : NULL;
    visitor_visit(v, attr);                             /* yield TMP_HAS_ATTR */
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    AstNode *c2 = node->child_count > 2 ? node->children[2] : NULL;
    if (c0) visitor_visit(v, c0);
    tr_ic_param(tr, c0 ? c0->type_ : NULL, (c0 && c0->t) ? c0->t : "0");
    if (c1) visitor_visit(v, c1);
    tr_ic_param(tr, c1 ? c1->type_ : NULL, (c1 && c1->t) ? c1->t : "0");
    if (c2) visitor_visit(v, c2);
    tr_ic_fparam(tr, c2 ? c2->type_ : NULL, (c2 && c2->t) ? c2->t : "0");
    tr_runtime_call(tr, ".core.DRAW3", 0);
    return node;
}

static AstNode *tr_visit_circle(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_runtime_call(tr, ".core.COPY_ATTR", 0);          /* norm_attr() */
    AstNode *attr = node->child_count > 3 ? node->children[3] : NULL;
    visitor_visit(v, attr);                             /* yield TMP_HAS_ATTR */
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    AstNode *c2 = node->child_count > 2 ? node->children[2] : NULL;
    if (c0) visitor_visit(v, c0);
    tr_ic_param(tr, c0 ? c0->type_ : NULL, (c0 && c0->t) ? c0->t : "0");
    if (c1) visitor_visit(v, c1);
    tr_ic_param(tr, c1 ? c1->type_ : NULL, (c1 && c1->t) ? c1->t : "0");
    if (c2) visitor_visit(v, c2);
    tr_ic_fparam(tr, c2 ? c2->type_ : NULL, (c2 && c2->t) ? c2->t : "0");
    tr_runtime_call(tr, ".core.CIRCLE", 0);
    return node;
}

/* visit_ASM (translator.py:963-967):
 *   ic_inline(f'#line {node.lineno} "{node.filename}"')
 *   ic_inline(node.asm)
 *   ic_inline(f'#line {node.lineno + len(node.asm.split("\n"))} "..."')
 * node.filename == gl.FILENAME (== cs->current_file). The middle line
 * count is len(split("\n")) == (#newlines)+1. */
static AstNode *tr_visit_asm(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    /* Use node->filename (snapshot at parse time of gl.FILENAME) so the
     * emitted `#line N "..."` reflects the file that contained the ASM
     * block — including #include'd files
     * (tap_include_asm_error → extra_chars.bas). Fall back to
     * cs->current_file if the node has no filename for some reason. */
    const char *fn = node->u.asm_block.filename
                       ? node->u.asm_block.filename
                       : (tr->cs->current_file ? tr->cs->current_file : "");
    const char *code = node->u.asm_block.code ? node->u.asm_block.code : "";
    int nlines = 1;
    for (const char *s = code; *s; s++) if (*s == '\n') nlines++;
    size_t fnl = strlen(fn);
    char *buf = arena_alloc(&tr->cs->arena, fnl + 48);
    snprintf(buf, fnl + 48, "#line %d \"%s\"", node->lineno, fn);
    translator_ic_inline(tr, buf);
    translator_ic_inline(tr, code);
    char *buf2 = arena_alloc(&tr->cs->arena, fnl + 48);
    snprintf(buf2, fnl + 48, "#line %d \"%s\"", node->lineno + nlines, fn);
    translator_ic_inline(tr, buf2);
    return node;
}

/* visit_POKE (translator.py:90-99): yield ch0(addr); yield ch1(val);
 * if ch0 is a global non-const VAR -> ic_store(ch1.type_,"*"+ch0.t,ch1.t)
 * else ic_store(ch1.type_, ch0.t, ch1.t). Parser make_typecast'd addr->
 * uinteger, val->(numbertype|ubyte) (zxbparser.py:2162-2207). */
static AstNode *tr_visit_poke(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *ch0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *ch1 = node->child_count > 1 ? node->children[1] : NULL;
    if (ch0) visitor_visit(v, ch0);
    if (ch1) visitor_visit(v, ch1);
    const char *a = (ch0 && ch0->t) ? ch0->t : "0";
    if (ch0 && ch0->tag == AST_ID &&
        ch0->u.id.class_ != CLASS_const &&
        ch0->u.id.scope == SCOPE_global) {
        size_t al = strlen(a);
        char *ind = arena_alloc(&tr->cs->arena, al + 2);
        ind[0] = '*';
        memcpy(ind + 1, a, al + 1);
        tr_ic_store(tr, ch1 ? ch1->type_ : NULL, ind,
                    (ch1 && ch1->t) ? ch1->t : "");
    } else {
        tr_ic_store(tr, ch1 ? ch1->type_ : NULL, a,
                    (ch1 && ch1->t) ? ch1->t : "");
    }
    return node;
}

/* visit_OUT (translator.py:808-811): yield child[0]; yield child[1];
 * ic_out(child[0].t, child[1].t). Parser make_typecast'd port->uinteger,
 * val->ubyte (zxbparser.py:2210-2216). */
static AstNode *tr_visit_out(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *port = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *val  = node->child_count > 1 ? node->children[1] : NULL;
    if (port) visitor_visit(v, port);
    if (val)  visitor_visit(v, val);
    tr_ic_out(tr, (port && port->t) ? port->t : "0",
                  (val && val->t) ? val->t : "0");
    return node;
}

/* visit_LOAD (translator.py:862-870) — also VERIFY (visit_VERIFY:880
 * `return self.visit_LOAD(node)`):
 *   yield c0; ic_param(TYPE.string, c0.t)
 *   yield c1; ic_param(gl.PTR_TYPE, c1.t)        # PTR_TYPE == uinteger
 *   yield c2; ic_param(gl.PTR_TYPE, c2.t)
 *   ic_param(TYPE.ubyte, int(node.token == "LOAD"))
 *   runtime_call(RuntimeLabel.LOAD_CODE, 0)
 * The ubyte flag is 1 for LOAD, 0 for VERIFY (keyed off the sentence
 * kind, the C analogue of node.token). LOAD_CODE == ".core.LOAD_CODE"
 * (io.py:62, NAMESPACE == .core). */
static AstNode *tr_visit_load(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const TypeInfo *str_t  = tr->cs->symbol_table->basic_types[TYPE_string];
    const TypeInfo *ptr_t  = tr->cs->symbol_table->basic_types[TYPE_uinteger];
    const TypeInfo *ubyte  = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    AstNode *c2 = node->child_count > 2 ? node->children[2] : NULL;

    if (c0) visitor_visit(v, c0);
    tr_ic_param(tr, str_t, (c0 && c0->t) ? c0->t : "");
    if (c1) visitor_visit(v, c1);
    tr_ic_param(tr, ptr_t, (c1 && c1->t) ? c1->t : "");
    if (c2) visitor_visit(v, c2);
    tr_ic_param(tr, ptr_t, (c2 && c2->t) ? c2->t : "");

    const char *flag =
        (node->u.sentence.kind &&
         strcmp(node->u.sentence.kind, "LOAD") == 0) ? "1" : "0";
    tr_ic_param(tr, ubyte, flag);
    tr_runtime_call(tr, ".core.LOAD_CODE", 0);
    return node;
}

/* visit_SAVE (translator.py:872-879): same first 3 params as LOAD but
 * NO ubyte flag, then runtime_call(RuntimeLabel.SAVE_CODE, 0).
 * SAVE_CODE == ".core.SAVE_CODE" (io.py:63). */
static AstNode *tr_visit_save(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const TypeInfo *str_t = tr->cs->symbol_table->basic_types[TYPE_string];
    const TypeInfo *ptr_t = tr->cs->symbol_table->basic_types[TYPE_uinteger];
    AstNode *c0 = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *c1 = node->child_count > 1 ? node->children[1] : NULL;
    AstNode *c2 = node->child_count > 2 ? node->children[2] : NULL;

    if (c0) visitor_visit(v, c0);
    tr_ic_param(tr, str_t, (c0 && c0->t) ? c0->t : "");
    if (c1) visitor_visit(v, c1);
    tr_ic_param(tr, ptr_t, (c1 && c1->t) ? c1->t : "");
    if (c2) visitor_visit(v, c2);
    tr_ic_param(tr, ptr_t, (c2 && c2->t) ? c2->t : "");

    tr_runtime_call(tr, ".core.SAVE_CODE", 0);
    return node;
}

/* BuiltinTranslator math family (builtin_translator.py:72-119): each is
 * ic_fparam(operand.type_, operand.t); runtime_call(LABEL, node.size).
 * SGN (builtin_translator.py:104-117) keys the label by the operand's
 * TSUFFIX. Returns 1 if fname handled, 0 if not (caller falls through to
 * the not-yet-ported set, mirroring "no IC" rather than Python's raise —
 * incremental-port discipline; the remaining builtins land in 4b+). */
static int tr_builtin_dispatch(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const char *fn = node->u.builtin.fname;
    AstNode *op = node->child_count > 0 ? node->children[0] : NULL;
    int nsz = node->type_ ? type_size(node->type_) : 0;
    const char *ot = (op && op->t) ? op->t : "0";

    /* RND (builtin_translator.py:60-61): zeroary; runtime_call(RND,
     * Type.float_.size). */
    if (strcmp(fn, "RND") == 0) {
        tr_runtime_call(tr, ".core.RND",
                        type_size(tr->cs->symbol_table->basic_types[TYPE_float]));
        return 1;
    }

    /* SIN/COS/TAN/ASN/ACS/ATN/EXP/LN/SQR (builtin_translator.py:72-119). */
    struct { const char *fn, *lbl; } M[] = {
        {"SIN",".core.SIN"}, {"COS",".core.COS"}, {"TAN",".core.TAN"},
        {"ASN",".core.ASIN"}, {"ACS",".core.ACOS"}, {"ATN",".core.ATAN"},
        {"EXP",".core.EXP"}, {"LN",".core.LN"}, {"SQR",".core.SQRT"},
    };
    for (size_t i = 0; i < sizeof(M)/sizeof(M[0]); i++) {
        if (strcmp(fn, M[i].fn) == 0) {
            tr_ic_fparam(tr, op ? op->type_ : NULL, ot);
            tr_runtime_call(tr, M[i].lbl, nsz);
            return 1;
        }
    }

    /* SGN (builtin_translator.py:104-117): label keyed by operand TSUFFIX. */
    if (strcmp(fn, "SGN") == 0) {
        const char *s = tr_tsuffix(op ? op->type_ : NULL);
        tr_ic_fparam(tr, op ? op->type_ : NULL, ot);
        const char *lbl =
            strcmp(s,"i8")==0  ? ".core.__SGNI8"  :
            strcmp(s,"u8")==0  ? ".core.__SGNU8"  :
            strcmp(s,"i16")==0 ? ".core.__SGNI16" :
            strcmp(s,"u16")==0 ? ".core.__SGNU16" :
            strcmp(s,"i32")==0 ? ".core.__SGNI32" :
            strcmp(s,"u32")==0 ? ".core.__SGNU32" :
            strcmp(s,"f16")==0 ? ".core.__SGNF16" :
                                 ".core.__SGNF";
        tr_runtime_call(tr, lbl, nsz);
        return 1;
    }

    /* IN (builtin_translator.py:26-27): ic_in(children[0].t). */
    if (strcmp(fn, "IN") == 0) {
        tr_ic_in(tr, ot);
        return 1;
    }

    /* INKEY (builtin_translator.py:23-24): zeroary; runtime_call(INKEY,
     * Type.string.size). */
    if (strcmp(fn, "INKEY") == 0) {
        tr_runtime_call(tr, ".core.INKEY",
                        type_size(tr->cs->symbol_table->basic_types[TYPE_string]));
        return 1;
    }

    /* ABS (builtin_translator.py:57-58): ic_abs(children[0].type_,
     * node.t, children[0].t). */
    if (strcmp(fn, "ABS") == 0) {
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_unop(tr, "abs", op ? op->type_ : NULL, node->t, ot, false);
        return 1;
    }

    /* PEEK (builtin_translator.py:66-70): NUMBER operand ->
     * ic_load(node.type_,node.t,"*#"+op.t) else "*"+op.t. */
    if (strcmp(fn, "PEEK") == 0) {
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        const char *pfx = (op && op->tag == AST_NUMBER) ? "*#" : "*";
        size_t ol = strlen(ot);
        char *src = arena_alloc(&tr->cs->arena, ol + 3);
        snprintf(src, ol + 3, "%s%s", pfx, ot);
        tr_ic_load(tr, node->type_, node->t, src);
        return 1;
    }

    /* LEN (builtin_translator.py:45-46): ic_lenstr(node.t, op.t). */
    if (strcmp(fn, "LEN") == 0) {
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_lenstr(tr, node->t, ot);
        return 1;
    }

    /* CODE (builtin_translator.py:29-35) / VAL (49-55): ic_fparam(
     * PTR_TYPE,op.t); ic_fparam(ubyte, op not STRING/VAR and op.t!="_"
     * ? 1 : 0); runtime_call(ASC, ubyte.size) | (VAL, node.size). */
    if (strcmp(fn, "CODE") == 0 || strcmp(fn, "VAL") == 0) {
        const TypeInfo *uint_t =
            tr->cs->symbol_table->basic_types[TYPE_uinteger];
        const TypeInfo *ubyte_t =
            tr->cs->symbol_table->basic_types[TYPE_ubyte];
        tr_ic_fparam(tr, uint_t, ot);
        bool freeit = op && op->tag != AST_STRING && op->tag != AST_ID &&
                      !(op->t && strcmp(op->t, "_") == 0);
        tr_ic_fparam(tr, ubyte_t, freeit ? "1" : "0");
        if (strcmp(fn, "CODE") == 0)
            tr_runtime_call(tr, ".core.__ASC",
                            type_size(ubyte_t));
        else
            tr_runtime_call(tr, ".core.VAL", nsz);
        return 1;
    }

    /* STR (builtin_translator.py:41-43): ic_fparam(float, c0.t);
     * runtime_call(STR_FAST, node.type_.size). */
    if (strcmp(fn, "STR") == 0) {
        tr_ic_fparam(tr, tr->cs->symbol_table->basic_types[TYPE_float], ot);
        tr_runtime_call(tr, ".core.__STR_FAST", nsz);
        return 1;
    }

    /* CHR (builtin_translator.py:37-39): ic_fparam(STR_INDEX_TYPE,
     * len(operand)=#args); runtime_call(CHR, node.size). */
    if (strcmp(fn, "CHR") == 0) {
        const TypeInfo *uint_t =
            tr->cs->symbol_table->basic_types[TYPE_uinteger];
        char nb[16];
        snprintf(nb, sizeof(nb), "%d", node->child_count);
        tr_ic_fparam(tr, uint_t, nb);
        tr_runtime_call(tr, ".core.CHR", nsz);
        return 1;
    }

    /* USR (builtin_translator.py:164-166) / USR_STR (159-162):
     * ic_fparam(PTR_TYPE|string, c0.t); runtime_call(USR|USR_STR,
     * node.type_.size). */
    if (strcmp(fn, "USR") == 0) {
        tr_ic_fparam(tr, tr->cs->symbol_table->basic_types[TYPE_uinteger],
                     ot);
        tr_runtime_call(tr, ".core.USR", nsz);
        return 1;
    }
    if (strcmp(fn, "USR_STR") == 0) {
        tr_ic_fparam(tr, tr->cs->symbol_table->basic_types[TYPE_string],
                     ot);
        tr_runtime_call(tr, ".core.USR_STR", nsz);
        return 1;
    }

    /* LBOUND / UBOUND runtime path (builtin_translator.py:127-157).
     * Constant dimensions are folded upstream (zxbparser.py p_expr_lbound
     * _expr const-prop) and never reach here; this is the non-constant-
     * dimension runtime lowering. operands[0]==array ID entry (child[0]),
     * operands[1]==dim expr (child[1], make_typecast'd to uinteger by the
     * parser). Python:
     *   yield node.operands[1]                       # visit dim expr
     *   ic_param(gl.BOUND_TYPE, operands[1].t)        # BOUND_TYPE=uinteger
     *   entry = operands[0]
     *   if entry.scope == global_:
     *       ic_fparam(gl.PTR_TYPE, f"#{entry.mangled}")
     *   elif entry.scope == parameter:
     *       ic_pload(gl.PTR_TYPE, entry.t, entry.offset)
     *       t1 = optemps.new_t(); ic_fparam(gl.PTR_TYPE, t1)
     *   elif entry.scope == local:
     *       ic_paddr(-entry.offset, entry.t)
     *       t1 = optemps.new_t(); ic_fparam(gl.PTR_TYPE, t1)
     *   runtime_call(LBOUND|UBOUND, self.TYPE(gl.BOUND_TYPE).size)
     * gl.BOUND_TYPE == gl.PTR_TYPE == uinteger (arch/z80/__init__.py).
     * The pload/paddr destination is entry.t but the fparam uses a fresh
     * optemps.new_t() (NOT the pload dest) — replicated bug-for-bug; the
     * pload→call register fusion in the backend carries the value. The
     * generic tr_visit_builtin already did `yield node.operand`
     * (children[0]==array ID); tr_visit_var returns immediately for a
     * CLASS_array node (visit_VARARRAY pass) so that yield is a no-op
     * here, exactly as Python's visit_VARARRAY. */
    if (strcmp(fn, "LBOUND") == 0 || strcmp(fn, "UBOUND") == 0) {
        const TypeInfo *uint_t =
            tr->cs->symbol_table->basic_types[TYPE_uinteger];
        AstNode *entry = node->child_count > 0 ? node->children[0] : NULL;
        AstNode *dim   = node->child_count > 1 ? node->children[1] : NULL;
        /* yield node.operands[1] */
        if (dim) visitor_visit(v, dim);
        /* ic_param(gl.BOUND_TYPE, operands[1].t) */
        tr_ic_param(tr, uint_t, (dim && dim->t) ? dim->t : "0");
        Scope sc = entry ? entry->u.id.scope : SCOPE_global;
        if (sc == SCOPE_global) {
            const char *mg = (entry && entry->u.id.mangled)
                            ? entry->u.id.mangled : "";
            size_t ml = strlen(mg);
            char *gp = arena_alloc(&tr->cs->arena, ml + 2);
            snprintf(gp, ml + 2, "#%s", mg);
            tr_ic_fparam(tr, uint_t, gp);
        } else if (sc == SCOPE_parameter) {
            char ofs[24];
            snprintf(ofs, sizeof(ofs), "%d", entry->u.id.offset);
            tr_ic_pload(tr, uint_t, (entry->t && entry->t[0]) ? entry->t
                                    : compiler_new_temp(tr->cs), ofs);
            tr_ic_fparam(tr, uint_t, compiler_new_temp(tr->cs));
        } else { /* SCOPE_local */
            tr_ic_paddr(tr, -(long)entry->u.id.offset,
                        (entry->t && entry->t[0]) ? entry->t
                        : compiler_new_temp(tr->cs));
            tr_ic_fparam(tr, uint_t, compiler_new_temp(tr->cs));
        }
        /* RuntimeLabel.LBOUND/UBOUND == CoreLabels.{LBOUND,UBOUND} ==
         * f"{NAMESPACE}.__LBOUND" / "...__UBOUND" (core.py:61/133,
         * NAMESPACE==CORE_NAMESPACE==".core"); backend.c RL_LBOUND/
         * RL_UBOUND + array/arraybound.asm module rows already match. */
        tr_runtime_call(tr,
                        strcmp(fn, "LBOUND") == 0 ? ".core.__LBOUND"
                                                  : ".core.__UBOUND",
                        type_size(uint_t));
        return 1;
    }

    (void)v;
    return 0;  /* not-yet-ported builtin — see follow-up */
}

/* visit_BUILTIN (translator.py:150-158): yield node.operand; dispatch to
 * visit_<fname>; if discard_result -> ic_fparam(node.type_, new_t()). */
static AstNode *tr_visit_builtin(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *op = node->child_count > 0 ? node->children[0] : NULL;
    if (op) visitor_visit(v, op);          /* yield node.operand */
    if (tr_builtin_dispatch(v, node)) {
        if (node->u.builtin.discard_result)
            tr_ic_fparam(tr, node->type_, compiler_new_temp(tr->cs));
    }
    return node;
}

/* visit_UNARY (translator.py:139-148) -> UnaryOpTranslator
 * (unary_op_translator.py). Each sub-op yields node.operand first.
 *   MINUS: ic_neg(node.type_,  node.t, operand.t)
 *   NOT:   ic_not(operand.type_, node.t, operand.t)
 *   BNOT:  ic_bnot(operand.type_, node.t, operand.t)
 *   ADDRESS: scalar global -> ic_load(node.type_, node.t, "#"+operand.t);
 *            parameter -> ic_paddr(operand.offset + operand.type_.size%2);
 *            local -> ic_paddr(-operand.offset); ARRAYACCESS -> ic_aaddr/
 *            ic_paaddr by scope (entry == operand's array ID child[0]). */
static AstNode *tr_visit_unary(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const char *opr = node->u.unary.operator;
    AstNode *operand = node->child_count > 0 ? node->children[0] : NULL;
    if (!opr) return node;

    if (strcmp(opr, "MINUS") == 0) {
        if (operand) visitor_visit(v, operand);
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_unop(tr, "neg", node->type_, node->t,
                   (operand && operand->t) ? operand->t : "", false);
        return node;
    }
    if (strcmp(opr, "NOT") == 0) {
        if (operand) visitor_visit(v, operand);
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_unop(tr, "not", operand ? operand->type_ : NULL, node->t,
                   (operand && operand->t) ? operand->t : "", true);
        return node;
    }
    if (strcmp(opr, "BNOT") == 0) {
        if (operand) visitor_visit(v, operand);
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        tr_ic_unop(tr, "bnot", operand ? operand->type_ : NULL, node->t,
                   (operand && operand->t) ? operand->t : "", false);
        return node;
    }
    if (strcmp(opr, "ADDRESS") == 0) {
        /* Bug B: Python `node.operand.scope` reads SCOPE off the OPERAND.
         * When the operand is an ARRAYACCESS, SymbolARRAYACCESS.scope
         * delegates to entry.scope (arrayaccess.py). The C union has no
         * scope on AST_ARRAYACCESS — reading `operand->u.id.scope` when
         * `operand->tag == AST_ARRAYACCESS` aliases the wrong variant
         * (always falls in SCOPE_global / undefined). Use the entry ID
         * (children[0]) scope in that case. */
        Scope scope;
        if (operand && operand->tag == AST_ARRAYACCESS) {
            AstNode *eid = operand->child_count > 0 ? operand->children[0]
                                                    : NULL;
            scope = eid ? eid->u.id.scope : SCOPE_global;
        } else {
            scope = operand ? operand->u.id.scope : SCOPE_global;
        }
        if (operand && operand->tag == AST_ARRAYACCESS) {
            /* visit_ADDRESS (unary_op_translator.py:29-37): `yield
             * node.operand`. Python's @array always wraps a freshly
             * SymbolARRAYACCESS.copy_from'd node (translator.py:260) →
             * token "ARRAYACCESS" → visit_ARRAYACCESS (push the index
             * args only, NO aload), regardless of whether the source
             * was an ARRAYLOAD read. Reproduce that by pushing the index
             * ARGUMENT children in reverse here (do NOT visitor_visit the
             * node — that would re-enter the ARRAYLOAD value path). */
            for (int i = operand->child_count - 1; i >= 1; i--)
                visitor_visit(v, operand->children[i]);
            AstNode *entry = operand->child_count > 0 ? operand->children[0]
                                                      : NULL;
            if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
            if (scope == SCOPE_global)
                tr_ic_aaddr(tr, node->t,
                            entry ? entry->u.id.mangled : "");
            else if (scope == SCOPE_parameter) {
                char ob[24];
                snprintf(ob, sizeof(ob), "*%ld",
                         entry ? (long)entry->u.id.offset : 0);
                tr_ic_paaddr(tr, node->t, ob);
            } else { /* local */
                char ob[24];
                snprintf(ob, sizeof(ob), "%ld",
                         entry ? -(long)entry->u.id.offset : 0);
                tr_ic_paaddr(tr, node->t, ob);
            }
            return node;
        }
        /* scalar variable */
        if (node->t == NULL) node->t = compiler_new_temp(tr->cs);
        if (scope == SCOPE_global) {
            size_t ol = operand && operand->t ? strlen(operand->t) : 0;
            char *src = arena_alloc(&tr->cs->arena, ol + 2);
            src[0] = '#';
            memcpy(src + 1, (operand && operand->t) ? operand->t : "", ol);
            src[ol + 1] = '\0';
            tr_ic_load(tr, node->type_, node->t, src);
        } else if (scope == SCOPE_parameter) {
            int sz = operand && operand->type_
                       ? type_size(operand->type_) : 0;
            tr_ic_paddr(tr,
                        (long)(operand ? operand->u.id.offset : 0) + (sz % 2),
                        node->t);
        } else { /* local */
            tr_ic_paddr(tr, -(long)(operand ? operand->u.id.offset : 0),
                        node->t);
        }
        return node;
    }
    fprintf(stderr, "zxbc: visit_UNARY unknown operator '%s'\n", opr);
    return node;
}

/* visit_CONSTEXPR (arch/z80/visitor/translator.py:130-131):
 *     def visit_CONSTEXPR(self, node):
 *         yield node.t
 * i.e. emit NO runtime code — the consuming op (POKE/LET/array AT/...)
 * reads node.t. SymbolCONSTEXPR.t (constexpr.py:43-45) ==
 * "#" + Translator.traverse_const(self). Set node->t here so the parent
 * visitor reads the static folded operand; crucially do NOT recurse the
 * UNARY ADDRESS child (visitor_generic would, re-emitting the empty
 * `ld hl,`). This is the fold that makes @label / DIM..AT @label match
 * Python (no spurious runtime). */
static AstNode *tr_visit_constexpr(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const char *r = tr_traverse_const(tr, node);
    size_t rl = strlen(r);
    char *s = arena_alloc(&tr->cs->arena, rl + 2);
    s[0] = '#';
    memcpy(s + 1, r, rl + 1);
    node->t = s;
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

/* ====================================================================
 *  S5.8d — visit_RESTORE / visit_READ                                  *
 *  (src/arch/z80/visitor/translator.py:479-496 / :498-529)             *
 *                                                                      *
 *  RuntimeLabel.RESTORE == ".core.__RESTORE",                          *
 *  RuntimeLabel.READ    == ".core.__READ"                              *
 *  (runtime/datarestore.py:12-13; NAMESPACE == .core). gl.PTR_TYPE ==  *
 *  uinteger (arch/z80/__init__.py:24). A declared label's .type_ is    *
 *  basic_types[PTR_TYPE] == uinteger (symboltable.py:627), so the      *
 *  RESTORE-with-label fparam type is uinteger too — same as the        *
 *  no-label gl.PTR_TYPE branch.                                        *
 * ==================================================================== */

/* visit_RESTORE (translator.py:479-496). C RESTORE SENTENCE:
 * child[0] = the label AST_ID (class_==CLASS_label) or NO children.
 * node.args == sentence children. */
static AstNode *tr_visit_restore(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    CompilerState *cs = tr->cs;

    /* :480-481  if not gl.DATA_IS_USED: return  (RESTORE is inert when
     * no READ exists — restore0/restore2 emit nothing). */
    if (!cs->data_is_used)
        return node;

    AstNode *lblnode = node->child_count > 0 ? node->children[0] : NULL;
    const char *lbl;
    const TypeInfo *type_;

    if (!lblnode) {
        /* :483-489  no label: if not gl.DATAS -> error; return.
         * else lbl = gl.DATAS[0].label.name; type_ = gl.PTR_TYPE. */
        if (cs->datas.len == 0) {
            err_no_data_defined(cs, node->lineno);
            return node;
        }
        lbl = cs->datas.data[0]->label_name;
        type_ = cs->symbol_table->basic_types[TYPE_uinteger]; /* PTR_TYPE */
    } else {
        /* :491-492  lbl = gl.DATA_LABELS[node.args[0].name];
         *           type_ = node.args[0].type_
         * A declared label's type_ is basic_types[PTR_TYPE]==uinteger
         * (symboltable.py:627); the C label AST_ID does not carry the
         * resolved entry type, so use uinteger directly — byte-identical
         * to Python and faithful to the source value. */
        const char *nm = lblnode->u.id.name ? lblnode->u.id.name : "";
        const char *mapped = hashmap_get(&cs->data_labels, nm);
        /* Python KeyErrors if the label was never make_label'd; the
         * corpus never hits that (RESTORE targets are always declared
         * labels — make_label ran). Fall back to the name itself only
         * defensively. */
        lbl = mapped ? mapped : nm;
        type_ = cs->symbol_table->basic_types[TYPE_uinteger];
    }

    /* :494  gl.DATA_LABELS_REQUIRED.add(lbl) */
    hashmap_set(&cs->data_labels_required, lbl, (void *)1);

    /* :495  self.ic_fparam(type_, "#" + lbl) */
    size_t ll = strlen(lbl);
    char *hl = arena_alloc(&cs->arena, ll + 2);
    hl[0] = '#';
    memcpy(hl + 1, lbl, ll + 1);
    tr_ic_fparam(tr, type_, hl);

    /* :496  self.runtime_call(RuntimeLabel.RESTORE, 0) */
    tr_runtime_call(tr, ".core.__RESTORE", 0);
    return node;
}

/* visit_READ (translator.py:498-529). C READ SENTENCE: child[0] = the
 * read target (AST_ID var, or AST_ARRAYACCESS). The parser already
 * rejected array/non-var targets (parser.c BTOK_READ arm) so a target
 * that reaches here is a scalar VAR or an ARRAYACCESS. */
static AstNode *tr_visit_read(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    CompilerState *cs = tr->cs;

    /* :499-501  if not gl.DATAS: error; return. (In practice main.c's
     * data_is_used && datas.len==0 guard short-circuits codegen before
     * this, but keep the faithful guard.) */
    if (cs->datas.len == 0) {
        err_no_data_defined(cs, node->lineno);
        return node;
    }

    AstNode *tgt = node->child_count > 0 ? node->children[0] : NULL;
    const TypeInfo *ubyte = cs->symbol_table->basic_types[TYPE_ubyte];

    /* :503  ic_fparam(TYPE.ubyte,
     *                  "#" + str(DATA_TYPES[TSUFFIX(args[0].type_)])) */
    char codebuf[16];
    snprintf(codebuf, sizeof(codebuf), "#%d",
             tr_data_type_code(tgt ? tgt->type_ : NULL));
    tr_ic_fparam(tr, ubyte, arena_strdup(&cs->arena, codebuf));

    /* :504  runtime_call(RuntimeLabel.READ, args[0].type_.size) */
    int sz = (tgt && tgt->type_) ? type_size(tgt->type_) : 0;
    tr_runtime_call(tr, ".core.__READ", sz);

    /* :506-529  store the read value. */
    if (tgt && tgt->tag == AST_ARRAYACCESS) {
        /* :506-527  array-element store-back of the __READ result.  This
         * mirrors visit_LETARRAY's dynamic store branch (translator.py
         * :334-342, ported at translator.c LETARRAY around the
         * AST_ARRAYACCESS / offset_is_none path).  Python's translator.py
         * :511-519:
         *   t = optemps.new_t()
         *   if arr.offset is None:
         *       yield arr                              # push indices
         *       global    : ic_astore (type_, entry.mangled, t)
         *       parameter : ic_pastore(type_, entry.offset, t)
         *       local     : ic_pastore(type_, -entry.offset, t)
         *   else:                                      # const offset
         *       global    : ic_store  (type_, "name + N", t)
         *       parameter : ic_pstore (type_, entry.offset - N, t)
         *       local     : ic_pstore (type_, -(entry.offset - N), t)
         * `t` is the read value — there is no RHS expression to visit.
         * Faithful to visit_LETARRAY's existing arr.offset-is-None
         * resolution (param-scope OR'd in: arrayaccess.py:77-78 returns
         * None when scope == SCOPE.parameter). */
        AstNode *aent = tgt->child_count > 0 ? tgt->children[0] : NULL;
        bool a_global = aent && aent->tag == AST_ID &&
                        aent->u.id.scope == SCOPE_global;
        bool a_param  = aent && aent->tag == AST_ID &&
                        aent->u.id.scope == SCOPE_parameter;
        bool a_local  = aent && aent->tag == AST_ID &&
                        aent->u.id.scope == SCOPE_local;
        long a_eoff = aent ? aent->u.id.offset : 0;
        char *t = compiler_new_temp(cs);
        bool offset_is_none = !tgt->u.arrayaccess.is_const || a_param;

        if (offset_is_none) {
            /* Python wraps the parsed ARRAYLOAD target in a fresh
             * SymbolARRAYACCESS (zxbparser.py:1821-1828): the write-side
             * shape that visit_ARRAYACCESS (translator.py:448-449)
             * resolves to `yield node.arglist` only — push the index
             * values, NO aload.  The C parser only ever builds the
             * is_load=true ARRAYACCESS for an expression-context
             * subscript (parser.c:1060) and the READ statement reuses
             * that node, so a plain visitor_visit here would dispatch
             * to the is_load branch and emit a __LOADF/__LOAD readback
             * we don't want.  Replicate the non-load arm directly
             * (translator.c:2908-2912 — push children in reverse). */
            for (int i = tgt->child_count - 1; i >= 1; i--)
                visitor_visit(v, tgt->children[i]);
            if (a_global) {
                tr_ic_astore(tr, tgt->type_,
                             aent->u.id.mangled ? aent->u.id.mangled : "",
                             t);
            } else if (a_param) {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "*%ld", a_eoff);
                tr_ic_pastore(tr, tgt->type_, ofs, t);
            } else if (a_local) {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "%ld", -a_eoff);
                tr_ic_pastore(tr, tgt->type_, ofs, t);
            } else {
                /* Unknown scope — fall back to generic descent (preserve
                 * pre-port behaviour for any case the array-store IC
                 * doesn't yet cover; never silently swallow). */
                visitor_generic(v, tgt);
            }
        } else {
            /* Constant offset (:520-527).  name = arr.entry.data_label. */
            long aoff = tgt->u.arrayaccess.offset;
            if (a_global) {
                const char *name = tr_array_data_label(tr, aent);
                size_t need = strlen(name) + 32;
                char *addr = arena_alloc(&tr->cs->arena, need);
                snprintf(addr, need, "%s + %ld", name, aoff);
                tr_ic_store(tr, tgt->type_, addr, t);
            } else if (a_param) {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "%ld", a_eoff - aoff);
                tr_ic_pstore(tr, tgt->type_, ofs, t);
            } else if (a_local) {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "%ld", -(a_eoff - aoff));
                tr_ic_pstore(tr, tgt->type_, ofs, t);
            } else {
                visitor_generic(v, tgt);
            }
        }
    } else if (tgt) {
        /* :529  emit_var_assign(node.args[0], t=optemps.new_t()) */
        tr_emit_var_assign(tr, tgt, compiler_new_temp(cs));
    }
    return node;
}

/* ====================================================================
 * S7.1b-i — visit_PRINT (core: string + numeric + ';'/',' + trailing EOL)
 *
 * Faithful port of src/arch/z80/visitor/translator.py:813-846:
 *
 *   def visit_PRINT(self, node):
 *       self.norm_attr()                        # runtime_call(COPY_ATTR,0)
 *       for i in node.children:
 *           yield i
 *           if i.token in ("PRINT_TAB","PRINT_AT","PRINT_COMMA")
 *                          + self.ATTR_TMP:
 *               continue
 *           self.ic_fparam(i.type_, i.t)
 *           label = { bool:PRINTU8, i8:PRINTI8, u8:PRINTU8, i16:PRINTI16,
 *                     u16:PRINTU16, i32:PRINTI32, u32:PRINTU32,
 *                     f16:PRINTF16, f:PRINTF, str:PRINTSTR
 *                   }[self.TSUFFIX(i.type_)]
 *           self.runtime_call(label, 0)
 *       if node.eol:
 *           self.runtime_call(RuntimeLabel.PRINT_EOL, 0)
 *
 * norm_attr (translator_visitor.py:172-174) == runtime_call(COPY_ATTR,0).
 * The skip-list elements are SENTENCE kinds only — a bare-expr child is
 * never one of them, so the C check is "child is an AST_SENTENCE whose
 * kind is in the list". ATTR_TMP (translator_visitor.py:40-41) =
 * (INK_TMP,PAPER_TMP,BRIGHT_TMP,FLASH_TMP,OVER_TMP,INVERSE_TMP,
 *  BOLD_TMP,ITALIC_TMP). The PRINT_AT/PRINT_TAB/PRINT_COMMA/_TMP child
 * visitors themselves are S7.1b-ii/iii — but the skip path must already
 * exist here (Python's loop has it unconditionally), so PRINT does not
 * fparam/print a control child. The per-type label is keyed by
 * tr_tsuffix (the faithful TSUFFIX port, translator.c:99). */

/* The Python `i.token in (...) + self.ATTR_TMP` skip-set, as SENTENCE
 * kinds. ATTR_TMP is the *_TMP attr family (S7.1b-iii); listed here
 * because Python's visit_PRINT loop skips them unconditionally. */
static bool tr_print_is_control_child(const AstNode *child) {
    if (child == NULL || child->tag != AST_SENTENCE ||
        child->u.sentence.kind == NULL)
        return false;
    const char *k = child->u.sentence.kind;
    return strcmp(k, "PRINT_TAB")   == 0 ||
           strcmp(k, "PRINT_AT")    == 0 ||
           strcmp(k, "PRINT_COMMA") == 0 ||
           strcmp(k, "INK_TMP")     == 0 ||
           strcmp(k, "PAPER_TMP")   == 0 ||
           strcmp(k, "BRIGHT_TMP")  == 0 ||
           strcmp(k, "FLASH_TMP")   == 0 ||
           strcmp(k, "OVER_TMP")    == 0 ||
           strcmp(k, "INVERSE_TMP") == 0 ||
           strcmp(k, "BOLD_TMP")    == 0 ||
           strcmp(k, "ITALIC_TMP")  == 0;
}

/* visit_PRINT dict: TSUFFIX(i.type_) -> RuntimeLabel string. The .core
 * label strings mirror the existing handler convention (see tr_visit_stop
 * ".core.__STOP", tr_visit_restore ".core.__RESTORE") and io.py values. */
static const char *tr_print_label_for(const char *suf) {
    if (strcmp(suf, "bool") == 0) return ".core.__PRINTU8";
    if (strcmp(suf, "i8")   == 0) return ".core.__PRINTI8";
    if (strcmp(suf, "u8")   == 0) return ".core.__PRINTU8";
    if (strcmp(suf, "i16")  == 0) return ".core.__PRINTI16";
    if (strcmp(suf, "u16")  == 0) return ".core.__PRINTU16";
    if (strcmp(suf, "i32")  == 0) return ".core.__PRINTI32";
    if (strcmp(suf, "u32")  == 0) return ".core.__PRINTU32";
    if (strcmp(suf, "f16")  == 0) return ".core.__PRINTF16";
    if (strcmp(suf, "f")    == 0) return ".core.__PRINTF";
    if (strcmp(suf, "str")  == 0) return ".core.__PRINTSTR";
    /* Python would KeyError on any other suffix — fail loud, do not
     * silently emit a wrong PRINT routine. */
    fprintf(stderr, "zxbc: visit_PRINT: no PRINT label for TSUFFIX '%s'\n",
            suf);
    return ".core.__PRINTU8";
}

static AstNode *tr_visit_print(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;

    /* :814  self.norm_attr() == runtime_call(COPY_ATTR, 0). */
    tr_runtime_call(tr, ".core.COPY_ATTR", 0);

    /* :815-843  for i in node.children: yield i; ... */
    for (int idx = 0; idx < node->child_count; idx++) {
        AstNode *i = node->children[idx];

        /* yield i — visit the child subtree (value-load / control-call). */
        if (i) visitor_visit(v, i);

        /* :820-829  skip the print-control children (their own visitors
         * already did the work; PRINT must not fparam/print them). */
        if (tr_print_is_control_child(i))
            continue;

        /* :831  self.ic_fparam(i.type_, i.t) */
        tr_ic_fparam(tr, i ? i->type_ : NULL,
                     (i && i->t) ? i->t : "");

        /* :832-843  label = {...}[TSUFFIX(i.type_)]; runtime_call(label,0) */
        const char *suf = tr_tsuffix(i ? i->type_ : NULL);
        tr_runtime_call(tr, tr_print_label_for(suf), 0);
    }

    /* :845-846  if node.eol: runtime_call(PRINT_EOL, 0) */
    if (node->u.sentence.eol)
        tr_runtime_call(tr, ".core.PRINT_EOL", 0);

    return node;
}

/* visit_PRINT_COMMA (translator.py:860-861): runtime_call(PRINT_COMMA,0)
 * — no children. The ',' separator's PRINT_COMMA SENTENCE child is
 * visited via visit_PRINT's `yield i`; this emits the comma-tab call,
 * then visit_PRINT's skip-list (tr_print_is_control_child) keeps PRINT
 * from fparam/printing the sentence. PRINT_AT/PRINT_TAB visit handlers
 * are deliberately deferred to S7.1b-ii. */
static AstNode *tr_visit_print_comma(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    tr_runtime_call(tr, ".core.PRINT_COMMA", 0);
    return node;
}

/* visit_PRINT_AT (translator.py:848-853):
 *   yield node.children[0]
 *   self.ic_param(TYPE.ubyte, node.children[0].t)
 *   yield node.children[1]
 *   self.ic_fparam(TYPE.ubyte, node.children[1].t)
 *   self.runtime_call(RuntimeLabel.PRINT_AT, 0)  # discard return
 * Asymmetry is faithful: child[0] (row) ic_param; child[1] (col, last
 * pushed) ic_fparam. PRINT_AT label = io.py:55 f"{NAMESPACE}.PRINT_AT"
 * (NAMESPACE == .core; no double-underscore mangle, matching the
 * .core.PRINT_COMMA / .core.PRINT_EOL convention). */
static AstNode *tr_visit_print_at(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    AstNode *row = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *col = node->child_count > 1 ? node->children[1] : NULL;
    if (row) visitor_visit(v, row);
    tr_ic_param(tr, ubyte, (row && row->t) ? row->t : "");
    if (col) visitor_visit(v, col);
    tr_ic_fparam(tr, ubyte, (col && col->t) ? col->t : "");
    tr_runtime_call(tr, ".core.PRINT_AT", 0);
    return node;
}

/* visit_PRINT_TAB (translator.py:855-858):
 *   yield node.children[0]
 *   self.ic_fparam(TYPE.ubyte, node.children[0].t)
 *   self.runtime_call(RuntimeLabel.PRINT_TAB, 0)
 * Single child (col) uses ic_fparam. PRINT_TAB label = io.py:59
 * f"{NAMESPACE}.PRINT_TAB" (.core, no mangle). */
static AstNode *tr_visit_print_tab(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    AstNode *col = node->child_count > 0 ? node->children[0] : NULL;
    if (col) visitor_visit(v, col);
    tr_ic_fparam(tr, ubyte, (col && col->t) ? col->t : "");
    tr_runtime_call(tr, ".core.PRINT_TAB", 0);
    return node;
}

/* visit_ATTR_TMP (translator_visitor.py:103-118) — the SINGLE handler
 * for all 8 in-PRINT temporary attrs (INK/PAPER/FLASH/BRIGHT/INVERSE/
 * OVER/BOLD/ITALIC _TMP):
 *   yield node.children[0]
 *   self.ic_fparam(node.children[0].type_, node.children[0].t)
 *   label = { "INK_TMP": RuntimeLabel.INK_TMP, ... }[node.token]
 *   self.runtime_call(label, 0)            # discard return
 * NOTE: ic_fparam uses the CHILD's OWN type_ (ubyte, because the parser
 * make_typecast'd the operand to ubyte) — NOT a hardcoded constant.
 * Labels are .core.<NAME>_TMP (io.py:25-32, NAMESPACE == .core, NO
 * double-underscore mangle — same convention as .core.PRINT_COMMA).
 * Python's _visit (translator_visitor.py:164-170) routes any token in
 * ATTR_TMP here; the C visitor dispatches by SENTENCE kind, registered
 * for all 8 "<NAME>_TMP" kinds in tr_register_handlers. */
static AstNode *tr_visit_attr_tmp(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *child0 = node->child_count > 0 ? node->children[0] : NULL;
    if (child0) visitor_visit(v, child0);
    tr_ic_fparam(tr, child0 ? child0->type_ : NULL,
                 (child0 && child0->t) ? child0->t : "");

    const char *kind = node->u.sentence.kind;
    const char *label = NULL;
    if      (strcmp(kind, "INK_TMP")     == 0) label = ".core.INK_TMP";
    else if (strcmp(kind, "PAPER_TMP")   == 0) label = ".core.PAPER_TMP";
    else if (strcmp(kind, "FLASH_TMP")   == 0) label = ".core.FLASH_TMP";
    else if (strcmp(kind, "BRIGHT_TMP")  == 0) label = ".core.BRIGHT_TMP";
    else if (strcmp(kind, "INVERSE_TMP") == 0) label = ".core.INVERSE_TMP";
    else if (strcmp(kind, "OVER_TMP")    == 0) label = ".core.OVER_TMP";
    else if (strcmp(kind, "BOLD_TMP")    == 0) label = ".core.BOLD_TMP";
    else if (strcmp(kind, "ITALIC_TMP")  == 0) label = ".core.ITALIC_TMP";
    tr_runtime_call(tr, label, 0);
    return node;
}

/* ====================================================================
 * String / array-element assignment + string-slice visitors —
 * faithful ports of src/arch/z80/visitor/translator.py:
 *   visit_LETARRAY        :329-364
 *   visit_LETSUBSTR       :366-402
 *   visit_LETARRAYSUBSTR  :404-446
 *   visit_STRSLICE        :451-468
 *
 * The C parser builds a single "LETARRAY" SENTENCE for BOTH a real
 * array-element assignment (child[0] = ARRAYACCESS) and a string
 * substring assignment (child[0] = FUNCCALL-shape: VAR string +
 * ARGLIST) — see parser.c:1644. Python instead builds LETARRAY /
 * LETSUBSTR / LETARRAYSUBSTR distinctly. `entry` (the assigned symbol)
 * is in both C shapes node.children[0].children[0] — the VAR id under
 * the ARRAYACCESS/FUNCCALL — the faithful analogue of Python's
 * node.children[0].entry (visit_LETARRAYSUBSTR :405) /
 * node.args[0].entry (visit_LETARRAY :333) / node.children[0]
 * (visit_LETSUBSTR :361).
 *
 * THE GATE (translator.py:330-331 visit_LETARRAY + :405-406
 * visit_LETARRAYSUBSTR): `if O_LEVEL > 1 and not entry.accessed:
 * return` — emit NOTHING and do NOT descend into children. This is the
 * `llc` zero-regression fix: a write-only string/array at O>1 must not
 * have its RHS STRING visited (which would mint a spurious constant
 * label via tr_visit_string). Without a LETARRAY handler the default
 * visitor_generic recurses into the RHS STRING. ==================================================================== */

/* The assigned symbol entry under a C "LETARRAY" lvalue (ARRAYACCESS or
 * FUNCCALL-shape): node.children[0].children[0]. */
static AstNode *tr_letarray_entry(AstNode *node) {
    AstNode *acc = node->child_count > 0 ? node->children[0] : NULL;
    return (acc && acc->child_count > 0) ? acc->children[0] : NULL;
}

/* visit_LETSUBSTR (translator.py:366-402): LET X$(a TO b) = Y$.
 * Faithful to the C "LETARRAY"-string shape: str_var = entry (the VAR
 * under the FUNCCALL), the index expr(s) live in the FUNCCALL's
 * ARGLIST → ARGUMENT children. p_substr_assignment (zxbparser.py
 * :1248-1305) with one index makes lower==upper==index, RHS = expr. */
static void tr_letsubstr_emit(Visitor *v, AstNode *node, AstNode *str_var,
                              AstNode *lower, AstNode *upper, AstNode *rhs) {
    Translator *tr = v->ctx;
    const TypeInfo *ptr = tr->cs->symbol_table->basic_types[TYPE_uinteger];
    const TypeInfo *str_t = tr->cs->symbol_table->basic_types[TYPE_string];
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];

    /* :369 yield Y$ */
    if (rhs) visitor_visit(v, rhs);

    /* :371-376 is_temporary_value(node.children[3]) (check.py:399-401):
     *   token not in ("STRING","VAR") and t[0] not in ("_","#"). */
    bool rhs_is_str_or_var = rhs && (rhs->tag == AST_STRING ||
                                     rhs->tag == AST_ID);
    const char *rt = (rhs && rhs->t) ? rhs->t : "";
    bool rhs_tmp = !rhs_is_str_or_var && rt[0] != '_' && rt[0] != '#';
    if (rhs_tmp) {
        tr_ic_param(tr, str_t, rt);
        tr_ic_param(tr, ubyte, "1");
    } else {
        tr_ic_param(tr, ptr, rt);
        tr_ic_param(tr, ubyte, "0");
    }

    /* :379-383 yield a; ic_param(PTR,a.t); yield b; ic_param(PTR,b.t).
     * Python p_substr_assignment (zxbparser.py:1297-1305) wraps each
     * index in BINARY MINUS(idx, string_base) — for a runtime idx the
     * MINUS does NOT fold (make_binary's func) and emits a `subu16
     * ttemp idx.t base` quad whose backend output (a `pop hl; push hl`
     * pair on strbase==0) is part of byte-identical parity.
     *
     * The C parser pre-wraps the STRSLICE's children in MINUS(idx,
     * base) ONLY when string_base != 0 (parser.c:1205, with the
     * deliberate empirical "x-0 folds at IR level" comment, which is
     * true for the READ path but NOT for the WRITE / LETSUBSTR path —
     * `pop hl;push hl` survives the IR but matters at the asm). When
     * the parser already did the rewrite, visiting `lower` runs the
     * BINARY MINUS and produces the temp; we just ic_param it. When
     * the parser skipped (strbase==0), we emit the missing subu16
     * here so the IR matches Python's. */
    bool parser_did_rewrite = (tr->cs->opts.string_base != 0);
    const char *base_s = NULL;
    if (!parser_did_rewrite) {
        char base_buf[16];
        snprintf(base_buf, sizeof(base_buf), "%d",
                 tr->cs->opts.string_base);
        base_s = arena_strdup(&tr->cs->arena, base_buf);
    }
    if (lower) visitor_visit(v, lower);
    const char *lower_t;
    if (parser_did_rewrite) {
        lower_t = (lower && lower->t) ? lower->t : "0";
    } else {
        const char *lt = (lower && lower->t) ? lower->t : "0";
        if (lower && lower->tag == AST_NUMBER) {
            char folded[24];
            long lv = (long)lower->u.number.value;
            snprintf(folded, sizeof(folded), "%ld",
                     lv - tr->cs->opts.string_base);
            lower_t = arena_strdup(&tr->cs->arena, folded);
        } else {
            const char *t14 = compiler_new_temp(tr->cs);
            tr_ic_arith(tr, "sub", ptr, t14, lt, base_s);
            lower_t = t14;
        }
    }
    tr_ic_param(tr, ptr, lower_t);
    if (upper) visitor_visit(v, upper);
    const char *upper_t;
    if (parser_did_rewrite) {
        upper_t = (upper && upper->t) ? upper->t : "0";
    } else {
        const char *ut = (upper && upper->t) ? upper->t : "0";
        if (upper && upper->tag == AST_NUMBER) {
            char folded[24];
            long uv = (long)upper->u.number.value;
            snprintf(folded, sizeof(folded), "%ld",
                     uv - tr->cs->opts.string_base);
            upper_t = arena_strdup(&tr->cs->arena, folded);
        } else {
            const char *t15 = compiler_new_temp(tr->cs);
            tr_ic_arith(tr, "sub", ptr, t15, ut, base_s);
            upper_t = t15;
        }
    }
    tr_ic_param(tr, ptr, upper_t);

    /* :385-400 load x$ by scope. C scope/offset/byref on the VAR id.
     * For a NON-global string scalar/param, Python VarRef.t returns
     * f"${self._t}" (varref.py:38-42) — a per-symbol cached optemp with
     * the '$' prefix. The parser stamps .t = mangled on every id; for a
     * local/parameter that mangle is wrong (a later ic_fparam would
     * emit `ld hl,(_mangled)` reading global storage). Mirror the
     * caching logic from tr_visit_var (translator.c:350-367): if .t is
     * still the parser mangle, mint a temp; if dynamic and no '$'
     * prefix, prepend it. Cache once so every reference matches. */
    Scope scope = str_var->u.id.scope;
    const char *mangled = str_var->u.id.mangled;
    if (scope != SCOPE_global) {
        if (str_var->t && mangled && strcmp(str_var->t, mangled) == 0)
            str_var->t = (char *)compiler_new_temp(tr->cs);
        const char *dst = (str_var->t && str_var->t[0]) ? str_var->t
                        : compiler_new_temp(tr->cs);
        if (str_var->type_ && type_is_dynamic(str_var->type_) &&
            dst[0] != '$') {
            char *ds = arena_alloc(&tr->cs->arena, strlen(dst) + 2);
            ds[0] = '$';
            strcpy(ds + 1, dst);
            dst = ds;
        }
        str_var->t = (char *)dst;
    }
    const char *svt = str_var->t ? str_var->t : mangled;
    if (scope == SCOPE_global) {
        tr_ic_fparam(tr, ptr, svt ? svt : "");
    } else if (scope == SCOPE_local) {
        char ofs[24];
        snprintf(ofs, sizeof(ofs), "%ld", -(long)str_var->u.id.offset);
        tr_ic_pload(tr, ptr, svt, ofs);
        tr_ic_fparam(tr, ptr, svt);
    } else if (scope == SCOPE_parameter) {
        char ofs[24];
        snprintf(ofs, sizeof(ofs), "%ld", (long)str_var->u.id.offset);
        tr_ic_pload(tr, ptr, svt, ofs);
        if (str_var->u.id.byref) {
            char ind[32];
            snprintf(ind, sizeof(ind), "*%s", svt);
            tr_ic_fparam(tr, ptr, arena_strdup(&tr->cs->arena, ind));
        } else {
            tr_ic_fparam(tr, ptr, svt);
        }
    } else {
        fprintf(stderr, "zxbc: visit_LETSUBSTR invalid scope %d\n",
                (int)scope);
        return;
    }
    /* :402 runtime_call(LETSUBSTR, 0) */
    tr_runtime_call(tr, ".core.__LETSUBSTR", 0);
}

/* ArrayRef.data_label (id_/ref/arrayref.py:62-63):
 *   f"{parent.mangled}.{gl.ARRAY_DATA_PREFIX}"  (ARRAY_DATA_PREFIX ==
 *   "__DATA__", global_.py:173). For a GLOBAL array this is also
 *   ArrayRef.t (arrayref.py:69-78: parent.scope == global_ → data_label).
 *   Same string the data-image emitter builds (var_translator.c:467-468). */
static const char *tr_array_data_label(Translator *tr, AstNode *entry) {
    const char *mangled = (entry && entry->u.id.mangled)
                              ? entry->u.id.mangled : "";
    size_t ml = strlen(mangled);
    char *buf = arena_alloc(&tr->cs->arena, ml + 10);
    snprintf(buf, ml + 10, "%s.__DATA__", mangled);
    return buf;
}

/* visit_ARRAYLOAD (translator.py:265-289) — array-element value READ.
 * The C parser builds AST_ARRAYACCESS for every array access (read AND
 * write target; it never mints a distinct AST_ARRAYLOAD). When the node
 * is VISITED AS A VALUE (POKE value, expression operand) it is Python's
 * ARRAYLOAD — make_call builds ARRAYLOAD for p_arr_access_expr
 * (zxbparser.py:388-392); the LETARRAY write target is handled in
 * tr_visit_letarray, which does not descend here for the cases it owns.
 *
 * SCOPED SUBSET (faithful, regression-free): only the CONSTANT-offset
 * GLOBAL branch (translator.py:277-280) is ported here —
 *   offset = node.offset
 *   if scope == global_:
 *       ic_load(type_, entry.t, "%s + %i" % (entry.t, offset))
 * with entry.t == ArrayRef.t == data_label for a global array. The
 * dynamic (`offset is None`) path and the parameter/local const paths
 * pull the __ARRAY runtime / IX-relative pload sequences not yet ported;
 * for those this handler reproduces the pre-existing default behaviour
 * (visitor_generic recurses the index children) so nothing regresses —
 * those cluster members stay DIVERGE exactly as before. */
static AstNode *tr_visit_arrayaccess(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *entry = node->child_count > 0 ? node->children[0] : NULL;

    bool is_const = node->u.arrayaccess.is_const;
    bool global = entry && entry->tag == AST_ID &&
                  entry->u.id.scope == SCOPE_global;

    /* visit_ARRAYACCESS (translator.py:448-449): `yield node.arglist`.
     * This is the LETARRAY store-target node (Python sym.ARRAYACCESS, not
     * ARRAYLOAD): it ONLY pushes the index values (in reverse, via the
     * ARGUMENT children) — NO aload/ic_load. The visit_LETARRAY dynamic
     * arm `yield arr` reaches here. */
    if (!node->u.arrayaccess.is_load) {
        for (int i = node->child_count - 1; i >= 1; i--)
            visitor_visit(v, node->children[i]);
        return node;
    }

    if (is_const && global && entry) {
        /* translator.py:278-280 (const, global):
         *   ic_load(node.type_, node.entry.t, "%s + %i" % (entry.t, off))
         * entry.t == data_label for a global array (arrayref.py:69-72).
         * %i is decimal. _load8/_load16 read ins[2] (the "_x.__DATA__ +
         * N" label) → "ld a,(...)" / "ld hl,(...)" and push the result;
         * the consumer reads node.t (Symbol.t lazy optemps.new_t(),
         * symbol_.py:72-76), independent of the ic_load t1. */
        const char *dlabel = tr_array_data_label(tr, entry);
        size_t need = strlen(dlabel) + 32;
        char *t2 = arena_alloc(&tr->cs->arena, need);
        snprintf(t2, need, "%s + %ld", dlabel, node->u.arrayaccess.offset);
        tr_ic_load(tr, node->type_, dlabel, t2);
        if (node->t == NULL)
            node->t = compiler_new_temp(tr->cs);
        return node;
    }

    /* visit_ARRAYLOAD (translator.py:265-289). scope = node.entry.scope.
     * node.t is the lazy result temp; ensure it exists (consumer reads
     * it). The index args were typecast to BOUND_TYPE (uinteger) at
     * parse; visiting them in REVERSE (visit_ARGLIST translator.py:210-
     * 212) emits paramu16 pushes — the C parser flattened the ARGLIST
     * ARGUMENT children into node->children[1..]. */
    if (node->t == NULL)
        node->t = compiler_new_temp(tr->cs);

    const TypeInfo *ptr_t =
        tr->cs->symbol_table->basic_types[TYPE_uinteger]; /* gl.PTR_TYPE */
    int ptr_size = 2;                                     /* uinteger size */
    bool is_param = entry && entry->tag == AST_ID &&
                    entry->u.id.scope == SCOPE_parameter;
    bool is_local = entry && entry->tag == AST_ID &&
                    entry->u.id.scope == SCOPE_local;
    long eoff = entry ? entry->u.id.offset : 0;

    /* Python SymbolARRAYACCESS.offset returns None UNCONDITIONALLY for
     * scope == SCOPE.parameter (arrayaccess.py:77-78), so visit_ARRAYLOAD
     * takes the `offset is None` (ic_paload) path for a param array even
     * with a constant subscript. The const-param ic_pload branch below is
     * never reached in Python — fold the param-scope override into the
     * dynamic gate to match `node.offset is None`. */
    if (!is_const || is_param) {
        /* :268-273 — yield node.args (the index ARGUMENT children, in
         * reverse); then scope-dispatched aload/paload. */
        for (int i = node->child_count - 1; i >= 1; i--)
            visitor_visit(v, node->children[i]);

        if (global && entry) {
            const char *dlabel = tr_array_data_label(tr, entry);
            tr_ic_aload(tr, node->type_, dlabel,
                        entry->u.id.mangled ? entry->u.id.mangled : "");
        } else if (is_param) {
            char ofs[24];
            snprintf(ofs, sizeof(ofs), "*%ld", eoff);
            tr_ic_paload(tr, node->type_, node->t, ofs);
        } else if (is_local) {
            char ofs[24];
            snprintf(ofs, sizeof(ofs), "%ld", -eoff);
            tr_ic_paload(tr, node->type_, node->t, ofs);
        } else {
            visitor_generic(v, node);
        }
        return node;
    }

    /* Constant offset, non-global (:275-289). offset == arrayaccess.offset.
     * NB: Python does NOT yield node.args on the const path. */
    long offset = node->u.arrayaccess.offset;
    if (is_param) {
        /* :283-284 ic_pload(node.type_, node.t, node.entry.offset-offset). */
        char ofs[24];
        snprintf(ofs, sizeof(ofs), "%ld", eoff - offset);
        tr_ic_pload(tr, node->type_, node->t, ofs);
    } else if (is_local) {
        /* :285-289:
         *   ic_pload(PTR_TYPE, t1, -entry.offset + PTR_TYPE.size)
         *   ic_add(PTR_TYPE, t2, t1, offset)
         *   ic_load(node.type_, t3, f"*${t2}") */
        const char *t1 = compiler_new_temp(tr->cs);
        const char *t2 = compiler_new_temp(tr->cs);
        const char *t3 = compiler_new_temp(tr->cs);
        char p1[24], a3[24], l2[40];
        snprintf(p1, sizeof(p1), "%ld", -eoff + ptr_size);
        tr_ic_pload(tr, ptr_t, t1, p1);
        snprintf(a3, sizeof(a3), "%ld", offset);
        tr_ic_arith(tr, "add", ptr_t, t2, t1, a3);
        snprintf(l2, sizeof(l2), "*$%s", t2);
        tr_ic_load(tr, node->type_, t3, l2);
    } else {
        visitor_generic(v, node);
    }
    return node;
}

/* visit_LETARRAY (translator.py:329-364): real array-element store.
 * arr = node.children[0] (ARRAYACCESS); the index path / scope store
 * is the array codegen — out of S5.8bc's string scope (no array-elem
 * store fixture is non-gated in the string corpus). Faithful slice:
 * the GATE + (when not gated) generic descent so the RHS / index are
 * still visited exactly as Python's `yield (yield generic_visit)`. */
static AstNode *tr_visit_letarray(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *entry = tr_letarray_entry(node);

    AstNode *acc = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *rhs = node->child_count > 1 ? node->children[1] : NULL;

    /* THE GATE — translator.py:330-331 (visit_LETARRAY) /
     * :405-406 (visit_LETARRAYSUBSTR): O_LEVEL>1 and the target entry
     * not accessed ⇒ emit NOTHING and do NOT descend (so a write-only
     * string/array's RHS STRING is never visited → no spurious label).
     *
     * Faithful entry-accessed source: the C parser builds the
     * `LET s$(i)=...` lvalue as a FUNCCALL-shape ONLY when `s` is an
     * undeclared CLASS_unknown id (parser.c:793 — a DIM'd/LET'd, hence
     * read-or-declared, string is CLASS_var ⇒ the STRSLICE-shape branch
     * parser.c:749, which the opt2_letsubstr_not_used / lvalsubstr_nolet
     * / sys_letsubstr0 fixtures take and which is already byte-SAME).
     * Python's p_substr_assignment (zxbparser.py:1248-1305) `to_var()`s
     * that id and builds a **LETSUBSTR** whose entry is NEVER a
     * FUNCCALL/CALL — so Python's FunctionGraphVisitor
     * (_get_calls_from_children, optimize.py:164) never marks it, and
     * Python's OptimizerVisitor.visit_LETSUBSTR (optimize.py:360-365,
     * O>1 + not accessed → NOP + W150) prunes it.  The C parser's
     * FUNCCALL-shape, however, makes the C FunctionGraph
     * (functiongraph.c:52) mark that lvalue's entry accessed — the
     * SAME C-parser FUNCCALL-overload `tr_visit_call_common`
     * (translator.c:827-846) and `collect_side_effects`
     * (optimizer.c:499-509) already special-case by gating on the
     * callee being a real CLASS_function/sub.  Apply that identical
     * faithful predicate here: when the lvalue is a FUNCCALL-shape on a
     * NON-callable string (the C LETSUBSTR-artifact — by construction
     * write-only & undeclared, never genuinely read), Python's
     * effective output is the visit_LETSUBSTR prune, regardless of the
     * spurious C accessed mark.  This is the `llc` zero-regression fix
     * and is byte-identical to Python; real calls (callee
     * CLASS_function/sub) and the CLASS_var STRSLICE-shape path are
     * untouched. */
    bool funccall_shape_noncallable =
        acc && acc->tag == AST_FUNCCALL &&
        acc->child_count > 0 && acc->children[0] &&
        acc->children[0]->tag == AST_ID &&
        acc->children[0]->u.id.class_ != CLASS_function &&
        acc->children[0]->u.id.class_ != CLASS_sub;
    bool entry_unused = entry &&
        (!entry->u.id.accessed ||
         (funccall_shape_noncallable && entry->type_ &&
          type_is_string(entry->type_)));
    if (tr->cs->opts.optimization_level > 1 && entry_unused)
        return node;

    /* String substring assignment — the C parser's FUNCCALL-shape
     * lvalue on a string VAR (Python p_substr_assignment builds
     * LETSUBSTR; translator.py:366-402). One ARGLIST index ⇒
     * lower==upper==that index (p_substr_assignment len==1). */
    if (entry && entry->type_ && type_is_string(entry->type_) &&
        acc && acc->tag == AST_FUNCCALL) {
        AstNode *arglist = acc->child_count > 1 ? acc->children[1] : NULL;
        AstNode *idx = NULL;
        if (arglist && arglist->child_count > 0) {
            AstNode *a0 = arglist->children[0];
            /* ARGUMENT wraps the index expr (child[0]); a bare expr or
             * STRSLICE node can also appear (parser.c:833-854). */
            if (a0 && a0->tag == AST_ARGUMENT && a0->child_count > 0)
                idx = a0->children[0];
            else
                idx = a0;
        }
        tr_letsubstr_emit(v, node, entry, idx, idx, rhs);
        return node;
    }

    /* String substring assignment — STRSLICE-shape lvalue (the other
     * C parser path for `LET a$(idx) = rhs` when `a$` is already a
     * declared CLASS_var string; parser.c:1066-1098 builds an
     * AST_STRSLICE with children [VAR, ARGUMENT(idx)] or [VAR, lower,
     * upper]).  Python's p_substr_assignment (zxbparser.py:1248-1305)
     * always produces a LETSUBSTR for this shape — including the
     * `string_base` MINUS adjustment that has already been applied to
     * the index in make_strslice / parse_call_or_array (mirroring
     * STRSLICE.make_node, symbols/strslice.py:74-83).  Route to
     * tr_letsubstr_emit so the runtime call lands.  Without this the
     * default visitor_generic descends into the STRSLICE and emits a
     * READ slice (__STRSLICE) — the strbase / strbase2 S1-DIVERGE. */
    if (entry && entry->type_ && type_is_string(entry->type_) &&
        acc && acc->tag == AST_STRSLICE) {
        /* STRSLICE children: [string_id, index...] (read shape) or
         * [string_id, lower, upper] (TO-shape). For a write target
         * (LET a$(idx)=rhs), p_substr_assignment with a single index
         * produces lower==upper==idx (zxbparser.py:1278-1281). The C
         * STRSLICE for `a$(idx)` (no TO) has a single index child;
         * use it for both lower and upper. */
        AstNode *lower = NULL, *upper = NULL;
        if (acc->child_count >= 3) {
            /* TO-shape: [str, lower, upper] */
            lower = acc->children[1];
            upper = acc->children[2];
        } else if (acc->child_count == 2) {
            /* Single-index shape: [str, idx-or-ARGUMENT(idx)] */
            AstNode *c1 = acc->children[1];
            if (c1 && c1->tag == AST_ARGUMENT && c1->child_count > 0)
                lower = upper = c1->children[0];
            else
                lower = upper = c1;
        }
        tr_letsubstr_emit(v, node, entry, lower, upper, rhs);
        return node;
    }

    /* Non-string array element — visit_LETARRAY (translator.py:329-364).
     * arr = node.children[0] (the ARRAYACCESS lvalue), scope = arr.scope.
     *
     * SCOPED SUBSET (faithful, regression-free): only the CONSTANT-offset
     * GLOBAL store branch (translator.py:347-351) is ported:
     *   name = arr.entry.data_label
     *   if scope == global_:
     *       yield node.children[1]                 # RHS
     *       ic_store(arr.type_, "%s + %i" % (name, arr.offset), rhs.t)
     * Note Python does NOT yield `arr` here (no index/aaddr codegen for
     * the const path) — only the RHS is visited, then a direct store to
     * "_x.__DATA__ + N". The dynamic / parameter / local store paths
     * (ic_astore / ic_pastore / runtime) are not yet ported; for those
     * the pre-existing default (visitor_generic) is preserved so nothing
     * regresses. */
    if (acc && acc->tag == AST_ARRAYACCESS) {
        AstNode *aent = acc->child_count > 0 ? acc->children[0] : NULL;
        bool a_global = aent && aent->tag == AST_ID &&
                        aent->u.id.scope == SCOPE_global;
        bool a_param  = aent && aent->tag == AST_ID &&
                        aent->u.id.scope == SCOPE_parameter;
        bool a_local  = aent && aent->tag == AST_ID &&
                        aent->u.id.scope == SCOPE_local;
        long a_eoff = aent ? aent->u.id.offset : 0;
        const char *rt;

        /* Python SymbolARRAYACCESS.offset (arrayaccess.py:77-78) returns
         * None UNCONDITIONALLY when scope == SCOPE.parameter — a param
         * array's element offset is never statically foldable (its bound
         * table lives at runtime), so `arr.offset is None` and
         * visit_LETARRAY takes the dynamic ic_pastore(f"*{entry.offset}")
         * path even for a constant subscript. The C `is_const` flag only
         * tracks subscript constness, so OR in the param-scope override
         * to match Python's `offset is None`. */
        bool offset_is_none = !acc->u.arrayaccess.is_const || a_param;
        if (offset_is_none) {
            /* :334-342 dynamic store:
             *   yield children[1]   (RHS)
             *   yield arr           (ARRAYACCESS — push indices)
             *   global: ic_astore(type_, entry.mangled, rhs.t)
             *   param : ic_pastore(type_, f"*{entry.offset}", rhs.t)
             *   local : ic_pastore(type_, -entry.offset, rhs.t)
             * rhs.t is read AFTER visiting (visit_NUMBER sets it to the
             * literal string, symbol_.t lazily mints a temp, etc.). */
            if (rhs) visitor_visit(v, rhs);
            rt = (rhs && rhs->t) ? rhs->t : "";
            visitor_visit(v, acc);                 /* yield arr */
            if (a_global) {
                tr_ic_astore(tr, acc->type_,
                             aent->u.id.mangled ? aent->u.id.mangled : "",
                             rt);
            } else if (a_param) {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "*%ld", a_eoff);
                tr_ic_pastore(tr, acc->type_, ofs, rt);
            } else if (a_local) {
                char ofs[24];
                snprintf(ofs, sizeof(ofs), "%ld", -a_eoff);
                tr_ic_pastore(tr, acc->type_, ofs, rt);
            } else {
                visitor_generic(v, node);
            }
            return node;
        }

        /* Constant offset (:343-362). name = arr.entry.data_label. */
        long aoff = acc->u.arrayaccess.offset;
        if (a_global) {
            const char *name = tr_array_data_label(tr, aent);
            size_t need = strlen(name) + 32;
            char *addr = arena_alloc(&tr->cs->arena, need);
            snprintf(addr, need, "%s + %ld", name, aoff);
            if (rhs) visitor_visit(v, rhs);          /* yield RHS */
            rt = (rhs && rhs->t) ? rhs->t : "";
            tr_ic_store(tr, acc->type_, addr, rt);
            return node;
        }
        if (a_local) {
            /* :353-362:
             *   ic_pload(PTR, t1, -(entry.offset - PTR.size))
             *   ic_add(PTR, t2, t1, arr.offset)
             *   yield children[1]   (RHS)
             *   string -> ic_store(type_, f"*{t2}", rhs.t)
             *   else   -> ic_store(type_, t2, rhs.t) */
            const TypeInfo *ptr_t =
                tr->cs->symbol_table->basic_types[TYPE_uinteger];
            int ptr_size = 2;
            const char *t1 = compiler_new_temp(tr->cs);
            const char *t2 = compiler_new_temp(tr->cs);
            char p1[24], a3[24];
            snprintf(p1, sizeof(p1), "%ld", -(a_eoff - ptr_size));
            tr_ic_pload(tr, ptr_t, t1, p1);
            snprintf(a3, sizeof(a3), "%ld", aoff);
            tr_ic_arith(tr, "add", ptr_t, t2, t1, a3);
            if (rhs) visitor_visit(v, rhs);          /* yield RHS */
            rt = (rhs && rhs->t) ? rhs->t : "";
            if (acc->type_ && type_is_string(acc->type_)) {
                char st[40];
                snprintf(st, sizeof(st), "*$%s", t2);
                tr_ic_store(tr, acc->type_, st, rt);
            } else {
                tr_ic_store(tr, acc->type_, t2, rt);
            }
            return node;
        }
        /* scope == parameter with const offset: Python raises
         * InternalError (translator.py:363-364) — never reached for a
         * valid program; fall to generic to avoid wrong code. */
    }

    /* Out-of-subset — faithful `yield (yield generic_visit(node))`. */
    visitor_generic(v, node);
    return node;
}

/* ArrayRef.count (arrayref.py:40-42): product of every bound's count. */
static long tr_arrayref_count(AstNode *id_) {
    AstNode *bl = id_ ? id_->u.id.arr_boundlist : NULL;
    if (!bl || bl->child_count == 0) return 0;
    long c = 1;
    for (int i = 0; i < bl->child_count; i++) {
        AstNode *bd = bl->children[i];
        long lo = tr_bound_val(bd && bd->child_count > 0 ? bd->children[0]
                                                          : NULL);
        long hi = tr_bound_val(bd && bd->child_count > 1 ? bd->children[1]
                                                          : NULL);
        c *= (hi - lo + 1);
    }
    return c;
}

/* _emit_arraycopy_child (translator.py:294-305): returns the t for one
 * side of a (non-string) array copy. global => "#<data_label>"; param /
 * local => ic_pload(PTR, t, offset-adjusted) and returns that temp. */
static const char *tr_emit_arraycopy_child(Translator *tr, AstNode *id_) {
    int ptr_size = 2;                          /* TYPE.size(PTR_TYPE) */
    if (id_ && id_->u.id.scope == SCOPE_global) {
        const char *dl = tr_array_data_label(tr, id_);
        size_t n = strlen(dl) + 2;
        char *s = arena_alloc(&tr->cs->arena, n);
        s[0] = '#';
        memcpy(s + 1, dl, strlen(dl) + 1);
        return s;
    }
    const char *t = compiler_new_temp(tr->cs);
    const TypeInfo *ptr_t =
        tr->cs->symbol_table->basic_types[TYPE_uinteger];
    long off = id_ ? id_->u.id.offset : 0;
    char ofs[24];
    if (id_ && id_->u.id.scope == SCOPE_parameter)
        snprintf(ofs, sizeof(ofs), "%ld", off - ptr_size);
    else                                       /* local */
        snprintf(ofs, sizeof(ofs), "%ld", -(off - ptr_size));
    tr_ic_pload(tr, ptr_t, t, ofs);
    return t;
}

/* visit_ARRAYCOPY (translator.py:307-327). C "ARRAYCOPY" SENTENCE:
 * children[0] = dest array ID, children[1] = source array ID
 * (parser.c p_array_copy → make_sentence("ARRAYCOPY", larray, rarray)).
 * Only the non-string (block-copy) arm is in scope here:
 *   t1 = _emit_arraycopy_child(t_dest)
 *   t2 = _emit_arraycopy_child(t_source)
 *   ic_load(BOUND_TYPE, t, str(t_source.size))   # size = count*elem
 *   ic_memcopy(t1, t2, t)
 * The string arm (STR_ARRAYCOPY runtime) is left to generic (no string
 * arraycopy fixture is in the owned cluster). */
static AstNode *tr_visit_arraycopy(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    AstNode *t_dest   = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *t_source = node->child_count > 1 ? node->children[1] : NULL;
    if (!t_dest || !t_source) { visitor_generic(v, node); return node; }

    bool src_string = t_source->type_ && type_is_string(t_source->type_);
    if (!src_string) {
        const char *t1 = tr_emit_arraycopy_child(tr, t_dest);
        const char *t2 = tr_emit_arraycopy_child(tr, t_source);
        /* t_source.size = count * elem_size (non-parameter). */
        long cnt = tr_arrayref_count(t_source);
        long esz = t_source->type_ ? type_size(t_source->type_) : 1;
        long sz  = (t_source->u.id.scope == SCOPE_parameter)
                       ? 2 /* TYPE.size(PTR_TYPE) */ : cnt * esz;
        const TypeInfo *bound_t =
            tr->cs->symbol_table->basic_types[TYPE_uinteger];
        const char *t = compiler_new_temp(tr->cs);
        char ssz[24];
        snprintf(ssz, sizeof(ssz), "%ld", sz);
        tr_ic_load(tr, bound_t, t, ssz);
        tr_ic_memcopy(tr, t1, t2, t);
        return node;
    }
    visitor_generic(v, node);
    return node;
}

/* visit_STRSLICE (translator.py:451-468): string read-slice s$(a TO b).
 * C AST_STRSLICE: children[0]=string, children[1]=lower, children[2]=
 * upper (parser.c:861-866 / :773-780). gl.PTR_TYPE == uinteger. */
static AstNode *tr_visit_strslice(Visitor *v, AstNode *node) {
    Translator *tr = v->ctx;
    const TypeInfo *ptr = tr->cs->symbol_table->basic_types[TYPE_uinteger];
    const TypeInfo *ubyte = tr->cs->symbol_table->basic_types[TYPE_ubyte];
    AstNode *str_  = node->child_count > 0 ? node->children[0] : NULL;
    AstNode *lower = node->child_count > 1 ? node->children[1] : NULL;
    AstNode *upper = node->child_count > 2 ? node->children[2] : NULL;
    /* Single-index shape `a$(idx)` — the C parser
     * (parse_call_or_array, parser.c:1090-1158) builds a 2-child
     * STRSLICE [VAR, ARGUMENT(idx)] (or [VAR, idx_expr] post-string_
     * base unwrap).  Python's STRSLICE always has lower AND upper as
     * direct expressions, with lower==upper for a single-index call
     * (make_strslice callers at zxbparser.py:411, :2549, :2564 etc.).
     * Mirror that here: treat the lone index as both lower and upper;
     * unwrap ARGUMENT (its visitor would emit an extra param, doubling
     * the count vs Python).  This is the (pre-existing-bug) reason
     * strbase2's `LET b$ = a$(2)` diverged after the LETSUBSTR routing
     * fix exposed the READ path. */
    if (lower && upper == NULL) {
        if (lower->tag == AST_ARGUMENT && lower->child_count > 0)
            lower = lower->children[0];
        upper = lower;
    } else if (lower && lower->tag == AST_ARGUMENT &&
               lower->child_count > 0) {
        lower = lower->children[0];
    }
    if (upper && upper != lower && upper->tag == AST_ARGUMENT &&
        upper->child_count > 0) {
        upper = upper->children[0];
    }

    /* :452 yield node.string */
    if (str_) visitor_visit(v, str_);

    /* :453-454 if string.token=="STRING" or (VAR and scope==global):
     *   ic_param(PTR_TYPE, string.t) */
    bool s_is_string = str_ && str_->tag == AST_STRING;
    bool s_is_var = str_ && str_->tag == AST_ID;
    bool s_global = s_is_var && str_->u.id.scope == SCOPE_global;
    if (s_is_string || s_global)
        tr_ic_param(tr, ptr, (str_ && str_->t) ? str_->t : "");

    /* :457-461 yield lower; ic_param(lower.type_, lower.t);
     *           yield upper; ic_param(upper.type_, upper.t) */
    if (lower) visitor_visit(v, lower);
    tr_ic_param(tr, lower ? lower->type_ : ptr,
                (lower && lower->t) ? lower->t : "0");
    if (upper) visitor_visit(v, upper);
    tr_ic_param(tr, upper ? upper->type_ : ptr,
                (upper && upper->t) ? upper->t : "0");

    /* :463-466 fparam(ubyte, 0) if (VAR and mangled[0]=="_") or STRING;
     *           else fparam(ubyte, 1) (non-variable ⇒ must be freed). */
    bool free0 = s_is_string ||
                 (s_is_var && str_->u.id.mangled &&
                  str_->u.id.mangled[0] == '_');
    tr_ic_fparam(tr, ubyte, free0 ? "0" : "1");

    /* :468 runtime_call(STRSLICE, TYPE(PTR_TYPE).size) */
    tr_runtime_call(tr, ".core.__STRSLICE", type_size(ptr));
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

/* ic_data (translator_inst_visitor.py:94-95):
 *   self.emit("data", self.TSUFFIX(type_), data)
 * data is a Python list -> Quad str()-coerces to the single-quoted list
 * repr; tr_py_list_repr builds the byte-identical repr. The backend
 * _data (backend.c:2899-2951, Q3 — complete & faithful) consumes
 * args[0]=tsuffix, args[1]=list-repr. */
static void tr_ic_data(Translator *tr, const TypeInfo *type_,
                       char **data, int n) {
    const char *args[2] = { tr_tsuffix(type_),
                            tr_py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "data", 2, args);
}

/* DATA_TYPES (translator_visitor.py:56):
 *   {"str":1,"i8":2,"u8":3,"i16":4,"u16":5,"i32":6,"u32":7,"f16":8,"f":9}
 * Keyed by TSUFFIX(type_). */
static int tr_data_type_code(const TypeInfo *type_) {
    const char *s = tr_tsuffix(type_);
    if (strcmp(s, "str") == 0) return 1;
    if (strcmp(s, "i8")  == 0) return 2;
    if (strcmp(s, "u8")  == 0) return 3;
    if (strcmp(s, "i16") == 0) return 4;
    if (strcmp(s, "u16") == 0) return 5;
    if (strcmp(s, "i32") == 0) return 6;
    if (strcmp(s, "u32") == 0) return 7;
    if (strcmp(s, "f16") == 0) return 8;
    if (strcmp(s, "f")   == 0) return 9;
    /* Python would KeyError — a DATA item of an unmappable type is a
     * real bug; surface it rather than emit a wrong byte. */
    fprintf(stderr, "zxbc: DATA_TYPES has no entry for '%s'\n", s);
    return 0;
}

/* emit_data_blocks (translator_visitor.py:125-153). MUST run before
 * emit_strings (the :141 add_string_label registers a CONST-string-DATA
 * label into STRING_LABELS that the subsequent emit_strings drain emits;
 * for the corpus FUNCPTR-thunk path the thunk body's visit_STRING does
 * the registering during FunctionTranslator.start). The byte emitters
 * (_data/_vard/_label) are S5.6/Q3-shipped & dispatch-wired. */
void translator_emit_data_blocks(Translator *tr) {
    CompilerState *cs = tr->cs;
    /* :127-128  if not gl.DATA_IS_USED or not gl.DATAS: return */
    if (!cs->data_is_used || cs->datas.len == 0)
        return;

    const TypeInfo *t_byte = cs->symbol_table->basic_types[TYPE_byte];
    const TypeInfo *t_ptr  = cs->symbol_table->basic_types[TYPE_uinteger];
    const TypeInfo *t_uint = cs->symbol_table->basic_types[TYPE_uinteger];
    const TypeInfo *t_str  = cs->symbol_table->basic_types[TYPE_string];
    const TypeInfo *t_fix  = cs->symbol_table->basic_types[TYPE_fixed];

    /* :130  for label_, datas in gl.DATAS: */
    for (int di = 0; di < cs->datas.len; di++) {
        DataRef *dr = cs->datas.data[di];
        /* :131  self.ic_label(label_) */
        tr_ic_label(tr, dr->label_name);

        for (int ii = 0; ii < dr->item_count; ii++) {
            DataItem *it = &dr->items[ii];

            /* :133-137  isinstance(d, symbols.FUNCDECL): the non-static
             * thunk. type byte = DATA_TYPES[TSUFFIX(d.type_)] | 0x80
             * formatted "%02Xh"; then a PTR_TYPE (u16) ptr to d.mangled
             * (the thunk's mangled, e.g. "___DATA__FUNCPTR__0"). */
            if (it->is_funcdecl) {
                AstNode *decl = it->node;
                AstNode *fent = decl->child_count > 0
                                    ? decl->children[0] : NULL;
                char hb[8];
                snprintf(hb, sizeof(hb), "%02Xh",
                         (tr_data_type_code(decl->type_) | 0x80) & 0xFF);
                char *one[1]; one[0] = arena_strdup(&cs->arena, hb);
                tr_ic_data(tr, t_byte, one, 1);

                const char *mg = (fent && fent->u.id.mangled)
                                     ? fent->u.id.mangled : "";
                char *m1[1]; m1[0] = arena_strdup(&cs->arena, mg);
                tr_ic_data(tr, t_ptr, m1, 1);
                continue;
            }

            /* :139  ic_data(TYPE.byte, [DATA_TYPES[TSUFFIX(d.value.type_)]]) */
            AstNode *val = it->node;
            char cb[8];
            snprintf(cb, sizeof(cb), "%d", tr_data_type_code(val->type_));
            char *tb[1]; tb[0] = arena_strdup(&cs->arena, cb);
            tr_ic_data(tr, t_byte, tb, 1);

            const TypeInfo *vf = val->type_ && val->type_->final_type
                                     ? val->type_->final_type : val->type_;
            BasicType vbt = vf ? vf->basic_type : TYPE_unknown;

            if (vbt == TYPE_string) {
                /* :140-142  lbl = add_string_label(d.value.value);
                 *           ic_data(gl.PTR_TYPE, [lbl])
                 * (CONST-string-in-DATA only — bare literals are never
                 * static so route via the FUNCPTR thunk; corpus-dead but
                 * faithful.) */
                const char *sv = (val->tag == AST_STRING
                                  && val->u.string.value)
                                     ? val->u.string.value : "";
                int sl = (val->tag == AST_STRING)
                             ? val->u.string.length : (int)strlen(sv);
                char *lbl = backend_add_string_label(tr->backend, sv, sl);
                char *l1[1]; l1[0] = lbl;
                tr_ic_data(tr, t_ptr, l1, 1);
            } else if (vbt == TYPE_fixed) {
                /* :143-145  bytes_ = 0xFFFFFFFF & int(value*2**16);
                 *  ic_data(TYPE.uinteger,
                 *          ["0x%04X"%(b&0xFFFF), "0x%04X"%(b>>16)]) */
                double dv = (val->tag == AST_NUMBER)
                                ? val->u.number.value : 0.0;
                unsigned long bytes_ =
                    0xFFFFFFFFUL & (unsigned long)(long)(dv * 65536.0);
                char lo[10], hi[10];
                snprintf(lo, sizeof(lo), "0x%04lX", bytes_ & 0xFFFF);
                snprintf(hi, sizeof(hi), "0x%04lX",
                         (bytes_ >> 16) & 0xFFFF);
                char *fw[2];
                fw[0] = arena_strdup(&cs->arena, lo);
                fw[1] = arena_strdup(&cs->arena, hi);
                tr_ic_data(tr, t_uint, fw, 2);
            } else {
                /* :146-147  ic_data(d.value.type_,
                 *                   [self.traverse_const(d.value)]) */
                char *cv[1];
                cv[0] = (char *)tr_traverse_const(tr, val);
                tr_ic_data(tr, val->type_, cv, 1);
            }
            (void)t_str; (void)t_fix;
        }
    }

    /* :149-151  missing_data_labels = set(DATA_LABELS_REQUIRED)
     *   .difference([x.label.name for x in gl.DATAS]);
     *   for data_label in missing_data_labels: ic_label(data_label)
     * (a label a RESTORE asked for beyond the last DATA line). Python's
     * `set` iteration order is hash-based; the corpus only ever has
     * 0/1 such label so the order is not byte-observable, but iterate
     * the HashMap's slot order (a stable per-process order) for
     * determinism. */
    {
        HashMap *req = &cs->data_labels_required;
        for (int hi2 = 0; hi2 < req->capacity; hi2++) {
            HashEntry *e = &req->entries[hi2];
            if (!e->occupied || !e->key) continue;
            bool present = false;
            for (int di = 0; di < cs->datas.len; di++) {
                if (strcmp(cs->datas.data[di]->label_name, e->key) == 0) {
                    present = true; break;
                }
            }
            if (!present)
                tr_ic_label(tr, e->key);
        }
    }

    /* :153  self.ic_vard("__DATA__END", ["00"]) */
    {
        char *z[1]; z[0] = (char *)"00";
        tr_ic_vard(tr, "__DATA__END", z, 1);
    }
}

/* emit_strings (translator_visitor.py:155-158):
 *   for str_, label_ in string_labels.STRING_LABELS.items():
 *     l = "%04X" % (len(str_) & 0xFFFF)
 *     self.ic_vard(label_, [l] + ["%02X" % ord(x) for x in str_])
 * Iterates the Backend STRING_LABELS store in INSERTION order. ic_vard
 * (translator_inst_visitor.py:244-245) -> emit("vard", name, data) with
 * data a Python list -> Quad str()-coerces to a single-quoted list repr;
 * tr_ic_vard / tr_py_list_repr build that byte-identical repr (same as
 * emit_jump_tables). NUL-safe: iterates the stored byte length. */
void translator_emit_strings(Translator *tr) {
    StringLabels *sl = &tr->backend->string_labels;
    for (int i = 0; i < sl->count; i++) {
        const char *bytes = sl->items[i].bytes;
        int blen = sl->items[i].len;
        int n = blen + 1; /* the "%04X" length word + one item per byte */
        char **data = arena_alloc(&tr->cs->arena,
                                  (size_t)n * sizeof(char *));
        char hdr[8];
        snprintf(hdr, sizeof(hdr), "%04X", blen & 0xFFFF);
        data[0] = arena_strdup(&tr->cs->arena, hdr);
        for (int k = 0; k < blen; k++) {
            char hb[4];
            snprintf(hb, sizeof(hb), "%02X",
                     (unsigned)(unsigned char)bytes[k]);
            data[k + 1] = arena_strdup(&tr->cs->arena, hb);
        }
        tr_ic_vard(tr, sl->items[i].label, data, n);
    }
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
    visitor_on_tag(v, AST_STRING, tr_visit_string);
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
    /* visit_BUILTIN (translator.py:150-158). */
    visitor_on_tag(v, AST_BUILTIN, tr_visit_builtin);
    /* visit_ASM (translator.py:963-967). */
    visitor_on_tag(v, AST_ASM, tr_visit_asm);
    /* visit_UNARY (translator.py:139-148). */
    visitor_on_tag(v, AST_UNARY, tr_visit_unary);
    visitor_on_tag(v, AST_CONSTEXPR, tr_visit_constexpr);
    visitor_on_tag(v, AST_ARGLIST, tr_visit_arglist);
    visitor_on_tag(v, AST_ARGUMENT, tr_visit_argument);
    visitor_on_sentence(v, "CALL", tr_visit_call);
    visitor_on_sentence(v, "LET", tr_visit_let);
    /* S5.8bc — string/array-element assignment + string-slice
     * (translator.py:329-468). The C parser builds one "LETARRAY"
     * SENTENCE for both array-elem and string-substring assignments
     * (parser.c:1644); tr_visit_letarray carries the O>1-not-accessed
     * GATE (the llc fix) and routes the string case to LETSUBSTR
     * semantics. STRSLICE is a tag node. ACTIVE. */
    visitor_on_sentence(v, "LETARRAY", tr_visit_letarray);
    visitor_on_sentence(v, "ARRAYCOPY", tr_visit_arraycopy);
    /* visit_ARRAYLOAD / visit_ARRAYACCESS (translator.py:265-289,448-449).
     * The C parser builds AST_ARRAYACCESS for every array access; this
     * handler ports the constant-offset GLOBAL value-read branch and
     * preserves the prior default for the out-of-subset (dynamic/param/
     * local) paths. The const-offset GLOBAL store is in tr_visit_letarray. */
    visitor_on_tag(v, AST_ARRAYACCESS, tr_visit_arrayaccess);
    visitor_on_tag(v, AST_STRSLICE, tr_visit_strslice);
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
    /* Simple statements (translator.py:50-54,101-104,886-889,904-907). */
    visitor_on_sentence(v, "NOP", tr_visit_nop);
    visitor_on_sentence(v, "CLS", tr_visit_cls);
    visitor_on_sentence(v, "BORDER", tr_visit_border);
    visitor_on_sentence(v, "PAUSE", tr_visit_pause);
    visitor_on_sentence(v, "RANDOMIZE", tr_visit_randomize);
    /* visit_ATTR_sentence (translator.py:921-931) — 8 standalone attrs. */
    visitor_on_sentence(v, "INK", tr_visit_attr_sentence);
    visitor_on_sentence(v, "PAPER", tr_visit_attr_sentence);
    visitor_on_sentence(v, "FLASH", tr_visit_attr_sentence);
    visitor_on_sentence(v, "BRIGHT", tr_visit_attr_sentence);
    visitor_on_sentence(v, "INVERSE", tr_visit_attr_sentence);
    visitor_on_sentence(v, "OVER", tr_visit_attr_sentence);
    visitor_on_sentence(v, "BOLD", tr_visit_attr_sentence);
    visitor_on_sentence(v, "ITALIC", tr_visit_attr_sentence);
    /* visit_BEEP (translator.py:891-902). */
    visitor_on_sentence(v, "BEEP", tr_visit_beep);
    /* Drawing primitives (translator.py:754-800). */
    visitor_on_sentence(v, "PLOT", tr_visit_plot);
    visitor_on_sentence(v, "DRAW", tr_visit_draw);
    visitor_on_sentence(v, "DRAW3", tr_visit_draw3);
    visitor_on_sentence(v, "CIRCLE", tr_visit_circle);
    /* visit_OUT (translator.py:808-811). */
    visitor_on_sentence(v, "OUT", tr_visit_out);
    /* visit_LOAD/visit_SAVE/visit_VERIFY (translator.py:862-880).
     * VERIFY routes to the LOAD handler (visit_VERIFY:880). */
    visitor_on_sentence(v, "LOAD", tr_visit_load);
    visitor_on_sentence(v, "VERIFY", tr_visit_load);
    visitor_on_sentence(v, "SAVE", tr_visit_save);
    /* visit_POKE (translator.py:90-99). */
    visitor_on_sentence(v, "POKE", tr_visit_poke);
    visitor_on_sentence(v, "EXIT_DO", tr_visit_exit_do);
    visitor_on_sentence(v, "EXIT_WHILE", tr_visit_exit_while);
    visitor_on_sentence(v, "EXIT_FOR", tr_visit_exit_for);
    visitor_on_sentence(v, "CONTINUE_DO", tr_visit_continue_do);
    visitor_on_sentence(v, "CONTINUE_WHILE", tr_visit_continue_while);
    visitor_on_sentence(v, "CONTINUE_FOR", tr_visit_continue_for);
    /* S5.8d — DATA/READ/RESTORE. "DATA" itself has NO translator handler
     * (Python has no visit_DATA — DATA is consumed entirely at parse +
     * emit_data_blocks). READ/RESTORE are active sentence handlers. */
    visitor_on_sentence(v, "READ", tr_visit_read);
    visitor_on_sentence(v, "RESTORE", tr_visit_restore);
    /* S7.1b-i — core PRINT (string + numeric + ';'/',' + trailing EOL).
     * PRINT_AT/PRINT_TAB/PRINT_COMMA/_TMP child visitors are S7.1b-ii/iii;
     * visit_PRINT itself (the loop, COPY_ATTR, type-keyed PRINT*,
     * PRINT_EOL) lands here. */
    visitor_on_sentence(v, "PRINT", tr_visit_print);
    /* visit_PRINT_COMMA (translator.py:860-861) — the ',' separator's
     * comma-tab call. Part of S7.1b-i (the ',' support). */
    visitor_on_sentence(v, "PRINT_COMMA", tr_visit_print_comma);
    /* visit_PRINT_AT (translator.py:848-853) / visit_PRINT_TAB
     * (translator.py:855-858) — the AT/TAB position-control children.
     * S7.1b-ii. */
    visitor_on_sentence(v, "PRINT_AT", tr_visit_print_at);
    visitor_on_sentence(v, "PRINT_TAB", tr_visit_print_tab);
    /* visit_ATTR_TMP (translator_visitor.py:103-118 + the _visit
     * ATTR_TMP routing :164-170) — the 8 in-PRINT temporary attrs.
     * S7.1b-iii. ONE handler, registered for all 8 "<NAME>_TMP" kinds. */
    visitor_on_sentence(v, "INK_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "PAPER_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "BRIGHT_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "FLASH_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "OVER_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "INVERSE_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "BOLD_TMP", tr_visit_attr_tmp);
    visitor_on_sentence(v, "ITALIC_TMP", tr_visit_attr_tmp);
}

void translator_visit(Translator *tr, AstNode *ast) {
    /* reset() analogue (translator_visitor.py:53-61 + common.init): the
     * C Translator is stack-scoped per compile; clear its class-state
     * mirror here. tmp_labels/LABEL_COUNTER live on the Backend and are
     * reset by backend_init (common.init) — not re-zeroed here.
     * TranslatorVisitor.reset() (:62) also string_labels.reset() — the
     * STRING_LABELS dedup store lives on the Backend; clear it here. */
    backend_string_labels_reset(tr->backend);
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

/* TranslatorVisitor.traverse_const (translator_visitor.py:177-245) —
 * the FULL constant-expression stringifier, ported branch-for-branch.
 * Previously a NUMBER/CONST-leaf stub; @label / @label+N (p_addr_of_id
 * CONSTEXPR-wrapped, parser.c) need the UNARY-ADDRESS / LABEL / BINARY
 * arms so the CONSTEXPR folds to the static label string instead of
 * the empty-operand runtime `ld hl,`. Mirrors the proven
 * var_translator.c vt_traverse_const_expr, plus Python's LABEL/FUNCTION
 * + has_address-ID arms (:233-245). */
static const char *tr_traverse_const(Translator *tr, AstNode *node) {
    if (!node) return "";

    /* NUMBER -> node.t (str(value)); :179-180 */
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

    /* CONSTEXPR -> traverse_const(node.expr); :239-240 */
    if (node->tag == AST_CONSTEXPR)
        return node->child_count > 0
                   ? tr_traverse_const(tr, node->children[0]) : "";

    if (node->tag == AST_UNARY) {
        const char *mid = node->u.unary.operator;
        AstNode *operand = node->child_count > 0 ? node->children[0]
                                                 : NULL;
        /* MINUS -> " -" + traverse_const(operand); :186-187 */
        if (mid && strcmp(mid, "MINUS") == 0) {
            const char *r = tr_traverse_const(tr, operand);
            size_t rl = strlen(r);
            char *out = arena_alloc(&tr->cs->arena, rl + 3);
            out[0] = ' '; out[1] = '-';
            memcpy(out + 2, r, rl + 1);
            return out;
        }
        /* ADDRESS -> traverse_const(operand) iff operand is global or a
         * LABEL/FUNCTION (:188-193). The C @-operand is the shared
         * symbol-table entry (CLASS_label once `<name>:` ran to_label,
         * or a global scalar). */
        if (mid && strcmp(mid, "ADDRESS") == 0)
            return tr_traverse_const(tr, operand);
        fprintf(stderr, "zxbc: traverse_const invalid unary op '%s'\n",
                mid ? mid : "");
        return "";
    }

    if (node->tag == AST_BINARY) {
        const char *mid = NULL;
        const char *op = node->u.binary.operator;
        if (op) {
            if      (strcmp(op, "PLUS")  == 0) mid = "+";
            else if (strcmp(op, "MINUS") == 0) mid = "-";
            else if (strcmp(op, "MUL")   == 0 ||
                     strcmp(op, "MULT")  == 0) mid = "*";
            else if (strcmp(op, "DIV")   == 0) mid = "/";
            else if (strcmp(op, "MOD")   == 0) mid = "%";
            else if (strcmp(op, "POW")   == 0) mid = "^";
            else if (strcmp(op, "SHL")   == 0) mid = ">>";
            else if (strcmp(op, "SHR")   == 0) mid = "<<";
        }
        if (mid == NULL) {
            fprintf(stderr,
                    "zxbc: traverse_const invalid binary op '%s'\n",
                    op ? op : "");
            return "";
        }
        AstNode *l = node->child_count > 0 ? node->children[0] : NULL;
        AstNode *r = node->child_count > 1 ? node->children[1] : NULL;
        const char *ls = tr_traverse_const(tr, l);
        const char *rs = tr_traverse_const(tr, r);
        size_t need = strlen(ls) + strlen(rs) + strlen(mid) + 8;
        char *out = arena_alloc(&tr->cs->arena, need);
        snprintf(out, need, "(%s) %s (%s)", ls, mid, rs);
        return out;
    }

    if (node->tag == AST_TYPECAST) {
        /* :222-232 width masks. Operand is children[0]. */
        AstNode *operand = node->child_count > 0 ? node->children[0]
                                                 : NULL;
        const char *inner = tr_traverse_const(tr, operand);
        const TypeInfo *ft = node->type_ && node->type_->final_type
                                 ? node->type_->final_type : node->type_;
        BasicType bt = ft ? ft->basic_type : TYPE_unknown;
        char *out;
        size_t il = strlen(inner);
        if (bt == TYPE_byte || bt == TYPE_ubyte) {
            out = arena_alloc(&tr->cs->arena, il + 12);
            snprintf(out, il + 12, "(%s) & 0xFF", inner);
        } else if (bt == TYPE_integer || bt == TYPE_uinteger) {
            out = arena_alloc(&tr->cs->arena, il + 14);
            snprintf(out, il + 14, "(%s) & 0xFFFF", inner);
        } else if (bt == TYPE_long || bt == TYPE_ulong) {
            out = arena_alloc(&tr->cs->arena, il + 18);
            snprintf(out, il + 18, "(%s) & 0xFFFFFFFF", inner);
        } else if (bt == TYPE_fixed) {
            out = arena_alloc(&tr->cs->arena, il + 26);
            snprintf(out, il + 26, "((%s) & 0xFFFF) << 16", inner);
        } else {
            return inner;
        }
        return out;
    }

    if (node->tag == AST_ID) {
        /* :233-235 CONST/VAR/LABEL/FUNCTION -> node.t.  A CLASS_label
         * entry's LabelRef.t == parent.mangled == ".LABEL._<name>"
         * (labelref.py:20,34). The C symbol-table label entry carries
         * the plain MANGLE_CHR mangled ("_<name>"); compute the
         * LabelRef-mangled form exactly as visit_GOTO/LABEL do via
         * tr_label_mangled. */
        if (node->u.id.class_ == CLASS_label)
            return tr_label_mangled(tr, node);
        /* CONST id -> its ConstRef.t (the resolved value string). */
        if (node->u.id.class_ == CLASS_const)
            return node->t ? node->t
                 : (node->u.id.mangled ? node->u.id.mangled : "");
        if (node->t != NULL)
            return node->t;
        /* :243-244 ID/VARARRAY with has_address & global -> mangled. */
        if (node->u.id.has_address &&
            node->u.id.scope == SCOPE_global &&
            node->u.id.mangled)
            return node->u.id.mangled;
        if (node->u.id.mangled)
            return node->u.id.mangled;
        return node->u.id.name ? node->u.id.name : "";
    }

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
        /* Python local_symbol_table.values() applies Scope.values(
         * filter_by_opt=True) at O>1 (scope.py:63-66): un-accessed
         * locals are dropped from the prologue init walk
         * (function_translator.py:58). Mirrors the filter in
         * compute_offsets (compiler.c) so the offsets and the per-local
         * init emission stay consistent — without this, a const7-shape
         * unused-local DIM with a static initializer would still emit
         * its lvard image despite locals_size having been folded to 0. */
        if (tr->cs->opts.optimization_level > 1 && !lv->u.id.accessed)
            continue;
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

            /* :79-80 if bound_ptrs != ["0","0"]:
             *     OPTIONS["__DEFINES"]["__ZXB_USE_LOCAL_ARRAY_WITH_BOUNDS__"]=""
             * Persists into the ASM filter_ pass (set_option_defines only
             * ADDS keys; never clears) so arrayalloc.asm:75 #ifdef fires. */
            if (strcmp(bptrs[0], "0") != 0 || strcmp(bptrs[1], "0") != 0)
                tr->cs->local_array_with_bounds_used = true;

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

    /* :122-164 stdcall teardown — free local strings & local arrays.
     * Python gates on `node.convention == CONVENTION.stdcall`. The C
     * parser leaves an un-annotated function's convention CONV_unknown
     * (Python's p_convention default is CONVENTION.stdcall), and every
     * other convention read already treats unknown == stdcall (only
     * CONV_fastcall is special). So gate on !fastcall — faithful to
     * Python's stdcall default. preserve_hl: ic_exchg() emitted at most
     * once before the first free and once after the loop. */
    if (!fastcall) {
        const TypeInfo *ptr_t =
            tr->cs->symbol_table->basic_types[TYPE_uinteger]; /* PTR_TYPE */
        const TypeInfo *str_t =
            tr->cs->symbol_table->basic_types[TYPE_string];
        int ptr_size = 2;
        bool preserve_hl = false;
        for (int li = 0; li < entry->u.id.local_entries_count; li++) {
            AstNode *lv = entry->u.id.local_entries[li];
            if (!lv) continue;
            SymbolClass c = lv->u.id.class_;
            int scope = lv->u.id.scope;
            bool is_str = lv->type_ && type_is_string(lv->type_);
            bool free_scope =
                scope == SCOPE_local ||
                (scope == SCOPE_parameter && !lv->u.id.byref);

            if (is_str) {
                /* :126-149 string locals / string-array locals. */
                if (c == CLASS_const || c == CLASS_function ||
                    c == CLASS_sub)
                    continue;
                if (c != CLASS_array) {
                    if (free_scope) {
                        if (!preserve_hl) {
                            preserve_hl = true;
                            tr_ic_exchg(tr);
                        }
                        long off = (scope == SCOPE_local)
                                       ? -lv->u.id.offset
                                       : lv->u.id.offset;
                        char ofs[24];
                        snprintf(ofs, sizeof(ofs), "%ld", off);
                        /* local_var.t == VarRef.t (varref.py:34-42): a
                         * non-global, *dynamic* (string) scalar is
                         * f"${self._t}" — the leading '$' is what makes
                         * _fploadstr (_pload.py:181-193) skip the LOADSTR
                         * runtime_call (ins[1][0] != '$'). The temp number
                         * itself never reaches output (offset comes from
                         * ins[2]); only the '$' sentinel is load-bearing. */
                        const char *base =
                            (lv->t && lv->t[0]) ? lv->t
                                                : compiler_new_temp(tr->cs);
                        const char *lt;
                        if (base[0] == '$') {
                            lt = base;
                        } else {
                            char *b = arena_alloc(&tr->cs->arena,
                                                  strlen(base) + 2);
                            b[0] = '$';
                            strcpy(b + 1, base);
                            lt = b;
                        }
                        tr_ic_fpload(tr, str_t, lt, ofs);
                        tr_runtime_call(tr, ".core.__MEM_FREE", 0);
                    }
                } else {
                    if (free_scope) {
                        if (!preserve_hl) {
                            preserve_hl = true;
                            tr_ic_exchg(tr);
                        }
                        /* :147-149 ic_param(BOUND_TYPE, count);
                         * _local_array_load; ARRAYSTR_FREE_MEM. */
                        long cnt = 1;
                        AstNode *bl = lv->u.id.arr_boundlist;
                        if (bl) {
                            for (int bi = 0; bi < bl->child_count; bi++) {
                                AstNode *bd = bl->children[bi];
                                long lo = tr_bound_val(
                                    bd && bd->child_count > 0
                                        ? bd->children[0] : NULL);
                                long hi = tr_bound_val(
                                    bd && bd->child_count > 1
                                        ? bd->children[1] : NULL);
                                cnt *= (hi - lo + 1);
                            }
                        }
                        char cs[24];
                        snprintf(cs, sizeof(cs), "%ld", cnt);
                        tr_ic_param(tr, ptr_t, cs);
                        const char *t2 = compiler_new_temp(tr->cs);
                        long la = lv->u.id.offset - ptr_size;
                        char lo2[24];
                        snprintf(lo2, sizeof(lo2), "%ld",
                                 (scope == SCOPE_parameter) ? la : -la);
                        tr_ic_pload(tr, ptr_t, t2, lo2);
                        tr_ic_fparam(tr, ptr_t, t2);
                        tr_runtime_call(tr,
                            ".core.__ARRAYSTR_FREE_MEM", 0);
                    }
                }
            }

            if (c == CLASS_array && !is_str && free_scope) {
                /* :151-161 non-string local/param array -> MEM_FREE. */
                if (!preserve_hl) {
                    preserve_hl = true;
                    tr_ic_exchg(tr);
                }
                /* _local_array_load (function_translator.py:34-41):
                 *   t2 = new_t()
                 *   param: ic_pload(PTR, t2, offset - PTR.size)
                 *   local: ic_pload(PTR, t2, -(offset - PTR.size))
                 *   ic_fparam(PTR, t2) */
                const char *t2 = compiler_new_temp(tr->cs);
                long la = lv->u.id.offset - ptr_size;
                char lo2[24];
                snprintf(lo2, sizeof(lo2), "%ld",
                         (scope == SCOPE_parameter) ? la : -la);
                tr_ic_pload(tr, ptr_t, t2, lo2);
                tr_ic_fparam(tr, ptr_t, t2);
                tr_runtime_call(tr, ".core.__MEM_FREE", 0);
            }
        }
        if (preserve_hl)
            tr_ic_exchg(tr);
    }

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
    /* zxbc.py:128-129  if gl.DATA_IS_USED:
     *                      gl.FUNCTIONS.extend(gl.DATA_FUNCTIONS)
     * Runs AFTER translator.visit (codegen.c) and immediately BEFORE the
     * FunctionTranslator drain, so the __DATA__FUNCPTR__N thunk FUNCDECLs
     * are queued exactly like ordinary pending top-level functions and
     * emitted by the same drain (the S5.7 FunctionTranslator needs no
     * change — a fastcall FUNCDECL whose body is
     * make_block(make_sentence("RETURN", func, value)) lowers to the
     * thunk via tr_visit_function + tr_visit_return). */
    if (tr->cs->data_is_used) {
        for (int i = 0; i < tr->cs->data_functions.len; i++)
            tr_pending_push(tr, tr->cs->data_functions.data[i]);
    }

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
