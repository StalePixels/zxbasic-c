/*
 * VarTranslator + Translator.default_value — the S5.3 data-space slice.
 *
 * Byte-faithful port of:
 *   src/arch/z80/visitor/var_translator.py  (VarTranslator: visit_VARDECL,
 *                                             visit_LABEL — whole file's
 *                                             scalar path; ARRAYDECL is
 *                                             out of the S5.3 scalar slice)
 *   src/arch/z80/visitor/translator.py:1029-1078  Translator.default_value
 *                                             (integer / float / fixed /
 *                                             CONSTEXPR branches)
 *   src/arch/z80/visitor/translator_inst_visitor.py
 *       ic_var   :241-242   ic_vard :244-245   ic_varx :247-248
 *       ic_deflabel :97-98  ic_label :145-146
 *
 * Driven by zxbc.py:196-199 over cs->data_ast (a BLOCK of VARDECL(entry)
 * built in codegen.c from the ordered symbol-table drain, mirroring
 * p_start:543-545 + make_var_declaration:263-267 = sym.VARDECL(entry)).
 *
 * The list args (ic_vard/ic_varx 2nd/3rd operand) are emitted as the
 * Python list-of-str repr string (e.g. "['01']") because Quad.__init__
 * str()-coerces every arg and the backend _vard/_varx do eval(ins[...]).
 * The C backend ports a faithful evaluator for exactly those repr forms.
 */
#include "translator.h"
#include "visitor.h"
#include "ic.h"
#include "errmsg.h"
#include "z80asm.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Python repr of a list[str]: "['a', 'b']" — now the shared
 * tr_py_list_repr (translator.c) so the S5.7d local-init IC wrappers and
 * this S5.6 data-image path produce byte-identical reprs. */
static char *py_list_repr(Translator *tr, char **items, int n) {
    return tr_py_list_repr(tr, items, n);
}

/* ic_var (translator_inst_visitor.py:241-242): emit("var", name, size_) */
static void vt_ic_var(Translator *tr, const char *name, int size_) {
    char sz[16];
    snprintf(sz, sizeof(sz), "%d", size_);
    const char *args[2] = { name, sz };
    tr_emit_quad(tr, "var", 2, args);
}

/* ic_vard (translator_inst_visitor.py:244-245): emit("vard", name, data)
 * — data is a Python list; Quad str()-coerces it to its repr. */
static void vt_ic_vard(Translator *tr, const char *name,
                       char **data, int n) {
    const char *args[2] = { name, py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "vard", 2, args);
}

/* ic_varx (translator_inst_visitor.py:247-248):
 *   emit("varx", name, TSUFFIX(type_), default_value) */
static void vt_ic_varx(Translator *tr, const char *name,
                       const TypeInfo *type_, char **data, int n) {
    const char *args[3] = { name, tr_tsuffix(type_),
                            py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "varx", 3, args);
}

/* ic_deflabel (translator_inst_visitor.py:97-98): emit("deflabel",label,t) */
static void vt_ic_deflabel(Translator *tr, const char *label,
                           const char *t) {
    const char *args[2] = { label, t };
    tr_emit_quad(tr, "deflabel", 2, args);
}

/* ic_label (translator_inst_visitor.py:145-146): emit("label", label) */
static void vt_ic_label(Translator *tr, const char *label) {
    const char *args[1] = { label };
    tr_emit_quad(tr, "label", 1, args);
}

/* Translator.traverse_const, NUMBER/CONST leaf only (translator_visitor.py
 * :177-184) — the S5.3/S5.4/S5.5 scalar behaviour, kept BYTE-IDENTICAL.
 * Scalar callers (DIM AT / DIM = CONSTEXPR) only ever pass NUMBER/CONST
 * leaves in the landed slices; UNARY/BINARY here is out-of-scalar-scope —
 * fail loud past the leaf (unchanged from S5.5). S5.6's array-init path
 * uses vt_traverse_const_expr (below) instead, so this stays scoped to
 * non-array constructs and cannot regress DIM-AT/scalar fixtures. */
static const char *vt_traverse_const(Translator *tr, AstNode *node) {
    if (!node) return "";
    if (node->tag == AST_NUMBER || node->tag == AST_CONSTEXPR) {
        /* node.t — for NUMBER == str(value); set lazily like visit_NUMBER */
        if (node->t == NULL && node->tag == AST_NUMBER) {
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
    if (node->tag == AST_ID) /* CONST entry: traverse_const returns node.t */
        return node->t ? node->t : "";
    fprintf(stderr,
            "zxbc: traverse_const non-leaf (tag=%d) not in scalar scope\n",
            (int)node->tag);
    return node->t ? node->t : "";
}

/* str(value) for a NUMBER (visit_NUMBER / Symbol.t lazy form, ast.c:71-77). */
static const char *vt_number_str(Translator *tr, AstNode *node) {
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

/* operator map (translator_visitor.py:201-211): token -> infix glyph. */
static const char *vt_binary_glyph(const char *op) {
    if (!op) return NULL;
    if (strcmp(op, "PLUS")  == 0) return "+";
    if (strcmp(op, "MINUS") == 0) return "-";
    if (strcmp(op, "MUL")   == 0 || strcmp(op, "MULT") == 0) return "*";
    if (strcmp(op, "DIV")   == 0) return "/";
    if (strcmp(op, "MOD")   == 0) return "%";
    if (strcmp(op, "POW")   == 0) return "^";
    if (strcmp(op, "SHL")   == 0) return ">>";
    if (strcmp(op, "SHR")   == 0) return "<<";
    return NULL;
}

/* TranslatorVisitor.traverse_const (translator_visitor.py:177-221) — the
 * FULL constant-expression stringifier. Ported branch-for-branch:
 *   NUMBER / CONST           -> node.t
 *   UNARY MINUS              -> " -" + traverse_const(operand)
 *   UNARY ADDRESS            -> traverse_const(operand) (operand must be
 *                               global / LABEL / FUNCTION)
 *   BINARY {PLUS,MINUS,MUL,DIV,MOD,POW,SHL,SHR}
 *                            -> "(L) <op> (R)"
 * S5.6-scoped: ONLY the array-init / array-AT path calls this
 * (var_translator.py:79 -> array_default_value -> default_value ->
 * traverse_const; and entry.addr for arrays). Scalar callers keep the
 * leaf-only vt_traverse_const above, so non-array output is unchanged. */
static const char *vt_traverse_const_expr(Translator *tr, AstNode *node) {
    if (!node) return "";

    if (node->tag == AST_NUMBER)
        return vt_number_str(tr, node);

    if (node->tag == AST_CONSTEXPR) {
        if (node->child_count > 0)
            return vt_traverse_const_expr(tr, node->children[0]);
        return node->t ? node->t : "";
    }

    if (node->tag == AST_ID) {
        /* Faithful port of TranslatorVisitor.traverse_const's ID arms
         * (translator_visitor.py:184-185,235-246), identical to the
         * complete tr_traverse_const (translator.c) AST_ID branch:
         *
         *  - CLASS_label -> node.t == LabelRef.t == parent.mangled ==
         *    ".LABEL._<name>" (labelref.py:20,34). The C label entry's
         *    own .mangled is the function-namespaced "_<fn>.<name>"
         *    (compiler.c:1256-1257) — NOT the LabelRef form — so the
         *    LabelRef-mangled string is computed here exactly as
         *    tr_label_mangled does (".LABEL._" + entry.name; global_.py
         *    LABELS_NAMESPACE=".LABEL", MANGLE_CHR="_"). Without this a
         *    local `@label` in a const-vector emitted "_test.label1"
         *    instead of Python's ".LABEL._label1".
         *  - CLASS_const -> ConstRef.t (the resolved value string).
         *  - else (ID/VARARRAY) -> node.t, else (has_address & global)
         *    .mangled (:243-244), else .mangled, else .name. */
        if (node->u.id.class_ == CLASS_label) {
            const char *nm = node->u.id.name ? node->u.id.name
                           : (node->t ? node->t : "");
            size_t nl = strlen(nm);
            char *r = arena_alloc(&tr->cs->arena, nl + 9);
            memcpy(r, ".LABEL._", 8);
            memcpy(r + 8, nm, nl + 1);
            return r;
        }
        if (node->u.id.class_ == CLASS_const) {
            /* ConstRef.t (constref.py:30-32) == _value.t (the stored
             * value's textual form), NOT the const's mangled label. The
             * C parser stamps id_node->t = mangled at DIM/CONST decl
             * (parser.c ~4026), and the runtime tr_visit_var
             * (translator.c CLASS_const arm) later overwrites with the
             * folded value — but var_translator runs first. Recurse
             * into default_value_expr directly so `dim p at <const>`
             * emits `_p EQU <value>` not `_p EQU _<const>`
             * (dim_at_label8). */
            if (node->u.id.default_value_expr)
                return vt_traverse_const_expr(tr, node->u.id.default_value_expr);
            if (node->t) return node->t;
            return node->u.id.mangled ? node->u.id.mangled : "";
        }
        if (node->t != NULL) return node->t;
        if (node->u.id.has_address &&
            node->u.id.scope == SCOPE_global &&
            node->u.id.mangled != NULL)
            return node->u.id.mangled;
        if (node->u.id.mangled != NULL) return node->u.id.mangled;
        return node->u.id.name ? node->u.id.name : "";
    }

    if (node->tag == AST_UNARY) {
        const char *mid = node->u.unary.operator;
        AstNode *operand = node->child_count > 0 ? node->children[0] : NULL;
        if (mid && strcmp(mid, "MINUS") == 0) {
            const char *r = vt_traverse_const_expr(tr, operand);
            size_t rl = strlen(r);
            char *out = arena_alloc(&tr->cs->arena, rl + 3);
            out[0] = ' '; out[1] = '-';
            memcpy(out + 2, r, rl + 1);
            return out;
        }
        if (mid && strcmp(mid, "ADDRESS") == 0)
            return vt_traverse_const_expr(tr, operand);
        fprintf(stderr, "zxbc: traverse_const invalid unary op '%s'\n",
                mid ? mid : "");
        return "";
    }

    if (node->tag == AST_BINARY) {
        const char *mid = vt_binary_glyph(node->u.binary.operator);
        if (mid == NULL) {
            fprintf(stderr, "zxbc: traverse_const invalid binary op '%s'\n",
                    node->u.binary.operator ? node->u.binary.operator : "");
            return "";
        }
        AstNode *l = node->child_count > 0 ? node->children[0] : NULL;
        AstNode *r = node->child_count > 1 ? node->children[1] : NULL;
        const char *ls = vt_traverse_const_expr(tr, l);
        const char *rs = vt_traverse_const_expr(tr, r);
        size_t need = strlen(ls) + strlen(rs) + strlen(mid) + 8;
        char *out = arena_alloc(&tr->cs->arena, need);
        snprintf(out, need, "(%s) %s (%s)", ls, mid, rs);
        return out;
    }

    /* translator_visitor.py:242-243 — ARRAYACCESS:
     *   return f"({entry.data_label} + {offset})"
     * arrconst.bas: `DIM p = @a(0,0)` wraps UNARY(ADDRESS, ARRAYACCESS)
     * in CONSTEXPR (zxbparser:700-702: `not is_static -> wrap UNARY`),
     * and var_translator's CONSTEXPR-default-value branch routes through
     * traverse_const. The ADDRESS arm above recurses into operand, so
     * an ARRAYACCESS lands here. Use the same `(data_label + offset)`
     * shape Python emits — peephole-folds to a static address. */
    if (node->tag == AST_ARRAYACCESS) {
        AstNode *entry = node->child_count > 0 ? node->children[0] : NULL;
        const char *mangled = (entry && entry->u.id.mangled)
                                  ? entry->u.id.mangled : "";
        size_t ml = strlen(mangled);
        char *dlabel = arena_alloc(&tr->cs->arena, ml + 10);
        snprintf(dlabel, ml + 10, "%s.__DATA__", mangled);
        long off = (long)node->u.arrayaccess.offset;
        char *out = arena_alloc(&tr->cs->arena, ml + 32);
        snprintf(out, ml + 32, "(%s + %ld)", dlabel, off);
        return out;
    }

    fprintf(stderr,
            "zxbc: traverse_const unhandled node tag=%d\n", (int)node->tag);
    return node->t ? node->t : "";
}

/* Translator.default_value (translator.py:1029-1078). Returns the byte/word
 * 2-hex string list; writes into *out (cap-checked). type_ may be a TYPEREF
 * — resolved via tr_tsuffix-equivalent (final basic type) + type_size. */
int tr_default_value(Translator *tr, const TypeInfo *type_,
                     AstNode *expr, char **out, int out_cap) {
    /* asserts: type_ is TYPE, is_basic, expr is_static — the S5.3 callers
     * uphold these (VarTranslator only reaches here for a static defval). */
    const TypeInfo *f = type_ && type_->final_type ? type_->final_type
                                                   : type_;
    BasicType bt = f ? f->basic_type : TYPE_unknown;
    int tsize = type_size(type_);

    /* CONSTEXPR / CONST branch (translator.py:1036-1056). */
    if (expr && (expr->tag == AST_CONSTEXPR ||
                 (expr->tag == AST_ID && expr->u.id.class_ == CLASS_const))) {
        if (bt == TYPE_float || bt == TYPE_string) {
            zxbc_error(tr->cs, expr->lineno,
                       "Can't convert non-numeric value to %s at compile time",
                       basictype_to_string(bt));
            if (out_cap > 0) out[0] = arena_strdup(&tr->cs->arena, "<ERROR>");
            return out_cap > 0 ? 1 : 0;
        }
        /* translator.py:1041 val = Translator.traverse_const(expr).
         * Python's traverse_const is the FULL recursive walk
         * (translator_visitor.py:177-246: CONSTEXPR->.expr, UNARY
         * ADDRESS->operand, LABEL->LabelRef.t==".LABEL._<name>"). The
         * leaf-only vt_traverse_const cannot resolve a CONSTEXPR-wrapped
         * `@label` (returns its unset .t == ""), so a local
         * `DIM a(..) => {@l1,..}` emitted empty `DEFW`. Use the recursive
         * vt_traverse_const_expr — the faithful traverse_const analogue.
         * Verified no scalar/global-array regression (the const-vector
         * leaves are NUMBER/CONST/CONSTEXPR, identically resolved). */
        const char *val = vt_traverse_const_expr(tr, expr);
        const TypeInfo *ef = expr->type_ && expr->type_->final_type
                                 ? expr->type_->final_type : expr->type_;
        int esize = type_size(expr->type_);
        char tmp[256];
        if (tsize == 1) {                                  /* U/byte */
            if (esize != 1)
                snprintf(tmp, sizeof(tmp), "#(%s) & 0xFF", val);
            else
                snprintf(tmp, sizeof(tmp), "#%s", val);
            if (out_cap > 0) out[0] = arena_strdup(&tr->cs->arena, tmp);
            return out_cap > 0 ? 1 : 0;
        }
        if (tsize == 2) {                                  /* U/integer */
            if (esize != 2)
                snprintf(tmp, sizeof(tmp), "##(%s) & 0xFFFF", val);
            else
                snprintf(tmp, sizeof(tmp), "##%s", val);
            if (out_cap > 0) out[0] = arena_strdup(&tr->cs->arena, tmp);
            return out_cap > 0 ? 1 : 0;
        }
        if (bt == TYPE_fixed) {
            int c = 0;
            if (c < out_cap) out[c++] = arena_strdup(&tr->cs->arena, "0000");
            snprintf(tmp, sizeof(tmp), "##(%s) & 0xFFFF", val);
            if (c < out_cap) out[c++] = arena_strdup(&tr->cs->arena, tmp);
            return c;
        }
        /* U/Long */
        int c = 0;
        snprintf(tmp, sizeof(tmp), "##(%s) & 0xFFFF", val);
        if (c < out_cap) out[c++] = arena_strdup(&tr->cs->arena, tmp);
        snprintf(tmp, sizeof(tmp), "##((%s) >> 16) & 0xFFFF", val);
        if (c < out_cap) out[c++] = arena_strdup(&tr->cs->arena, tmp);
        (void)ef;
        return c;
    }

    /* float branch (translator.py:1058-1069):
     *   C, DE, HL = Float.float(expr.value)   # = fp.immediate_float
     *   C  = C[:-1];  C  = C[-2:]
     *   DE = DE[:-1]; DE = ("00" + DE)[-4:]
     *   HL = HL[:-1]; HL = ("00" + HL)[-4:]
     *   return [C, DE[-2:], DE[:-2], HL[-2:], HL[:-2]]
     * Float.float returns the ("0XXh","0XXXXh","0XXXXh") triple. */
    if (bt == TYPE_float) {
        double dvf = (expr && expr->tag == AST_NUMBER) ? expr->u.number.value
                                                       : 0.0;
        char Cs[8], DEs[8], HLs[8];
        z80h_immediate_float(dvf, Cs, DEs, HLs);

        /* C[:-1] (drop 'h') then [-2:] (last 2). "0XXh" -> "0XX" -> "XX". */
        size_t cl = strlen(Cs);                 /* "0XXh" => 4 */
        char cc[3];
        { size_t body = cl - 1;                 /* drop trailing 'h' */
          size_t st = body >= 2 ? body - 2 : 0;
          size_t k = 0;
          for (size_t i = st; i < body; i++) cc[k++] = Cs[i];
          cc[k] = '\0'; }

        /* ("00" + DE[:-1])[-4:]  ; DE,HL are "0XXXXh" => body "0XXXX". */
        char de4[5], hl4[5];
        const char *src[2] = { DEs, HLs };
        char *dst[2] = { de4, hl4 };
        for (int j = 0; j < 2; j++) {
            char pad[16];
            size_t bl = strlen(src[j]) - 1;     /* drop 'h' */
            snprintf(pad, sizeof(pad), "00%.*s", (int)bl, src[j]);
            size_t pl = strlen(pad);
            size_t st = pl >= 4 ? pl - 4 : 0;
            size_t k = 0;
            for (size_t i = st; i < pl; i++) dst[j][k++] = pad[i];
            dst[j][k] = '\0';
        }

        /* [C, DE[-2:], DE[:-2], HL[-2:], HL[:-2]] — de4/hl4 are 4 chars. */
        char dlo[3] = { de4[2], de4[3], 0 }, dhi[3] = { de4[0], de4[1], 0 };
        char hlo[3] = { hl4[2], hl4[3], 0 }, hhi[3] = { hl4[0], hl4[1], 0 };
        const char *vals[5] = { cc, dlo, dhi, hlo, hhi };
        int c = 0;
        for (int i = 0; i < 5 && c < out_cap; i++)
            out[c++] = arena_strdup(&tr->cs->arena, vals[i]);
        return c;
    }

    /* fixed / integer numeric branch (translator.py:1071-1078). */
    int64_t value;
    double dv = (expr && expr->tag == AST_NUMBER) ? expr->u.number.value
                                                  : 0.0;
    if (bt == TYPE_fixed)
        value = (int64_t)(0xFFFFFFFFLL & (int64_t)(dv * 65536.0));
    else
        value = (int64_t)dv;                /* Python int(expr.value) */

    /* values = [v, v>>8, v>>16, v>>24]; result = ["%02X"%(v&0xFF)];
     * return result[:type_.size]  (translator.py:1076-1078) */
    int64_t shifts[4] = { value, value >> 8, value >> 16, value >> 24 };
    int n = tsize < 4 ? tsize : 4;
    if (n > out_cap) n = out_cap;
    for (int i = 0; i < n; i++) {
        char b[8];
        snprintf(b, sizeof(b), "%02X", (unsigned)(shifts[i] & 0xFF));
        out[i] = arena_strdup(&tr->cs->arena, b);
    }
    return n;
}

/* ==================================================================== *
 *  S5.6 — Array data image (var_translator.py:45-103)                   *
 * ==================================================================== */

/* ic_data (translator_inst_visitor.py:94-95): emit("data",TSUFFIX,data). */
static void vt_ic_data(Translator *tr, const TypeInfo *type_,
                       char **data, int n) {
    const char *args[2] = { tr_tsuffix(type_), py_list_repr(tr, data, n) };
    tr_emit_quad(tr, "data", 2, args);
}

/* Translator.array_default_value (translator.py:1081-1092):
 *   if not list: return default_value(type_, values)
 *   else: l=[]; for row in values: l += array_default_value(type_, row)
 * The Python `values` nesting is the ARRAYINIT tree: an ARRAYINIT node is
 * the list (recurse its children as rows); any other node is a scalar
 * leaf (-> default_value). Appends arena-owned 2-hex strings to *out. */
int tr_array_default_value(Translator *tr, const TypeInfo *type_,
                                  AstNode *values, char **out,
                                  int out_off, int out_cap) {
    if (values && values->tag == AST_ARRAYINIT) {
        int off = out_off;
        for (int i = 0; i < values->child_count; i++)
            off = tr_array_default_value(tr, type_, values->children[i],
                                         out, off, out_cap);
        return off;
    }
    /* scalar leaf -> Translator.default_value(type_, leaf) */
    char buf[5];
    char *tmp[5];
    int got = tr_default_value(tr, type_, values, tmp, 5);
    (void)buf;
    int off = out_off;
    for (int i = 0; i < got && off < out_cap; i++)
        out[off++] = tmp[i];
    return off;
}

/* Numeric operator fold for a static array-bound expression, mirroring
 * the parser-side zxbc_eval_binop (parser.c) that backs Python's
 * eval_to_num.  Returns false (== Python's eval() exception -> None) for a
 * non-numeric / undefined op or a div/mod-by-zero. */
static bool vt_eval_binop(const char *op, double l, double r, double *out) {
    if (!op) return false;
    if (strcmp(op, "PLUS") == 0)  { *out = l + r; return true; }
    if (strcmp(op, "MINUS") == 0) { *out = l - r; return true; }
    if (strcmp(op, "MULT") == 0 || strcmp(op, "MUL") == 0) { *out = l * r; return true; }
    if (strcmp(op, "DIV") == 0)  { if (r == 0) return false; *out = l / r; return true; }
    if (strcmp(op, "MOD") == 0)  { if (r == 0) return false; *out = fmod(l, r); return true; }
    if (strcmp(op, "POW") == 0)  { *out = pow(l, r); return true; }
    if (strcmp(op, "SHL") == 0)  { *out = (double)((int64_t)l << (int64_t)r); return true; }
    if (strcmp(op, "SHR") == 0)  { *out = (double)((int64_t)l >> (int64_t)r); return true; }
    if (strcmp(op, "BAND") == 0) { *out = (double)((int64_t)l & (int64_t)r); return true; }
    if (strcmp(op, "BOR") == 0)  { *out = (double)((int64_t)l | (int64_t)r); return true; }
    if (strcmp(op, "BXOR") == 0) { *out = (double)((int64_t)l ^ (int64_t)r); return true; }
    if (strcmp(op, "LT") == 0)   { *out = (l <  r) ? 1 : 0; return true; }
    if (strcmp(op, "GT") == 0)   { *out = (l >  r) ? 1 : 0; return true; }
    if (strcmp(op, "EQ") == 0)   { *out = (l == r) ? 1 : 0; return true; }
    if (strcmp(op, "LE") == 0)   { *out = (l <= r) ? 1 : 0; return true; }
    if (strcmp(op, "GE") == 0)   { *out = (l >= r) ? 1 : 0; return true; }
    if (strcmp(op, "NE") == 0)   { *out = (l != r) ? 1 : 0; return true; }
    if (strcmp(op, "AND") == 0)  { *out = ((int64_t)l && (int64_t)r) ? 1 : 0; return true; }
    if (strcmp(op, "OR") == 0)   { *out = ((int64_t)l || (int64_t)r) ? 1 : 0; return true; }
    if (strcmp(op, "XOR") == 0)  { *out = ((!!(int64_t)l) ^ (!!(int64_t)r)) ? 1 : 0; return true; }
    return false;
}

/* eval_to_num analogue (api/utils.py:173) for a resolved BOUND expr.
 * Python's SymbolBOUND.make_node (bound.py:40-65) folds each bound to a
 * concrete integer at PARSE time via eval_to_num(node.t), so the
 * VarTranslator (var_translator.py:69/98-102/602) just reads
 * bound.upper - bound.lower + 1.  The C parser keeps the bound's
 * UNRESOLVED expr tree on the AST_BOUND children (parser.c:5084-5086) —
 * a NUMBER, a named CONST id, or a CONSTEXPR/BINARY/UNARY of those — so
 * the translator must evaluate it to the same integer Python stored.
 * This mirrors parser.c:zxbc_eval_to_num branch-for-branch (the same
 * evaluator bound_count/p_arr_decl already use at parse time), so a const
 * array bound (e.g. const3 `dim x(DGMAXY, DGMAXX)`, dim_const_const's
 * transitive `const MAXMOBS = MHEIGHT`) sizes the DATA image and idx
 * table identically.  Returns true + *out on a numeric resolution; false
 * == Python's None (e.g. an address-of leaf — but a bound never reaches
 * codegen non-numeric: make_bound rejected it). */
static bool vt_bound_eval(AstNode *n, double *out) {
    if (!n) return false;
    switch (n->tag) {
    case AST_NUMBER:
        *out = n->u.number.value;
        return true;
    case AST_CONSTEXPR:
        return n->child_count > 0 && vt_bound_eval(n->children[0], out);
    case AST_ID:
        /* A named CONST: Python's .t is the resolved numeric string
         * (const+number arithmetic already folded). Resolve via the
         * stored constant value (default_value_expr). */
        if (n->u.id.class_ == CLASS_const && n->u.id.default_value_expr)
            return vt_bound_eval(n->u.id.default_value_expr, out);
        return false;
    case AST_BINARY: {
        double l, r;
        if (n->child_count < 2) return false;
        if (!vt_bound_eval(n->children[0], &l)) return false;
        if (!vt_bound_eval(n->children[1], &r)) return false;
        return vt_eval_binop(n->u.binary.operator, l, r, out);
    }
    case AST_UNARY: {
        double v;
        const char *op = n->u.unary.operator;
        if (op && strcmp(op, "ADDRESS") == 0) return false;
        if (n->child_count < 1) return false;
        if (!vt_bound_eval(n->children[0], &v)) return false;
        if (op && strcmp(op, "MINUS") == 0) { *out = -v; return true; }
        if (op && strcmp(op, "PLUS") == 0)  { *out = v;  return true; }
        if (op && strcmp(op, "NOT") == 0)   { *out = (v == 0) ? 1 : 0; return true; }
        if (op && strcmp(op, "BNOT") == 0)  { *out = (double)(~(int64_t)v); return true; }
        return false;
    }
    default:
        return false;
    }
}

/* SymbolBOUND geometry: BOUND child is [lower expr, upper expr]
 * (parser.c:5084-5086; bound.py:33-37 lower/upper/count). Resolve the
 * bound expr to its folded integer (a NUMBER literal, a named CONST, or a
 * static const expression) — see vt_bound_eval. */
static long vt_bound_val(AstNode *n) {
    double v;
    if (vt_bound_eval(n, &v)) return (long)v;
    return 0;
}

/* VarTranslator.visit_ARRAYDECL (var_translator.py:45-103). The central
 * S5.6 data-image emitter. node is the parser ARRAYDECL:
 *   child[0] = entry (CLASS_array ID), child[1] = BOUNDLIST,
 *   then optional AT-addr expr and/or ARRAYINIT (parser.c:2156-2172).
 *
 * Faithfulness notes / scope: is_dynamically_accessed / lbound_used /
 * ubound_used are not yet modelled on the C ID (S5.7+); they default
 * false, so bound_ptrs stays ["0","0"] for the zero-based numeric corpus
 * (41/44/05/array0x), exactly matching Python there. The always-emitted
 * 2-ptr DATA row, the dim-0-omitting "%04X" idx table, the unconditional
 * data_ptr VARX, and the ARRAYINIT default lowering are ported verbatim. */
static void vt_visit_arraydecl(Translator *tr, AstNode *node) {
    AstNode *entry = node->child_count > 0 ? node->children[0] : NULL;
    if (!entry) return;

    AstNode *boundlist = NULL;
    AstNode *addr_expr = NULL;
    AstNode *init_node = NULL;
    for (int i = 1; i < node->child_count; i++) {
        AstNode *c = node->children[i];
        if (!c) continue;
        if (c->tag == AST_BOUNDLIST) { boundlist = c; continue; }
        /* ARRAYINIT or expression initializer; an AT-addr expr is any
         * other non-BOUNDLIST expr that precedes/replaces the init. The
         * parser appends AT-addr before init (parser.c:2170-2171). */
        if (c->tag == AST_ARRAYINIT) { init_node = c; continue; }
        if (init_node == NULL && addr_expr == NULL) addr_expr = c;
        else init_node = c;
    }
    int ndims = boundlist ? boundlist->child_count : 0;

    /* if not entry.accessed: warning_not_used; O>1 -> drop (DCE). */
    if (!entry->u.id.accessed) {
        warn_not_used(tr->cs, entry->lineno,
                      entry->u.id.name ? entry->u.id.name : "", "Variable");
        if (tr->cs->opts.optimization_level > 1)
            return;
    }

    const char *mangled = entry->u.id.mangled ? entry->u.id.mangled : "";
    size_t ml = strlen(mangled);

    char *lbound_label = arena_alloc(&tr->cs->arena, ml + 12);
    snprintf(lbound_label, ml + 12, "%s.__LBOUND__", mangled);
    char *ubound_label = arena_alloc(&tr->cs->arena, ml + 12);
    snprintf(ubound_label, ml + 12, "%s.__UBOUND__", mangled);

    /* is_zero_based: all bounds' lower == 0 (arrayref.py:89-91). */
    bool is_zero_based = true;
    for (int i = 0; i < ndims; i++) {
        AstNode *bd = boundlist->children[i];
        long lo = vt_bound_val(bd && bd->child_count > 0 ? bd->children[0]
                                                         : NULL);
        if (lo != 0) { is_zero_based = false; break; }
    }
    /* is_dynamically_accessed / lbound_used / ubound_used (arrayref.py
     * :23/24/29): read off the shared array ID entry (node->children[0],
     * the same symbol-table node every array access / LBOUND / UBOUND
     * site mutates — symboltable_declare stores one node, access_array /
     * access_id return it). Set by SymbolARRAYACCESS.__init__
     * (arrayaccess.py:37 -> parser.c array-access success path) and
     * p_expr_lbound_expr (zxbparser.py:3374/3376 -> parser.c
     * parse_builtin_func LBOUND/UBOUND gate). These are exactly the gates
     * VarTranslator.visit_ARRAYDECL (var_translator.py:58/61) ORs to
     * decide the descriptor's __LBOUND__ / __UBOUND__ ptr slot + the
     * trailing bound table for a non-zero-based global array.
     * OPTIONS.array_check defaults off (config.py). */
    bool is_dynamically_accessed = entry->u.id.is_dynamically_accessed;
    bool lbound_used = entry->u.id.lbound_used;
    bool ubound_used = entry->u.id.ubound_used;
    /* OPTIONS.array_check (config.py:222 / var_translator.py:61): when
     * set, the global array's __DATA__.__PTR__ bound_ptrs[1] is forced
     * to point at __UBOUND__ (and the __UBOUND__ vard table is emitted)
     * even when no static UBOUND reference (entry.ubound_used) drove
     * it.  Faithful to var_translator.py:61's `if entry.ubound_used or
     * OPTIONS.array_check`. Owned by `#pragma array_check = true`
     * (arrcheck.bas). */
    bool array_check = tr->cs->opts.array_check;

    char *bound_ptrs[2] = { "0", "0" };
    if (!is_zero_based && (is_dynamically_accessed || lbound_used))
        bound_ptrs[0] = lbound_label;
    if (ubound_used || array_check)
        bound_ptrs[1] = ubound_label;

    char *data_label = arena_alloc(&tr->cs->arena, ml + 10);
    snprintf(data_label, ml + 10, "%s.__DATA__", mangled);
    char *data_ptr_label = arena_alloc(&tr->cs->arena, ml + 18);
    snprintf(data_ptr_label, ml + 18, "%s.__DATA__.__PTR__", mangled);

    char *idx_table_label = backend_tmp_label(tr->backend);

    /* idx table: ["%04X" % (ndims-1)] + ["%04X"%(u-l+1) for bounds[1:]]
     * + ["%02X" % type_.size]  (var_translator.py:66-71). */
    char *idx[64];
    int idxn = 0;
    {
        char b[8];
        snprintf(b, sizeof(b), "%04X", (unsigned)((ndims - 1) & 0xFFFF));
        idx[idxn++] = arena_strdup(&tr->cs->arena, b);
        for (int i = 1; i < ndims && idxn < 63; i++) {
            AstNode *bd = boundlist->children[i];
            long lo = vt_bound_val(bd && bd->child_count > 0
                                       ? bd->children[0] : NULL);
            long hi = vt_bound_val(bd && bd->child_count > 1
                                       ? bd->children[1] : NULL);
            snprintf(b, sizeof(b), "%04X",
                     (unsigned)((hi - lo + 1) & 0xFFFF));
            idx[idxn++] = arena_strdup(&tr->cs->arena, b);
        }
        int esz = type_size(node->type_);
        snprintf(b, sizeof(b), "%02X", (unsigned)(esz & 0xFF));
        idx[idxn++] = arena_strdup(&tr->cs->arena, b);
    }

    /* count = product of bound counts; size = count * elem_size
     * (arrayref.py:40-47, arraydecl.py:34-37). */
    long count = 1;
    for (int i = 0; i < ndims; i++) {
        AstNode *bd = boundlist->children[i];
        long lo = vt_bound_val(bd && bd->child_count > 0 ? bd->children[0]
                                                         : NULL);
        long hi = vt_bound_val(bd && bd->child_count > 1 ? bd->children[1]
                                                         : NULL);
        count *= (hi - lo + 1);
    }
    long size = count * type_size(node->type_);

    /* Sized to total cell-byte count (per arrayref/arraydecl). Arena-owned
     * so it's safe past this function's scope (vt_ic_vard formats into the
     * py-list repr immediately, but the slots are read once during that
     * formatting). For an empty array (count==0) fall back to a 1-slot
     * placeholder to keep callers safe. */
    long cap = size > 0 ? size : 1;
    char **arr_data = (char **)arena_alloc(&tr->cs->arena,
                                           sizeof(char *) * (size_t)cap);
    int arr_n = 0;
    bool has_addr = (addr_expr != NULL);

    if (has_addr) {
        /* entry.addr -> ic_deflabel(data_label, addr) (no data row).
         * Array AT-addr can be @label / @a+1 -> full traverse_const. */
        const char *addr = vt_traverse_const_expr(tr, addr_expr);
        vt_ic_deflabel(tr, data_label, addr);
    } else if (init_node != NULL) {
        arr_n = tr_array_default_value(tr, node->type_, init_node,
                                       arr_data, 0, (int)cap);
    } else {
        for (long i = 0; i < size && arr_n < cap; i++)
            arr_data[arr_n++] = "00";
    }

    /* PTR_TYPE == uinteger (u16) — gl.PTR_TYPE on zx48k. */
    const TypeInfo *ptr = tr->cs->symbol_table->basic_types[TYPE_uinteger];

    /* ic_varx(node.mangled, PTR_TYPE, [idx_table_label]). */
    { char *one[1]; one[0] = idx_table_label;
      vt_ic_varx(tr, mangled, ptr, one, 1); }

    if (has_addr) {
        /* ic_varx(data_ptr_label, PTR_TYPE, [traverse_const(addr)]);
         * then ic_data(PTR_TYPE, bound_ptrs). */
        char *one[1]; one[0] = (char *)vt_traverse_const_expr(tr, addr_expr);
        vt_ic_varx(tr, data_ptr_label, ptr, one, 1);
        vt_ic_data(tr, ptr, bound_ptrs, 2);
    } else {
        char *one[1]; one[0] = data_label;
        vt_ic_varx(tr, data_ptr_label, ptr, one, 1);
        vt_ic_data(tr, ptr, bound_ptrs, 2);
        vt_ic_vard(tr, data_label, arr_data, arr_n);
    }

    vt_ic_vard(tr, idx_table_label, idx, idxn);

    if (strcmp(bound_ptrs[0], "0") != 0) {
        char *lb[64]; int n = 0;
        for (int i = 0; i < ndims && n < 64; i++) {
            AstNode *bd = boundlist->children[i];
            long lo = vt_bound_val(bd && bd->child_count > 0
                                       ? bd->children[0] : NULL);
            char b[8];
            snprintf(b, sizeof(b), "%04X", (unsigned)(lo & 0xFFFF));
            lb[n++] = arena_strdup(&tr->cs->arena, b);
        }
        vt_ic_vard(tr, lbound_label, lb, n);
    }
    if (strcmp(bound_ptrs[1], "0") != 0) {
        char *ub[64]; int n = 0;
        for (int i = 0; i < ndims && n < 64; i++) {
            AstNode *bd = boundlist->children[i];
            long hi = vt_bound_val(bd && bd->child_count > 1
                                       ? bd->children[1] : NULL);
            char b[8];
            snprintf(b, sizeof(b), "%04X", (unsigned)(hi & 0xFFFF));
            ub[n++] = arena_strdup(&tr->cs->arena, b);
        }
        vt_ic_vard(tr, ubound_label, ub, n);
    }
}

/* VarTranslator.visit_VARDECL (var_translator.py:26-43). node.children[0]
 * is the entry ID; VARDECL.type_/mangled/size/default_value forward to it
 * (vardecl.py:22-37). */
static void vt_visit_vardecl(Translator *tr, AstNode *node) {
    AstNode *entry = node->child_count > 0 ? node->children[0] : NULL;
    if (!entry) return;

    if (!entry->u.id.accessed) {
        warn_not_used(tr->cs, entry->lineno,
                      entry->u.id.name ? entry->u.id.name : "",
                      "Variable");
        if (tr->cs->opts.optimization_level > 1)
            return;                         /* unused vars not compiled */
    }

    if (entry->u.id.addr_expr != NULL) {
        /* visit_VARDECL (var_translator.py:33-35):
         *   addr = self.traverse_const(entry.addr)
         *            if isinstance(entry.addr, symbols.SYMBOL) else entry.addr
         *   self.ic_deflabel(entry.mangled, addr)
         * Python ALWAYS routes a SYMBOL entry.addr through the full
         * recursive Translator.traverse_const — the SAME walk used for
         * arrays (var_translator.c:151 vt_traverse_const_expr) — not the
         * leaf-only form. For the CONSTEXPR branch (p_var_decl_at:672-674,
         * `DIM x AT @label[+k]`) entry.addr is a UNARY ADDRESS / BINARY;
         * the leaf-only vt_traverse_const failed it ("not in scalar
         * scope") and emitted an empty `_x EQU ` operand. The recursive
         * form resolves @label -> .LABEL._<name> (labelref.py:20,34) and
         * is byte-identical for the NUMBER/CONST leaf addr the prior
         * scalar corpus (opt2_dim_at etc.) used. */
        const char *addr = vt_traverse_const_expr(tr, entry->u.id.addr_expr);
        vt_ic_deflabel(tr, entry->u.id.mangled, addr);
        return;
    }

    if (entry->u.id.default_value_expr == NULL) {
        vt_ic_var(tr, entry->u.id.mangled, type_size(entry->type_));
        return;
    }

    AstNode *dv = entry->u.id.default_value_expr;
    if (dv->tag == AST_CONSTEXPR) {
        /* Python var_translator.py:41 — traverse_const(default_value).
         * Python's traverse_const is the FULL recursive walk
         * (translator_visitor.py:177-246). The leaf-only vt_traverse_const
         * fails past the CONSTEXPR wrapper for shapes like @a(0,0)
         * (UNARY ADDRESS / ARRAYACCESS), emitting an empty operand.
         * Use the recursive vt_traverse_const_expr — same call array
         * var_translator already uses for ic_varx default values, so the
         * scalar leaf cases (NUMBER/CONST) stay byte-identical. */
        char *one[1];
        one[0] = (char *)vt_traverse_const_expr(tr, dv);
        vt_ic_varx(tr, entry->u.id.mangled, entry->type_, one, 1);
        return;
    }

    char *data[5];
    int n = tr_default_value(tr, entry->type_, dv, data, 5);
    vt_ic_vard(tr, entry->u.id.mangled, data, n);
}

/* VarTranslator.visit_LABEL (var_translator.py:21-24): ic_label(mangled)
 * for the entry and each alias. The S5.3 scalar corpus has no LABELs in
 * data_ast (data_ast is VARDECLs only); ported for the array/aliased
 * sprints. aliased_by is not yet modelled in the C ID node — emit just
 * the primary label (faithful for the no-alias case the S5.3 corpus has).
 */
static void vt_visit_label(Translator *tr, AstNode *node) {
    AstNode *lbl = node->child_count > 0 ? node->children[0] : node;
    const char *m = lbl->u.id.mangled ? lbl->u.id.mangled
                  : (lbl->u.id.name ? lbl->u.id.name : "");
    vt_ic_label(tr, m);
}

void var_translator_visit(Translator *tr, AstNode *data_ast) {
    if (!data_ast) return;
    /* data_ast is a BLOCK; iterate its children (VARDECL / LABEL). The
     * Python VarTranslator is a NodeVisitor over the data_ast tree —
     * here the tree is exactly one BLOCK level deep (p_start drains
     * VARDECL(entry) into it), so an explicit child walk is the faithful
     * analogue (no nested structure to recurse). */
    for (int i = 0; i < data_ast->child_count; i++) {
        AstNode *ch = data_ast->children[i];
        if (!ch) continue;
        if (ch->tag == AST_VARDECL)      vt_visit_vardecl(tr, ch);
        else if (ch->tag == AST_ARRAYDECL) vt_visit_arraydecl(tr, ch);
        else if (ch->tag == AST_SENTENCE &&
                 ch->u.sentence.kind &&
                 strcmp(ch->u.sentence.kind, "LABEL") == 0)
            vt_visit_label(tr, ch);
    }
}
