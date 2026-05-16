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

/* Python repr of a list[str]: "['a', 'b']" (single quotes, ", " sep, the
 * S5.3 element strings never contain quotes/backslashes — hex bytes,
 * "#lit", "##expr", "(expr) & 0xFFFF"). Arena-owned result. */
static char *py_list_repr(Translator *tr, char **items, int n) {
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
 * :177-184). The S5.3 corpus's CONSTEXPR initializers — when present — are
 * scalar NUMBER/CONST; UNARY/BINARY constant folding is out of the S5.3
 * NUMBER-literal slice. Fail loud past the leaf rather than mis-evaluate
 * silently (CLAUDE.md rules 8 & 9). */
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
            "zxbc: traverse_const non-leaf (tag=%d) not in S5.3 scope\n",
            (int)node->tag);
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
        const char *val = vt_traverse_const(tr, expr);
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
        /* entry.addr is a SYMBOL (the typecast) -> traverse_const; the
         * non-SYMBOL int form (var_translator.py:34) is the C int64 addr
         * path, unused by the S5.3 corpus. */
        const char *addr = vt_traverse_const(tr, entry->u.id.addr_expr);
        vt_ic_deflabel(tr, entry->u.id.mangled, addr);
        return;
    }

    if (entry->u.id.default_value_expr == NULL) {
        vt_ic_var(tr, entry->u.id.mangled, type_size(entry->type_));
        return;
    }

    AstNode *dv = entry->u.id.default_value_expr;
    if (dv->tag == AST_CONSTEXPR) {
        char *one[1];
        one[0] = (char *)vt_traverse_const(tr, dv);
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
        else if (ch->tag == AST_SENTENCE &&
                 ch->u.sentence.kind &&
                 strcmp(ch->u.sentence.kind, "LABEL") == 0)
            vt_visit_label(tr, ch);
    }
}
