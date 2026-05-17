/*
 * opthelpers.c — see opthelpers.h. Faithful port of the helpers.py /
 * cpustate.py RE_OFFSET surface the O3 path uses.
 *
 * Python-eval faithfulness (simplify_arg, helpers.py:288-322): Python
 * does `eval(arg, {}, {})` and keeps the result iff it is int/float;
 * NameError / SyntaxError / ValueError -> "return arg unchanged". The
 * operands the optimizer feeds here are single asm tokens or simple
 * constant arithmetic expressions emitted by the backend (e.g.
 * "(_label + 1)", "4 + 3"). A bounded recursive-descent constant
 * evaluator over the Python numeric grammar reproduces eval's observable
 * result for exactly these: a pure-numeric constant expression folds to
 * its value; anything containing an identifier raises NameError in
 * Python and is returned unchanged here (we detect a bare/qualified name
 * and bail, matching eval's NameError). simplify_asm_args is only ever
 * invoked at optimization_level > 3 (basicblock.py:124 +
 * main.py:202) — out of the S5.9b inert-at-O2 scope but faithfully
 * reachable when O>3 is calibrated (S5.9c/S5.10).
 */
#include "opthelpers.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define UNK "*UNKNOWN_"

static char *h_strdup(Arena *a, const char *s) { return arena_strdup(a, s); }
static char *h_strndup(Arena *a, const char *s, size_t n) {
    return arena_strndup(a, s, n);
}
static int h_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

char *opt_new_tmp_val16(Arena *a) {
    char *lo = z80h_new_tmp_val(a);
    char *hi = z80h_new_tmp_val(a);
    /* f"{new_tmp_val()}{HL_SEP}{new_tmp_val()}" — lo generated first */
    size_t n = strlen(lo) + 1 + strlen(hi) + 1;
    char *r = (char *)arena_alloc(a, n);
    snprintf(r, n, "%s%s%s", lo, OPT_HL_SEP, hi);
    return r;
}

char *opt_new_tmp_val16_from_label(Arena *a, const char *label) {
    size_t n = strlen(UNK "H_") + strlen(label) + 1 +
               strlen(UNK "L_") + strlen(label) + 1;
    char *r = (char *)arena_alloc(a, n);
    snprintf(r, n, "%sH_%s%s%sL_%s", UNK, label, OPT_HL_SEP, UNK, label);
    return r;
}

static int hl_parts(const char *x) {
    int n = 1;
    for (const char *p = x; *p; p++) if (*p == '|') n++;
    return n;
}

bool opt_is_unknown8(const char *x) {
    if (x == NULL) return true;
    if (!z80h_is_unknown(x)) return false;
    return hl_parts(x) == 1;
}

bool opt_is_unknown16(const char *x) {
    if (x == NULL) return true;
    if (!z80h_is_unknown(x)) return false;
    return hl_parts(x) == 2;
}

/* split by HL_SEP, return last segment */
char *opt_get_L_from_unknown_value(Arena *a, const char *tmp_val) {
    const char *bar = strrchr(tmp_val, '|');
    return h_strdup(a, bar ? bar + 1 : tmp_val);
}
/* split by HL_SEP, return first segment */
char *opt_get_H_from_unknown_value(Arena *a, const char *tmp_val) {
    const char *bar = strchr(tmp_val, '|');
    return bar ? h_strndup(a, tmp_val, (size_t)(bar - tmp_val))
               : h_strdup(a, tmp_val);
}

char *opt_get_orig_label_from_unknown16(Arena *a, const char *x) {
    if (!opt_is_unknown16(x)) return NULL;
    char *H = opt_get_H_from_unknown_value(a, x);
    char *L = opt_get_L_from_unknown_value(a, x);
    const char *hp = UNK "H_", *lp = UNK "L_";
    size_t hpl = strlen(hp), lpl = strlen(lp);
    if (strncmp(H, hp, hpl) != 0) return NULL;
    const char *hi = H + hpl;
    if (strncmp(L, lp, lpl) != 0) return NULL;
    const char *lo = L + lpl;
    if (strcmp(hi, lo) != 0) return NULL;
    return h_strdup(a, hi);
}

bool opt_is_label(const char *x) {
    /* str(x)[:1] in "._" — Python: empty string -> "" not in "._" -> False */
    if (x == NULL) return false;       /* str(None)="None"[:1]="N" -> False */
    return x[0] == '.' || x[0] == '_';
}

bool opt_to_int(const char *x, long *out) {
    return z80h_valnum(x, out);
}

/* ---- constant-expression evaluator (Python eval over numeric grammar) */
typedef struct { const char *s; bool err; bool name; bool isflt;
                 double f; long i; } CEv;

static void ce_skip(CEv *c) { while (*c->s && h_ws((unsigned char)*c->s)) c->s++; }

/* forward */
static void ce_expr(CEv *c);

static void ce_atom(CEv *c) {
    ce_skip(c);
    char ch = *c->s;
    if (ch == '(') {
        c->s++;
        ce_expr(c);
        ce_skip(c);
        if (*c->s == ')') c->s++; else c->err = true;
        return;
    }
    if (ch == '+' || ch == '-') {
        c->s++;
        ce_atom(c);
        if (c->isflt) c->f = (ch == '-') ? -c->f : c->f;
        else c->i = (ch == '-') ? -c->i : c->i;
        return;
    }
    if (isdigit((unsigned char)ch) || ch == '.') {
        char *endp = NULL;
        const char *start = c->s;
        /* try integer first (no '.', 'e', no '..') */
        bool isf = false;
        for (const char *p = start; *p; p++) {
            if (*p == '.' || *p == 'e' || *p == 'E') { isf = true; break; }
            if (!isdigit((unsigned char)*p)) break;
        }
        if (isf) {
            double d = strtod(start, &endp);
            if (endp == start) { c->err = true; return; }
            c->isflt = true; c->f = d; c->s = endp;
        } else {
            long v = strtol(start, &endp, 10);
            if (endp == start) { c->err = true; return; }
            c->isflt = false; c->i = v; c->s = endp;
        }
        return;
    }
    /* identifier / anything else -> Python NameError / SyntaxError */
    c->name = true;
    c->err = true;
}

static double ce_f(const CEv *c) { return c->isflt ? c->f : (double)c->i; }

static void ce_pow(CEv *c) {
    ce_atom(c);
    if (c->err) return;
    ce_skip(c);
    if (c->s[0] == '*' && c->s[1] == '*') {
        bool lf = c->isflt; double lv = ce_f(c); long li = c->i;
        c->s += 2;
        ce_pow(c); /* right assoc */
        if (c->err) return;
        if (lf || c->isflt) {
            c->isflt = true;
            c->f = pow(lf ? lv : (double)li, ce_f(c));
        } else {
            c->isflt = false;
            c->i = (long)pow((double)li, (double)c->i);
        }
    }
}

static void ce_term(CEv *c) {
    ce_pow(c);
    for (;;) {
        ce_skip(c);
        char op = *c->s;
        if (op == '/' && c->s[1] == '/') {
            c->s += 2;
            bool lf = c->isflt; double lv = ce_f(c); long li = c->i;
            ce_pow(c); if (c->err) return;
            if (lf || c->isflt) { c->isflt = true;
                c->f = floor((lf ? lv : (double)li) / ce_f(c)); }
            else { if (c->i == 0) { c->err = true; return; }
                long q = li / c->i;
                if ((li % c->i != 0) && ((li < 0) != (c->i < 0))) q--;
                c->isflt = false; c->i = q; }
        } else if (op == '*' || op == '/' || op == '%') {
            c->s++;
            bool lf = c->isflt; double lv = ce_f(c); long li = c->i;
            ce_pow(c); if (c->err) return;
            if (op == '*') {
                if (lf || c->isflt) { c->isflt = true; c->f = (lf?lv:(double)li) * ce_f(c); }
                else { c->isflt = false; c->i = li * c->i; }
            } else if (op == '/') {
                c->isflt = true; double rv = ce_f(c);
                if (rv == 0) { c->err = true; return; }
                c->f = (lf?lv:(double)li) / rv;
            } else { /* % */
                if (lf || c->isflt) { c->isflt = true;
                    c->f = fmod((lf?lv:(double)li), ce_f(c)); }
                else { if (c->i == 0) { c->err = true; return; }
                    long m = li % c->i;
                    if (m != 0 && ((m < 0) != (c->i < 0))) m += c->i;
                    c->isflt = false; c->i = m; }
            }
        } else break;
    }
}

static void ce_expr(CEv *c) {
    ce_term(c);
    for (;;) {
        ce_skip(c);
        char op = *c->s;
        if (op == '+' || op == '-') {
            c->s++;
            bool lf = c->isflt; double lv = ce_f(c); long li = c->i;
            ce_term(c); if (c->err) return;
            if (lf || c->isflt) { c->isflt = true;
                c->f = (lf?lv:(double)li) + (op=='+'? ce_f(c) : -ce_f(c)); }
            else { c->isflt = false; c->i = (op=='+') ? li + c->i : li - c->i; }
        } else break;
    }
}

/* Evaluate constant expr. Returns true with result string on success
 * (int -> str(int), float -> str(float) Python repr-ish); false on
 * NameError/SyntaxError/ValueError (caller returns arg unchanged). */
static bool ce_eval(Arena *a, const char *expr, char **out) {
    CEv c; c.s = expr; c.err = false; c.name = false;
    c.isflt = false; c.f = 0; c.i = 0;
    ce_expr(&c);
    if (c.err) return false;
    ce_skip(&c);
    if (*c.s != '\0') return false; /* trailing garbage -> SyntaxError */
    char buf[64];
    if (c.isflt) {
        /* Python str(float): integral floats show ".0" */
        if (c.f == floor(c.f) && isfinite(c.f) && fabs(c.f) < 1e16)
            snprintf(buf, sizeof(buf), "%.1f", c.f);
        else snprintf(buf, sizeof(buf), "%.12g", c.f);
    } else {
        snprintf(buf, sizeof(buf), "%ld", c.i);
    }
    *out = h_strdup(a, buf);
    return true;
}

char *opt_simplify_arg(Arena *a, const char *arg0) {
    /* arg = arg.strip() */
    const char *b = arg0;
    while (*b && h_ws((unsigned char)*b)) b++;
    const char *e = arg0 + strlen(arg0);
    while (e > b && h_ws((unsigned char)e[-1])) e--;
    char *arg = h_strndup(a, b, (size_t)(e - b));

    char *result = NULL;
    if (!ce_eval(a, arg, &result)) {
        /* NameError/SyntaxError/ValueError -> result stays None */
        result = NULL;
    }
    if (result == NULL) return arg;        /* return arg */
    if (!z80h_is_mem_access(arg)) return result;  /* return result */
    /* return f"({result})" */
    size_t n = strlen(result) + 3;
    char *r = (char *)arena_alloc(a, n);
    snprintf(r, n, "(%s)", result);
    return r;
}

char *opt_simplify_asm_args(Arena *a, const char *asm_) {
    /* chunks = asm.split(" ", 1) */
    const char *sp = strchr(asm_, ' ');
    if (sp == NULL) return h_strdup(a, asm_);   /* len(chunks) != 2 */
    char *head = h_strndup(a, asm_, (size_t)(sp - asm_));
    const char *rest = sp + 1;
    /* args = [simplify_arg(x) for x in chunks[1].split(",", 1)] */
    const char *comma = strchr(rest, ',');
    char *a0, *a1 = NULL;
    if (comma) {
        a0 = h_strndup(a, rest, (size_t)(comma - rest));
        a1 = h_strdup(a, comma + 1);
    } else {
        a0 = h_strdup(a, rest);
    }
    char *s0 = opt_simplify_arg(a, a0);
    char *s1 = a1 ? opt_simplify_arg(a, a1) : NULL;
    /* "{} {}".format(head, ", ".join(args)) */
    size_t n = strlen(head) + 1 + strlen(s0) + (s1 ? 2 + strlen(s1) : 0) + 1;
    char *r = (char *)arena_alloc(a, n);
    if (s1) snprintf(r, n, "%s %s, %s", head, s0, s1);
    else    snprintf(r, n, "%s %s", head, s0);
    return r;
}

/* RE_IDX = r"^([iI][xXyY])[ ]*([-+])[ \t]*(.*)$" */
bool opt_idx_args(Arena *a, const char *x,
                  const char **reg, const char **sign, const char **args) {
    const char *p = x;
    if (!(p[0] == 'i' || p[0] == 'I')) return false;
    char c1 = p[1];
    if (!(c1 == 'x' || c1 == 'X' || c1 == 'y' || c1 == 'Y')) return false;
    char rb[3] = { (char)tolower((unsigned char)p[0]),
                   (char)tolower((unsigned char)p[1]), 0 };
    p += 2;
    while (*p == ' ') p++;            /* [ ]* (space only) */
    if (*p != '-' && *p != '+') return false;
    char sgn = *p; p++;
    while (*p == ' ' || *p == '\t') p++;  /* [ \t]* */
    *reg  = h_strdup(a, rb);
    char sb[2] = { sgn, 0 };
    *sign = h_strdup(a, sb);
    *args = h_strdup(a, p);          /* (.*)$ — greedy rest */
    return true;
}

void opt_dict_intersection(Arena *ar, OMap *out,
                           const OMap *da, const OMap *db) {
    omap_init(out);
    for (int i = 0; i < da->len; i++) {
        const char *k = da->data[i].key;
        const char *va = da->data[i].val;
        int bi = -1;
        for (int j = 0; j < db->len; j++)
            if (strcmp(db->data[j].key, k) == 0) { bi = j; break; }
        if (bi < 0) continue;
        const char *vb = db->data[bi].val;
        bool eq = (va == NULL && vb == NULL) ||
                  (va != NULL && vb != NULL && strcmp(va, vb) == 0);
        if (eq) omap_set(ar, out, k, va);
    }
}

/* RE_OFFSET = r"(^[*._a-zA-Z0-9]+(?:[+-]\d+)*)([+-]\d+)$"
 * Group 1 (greedy) then a final mandatory ([+-]\d+) at end-of-string.
 * Implemented by locating the LAST [+-]\d+ run that ends the string,
 * with the remaining prefix matching ^[*._a-zA-Z0-9]+(?:[+-]\d+)* . */
bool opt_re_offset(Arena *a, const char *addr, const char **base, long *off) {
    size_t n = strlen(addr);
    if (n == 0) return false;
    /* find the trailing ([+-]\d+) : a sign followed by 1+ digits at end */
    size_t i = n;
    while (i > 0 && isdigit((unsigned char)addr[i - 1])) i--;
    if (i == n) return false;            /* no trailing digits */
    if (i == 0) return false;            /* need a sign before digits */
    if (addr[i - 1] != '+' && addr[i - 1] != '-') return false;
    size_t sign_pos = i - 1;
    if (sign_pos == 0) return false;     /* group1 needs >=1 leading char */
    /* group1 = addr[0:sign_pos] must match ^[*._a-zA-Z0-9]+(?:[+-]\d+)* */
    size_t j = 0;
    /* [*._a-zA-Z0-9]+ */
    size_t k = 0;
    while (k < sign_pos) {
        char ch = addr[k];
        if (ch == '*' || ch == '.' || ch == '_' ||
            isalnum((unsigned char)ch)) k++;
        else break;
    }
    if (k == 0) return false;
    j = k;
    /* (?:[+-]\d+)* */
    while (j < sign_pos) {
        if (addr[j] != '+' && addr[j] != '-') return false;
        j++;
        size_t d0 = j;
        while (j < sign_pos && isdigit((unsigned char)addr[j])) j++;
        if (j == d0) return false;       /* \d+ requires >=1 digit */
    }
    if (j != sign_pos) return false;
    *base = h_strndup(a, addr, sign_pos);
    long v = strtol(addr + sign_pos, NULL, 10);
    *off = v;
    return true;
}
