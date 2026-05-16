/*
 * evaluator.c — see evaluator.h. Faithful port of evaluator.py.
 *
 * UNARY/BINARY tables, Number arithmetic, the Evaluator.__init__ recursive
 * wrap and eval (lazy binary thunks via on-demand recursion) and
 * normalize() are reproduced exactly. RE_SVAR semantics for the len-1
 * atom path match pattern.py's RE_SVAR (start-anchored).
 */
#include "evaluator.h"
#include "z80asm.h"
#include "memcell.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "utils.h" /* common parse_int — port of src.api.utils.parse_int */

/* -------------------------------------------------------------------- */
static const char *FN_NAMES[] = {
    "!", "+", "==", "<>", "&&", "||", "IN", ",", ".+", ".-", ".*", "./",
    "IS_ASM", "IS_INDIR", "IS_REG16", "IS_REG8", "IS_LABEL", "IS_IMMED",
    "LEN", "INSTR", "HIREG", "LOREG", "HIVAL", "LOVAL", "GVAL",
    "IS_REQUIRED", "CTEST", "NEEDS", "FLAGVAL", "OP1", "OP2"
};

const char *fn_name(FN f) {
    if (f < 0 || f >= FN__INVALID) return "";
    return FN_NAMES[f];
}

bool fn_lookup(const char *s, FN *out) {
    for (int i = 0; i < (int)FN__INVALID; i++) {
        if (strcmp(s, FN_NAMES[i]) == 0) { *out = (FN)i; return true; }
    }
    return false;
}

/* BINARY = {OP_EQ, OP_PLUS, OP_NE, OP_AND, OP_OR, OP_IN, OP_COMMA,
 *           OP_NPLUS, OP_NSUB, OP_NMUL, OP_NDIV} */
bool fn_is_binary(FN f) {
    switch (f) {
    case FN_OP_EQ: case FN_OP_PLUS: case FN_OP_NE: case FN_OP_AND:
    case FN_OP_OR: case FN_OP_IN: case FN_OP_COMMA: case FN_OP_NPLUS:
    case FN_OP_NSUB: case FN_OP_NMUL: case FN_OP_NDIV:
        return true;
    default: return false;
    }
}
/* UNARY = all the rest of FN (OP_NOT + IS_* + LEN/INSTR/.../OP1/OP2) */
bool fn_is_unary(FN f) {
    switch (f) {
    case FN_OP_NOT: case FN_IS_ASM: case FN_IS_INDIR: case FN_IS_REG16:
    case FN_IS_REG8: case FN_IS_LABEL: case FN_IS_IMMED: case FN_LEN:
    case FN_INSTR: case FN_HIREG: case FN_LOREG: case FN_HIVAL:
    case FN_LOVAL: case FN_GVAL: case FN_IS_REQUIRED: case FN_CTEST:
    case FN_NEEDS: case FN_FLAGVAL: case FN_OP1: case FN_OP2:
        return true;
    default: return false;
    }
}
bool fn_is_oper(const char *s) {
    FN f;
    if (!fn_lookup(s, &f)) return false;
    return fn_is_unary(f) || fn_is_binary(f);
}
bool fn_is_unary_name(const char *s) {
    FN f; return fn_lookup(s, &f) && fn_is_unary(f);
}
bool fn_is_binary_name(const char *s) {
    FN f; return fn_lookup(s, &f) && fn_is_binary(f);
}

/* -------------------------------------------------------------------- */
PNode *pnode_str(Arena *a, const char *s) {
    PNode *n = (PNode *)arena_alloc(a, sizeof(PNode));
    n->kind = PN_STR;
    n->str = arena_strdup(a, s ? s : "");
    vec_init(n->list);
    return n;
}
PNode *pnode_list(Arena *a) {
    PNode *n = (PNode *)arena_alloc(a, sizeof(PNode));
    n->kind = PN_LIST;
    n->str = NULL;
    vec_init(n->list);
    return n;
}
void pnode_push(PNode *l, PNode *child) { vec_push(l->list, child); }

PNode *pnode_clone(Arena *a, const PNode *n) {
    if (n->kind == PN_STR) return pnode_str(a, n->str);
    PNode *r = pnode_list(a);
    for (int i = 0; i < n->list.len; i++)
        vec_push(r->list, pnode_clone(a, n->list.data[i]));
    return r;
}

/* -------------------------------------------------------------------- */
/* Ev mirrors Python self.expression: an ordered list of slots; a slot is
 * either a wrapped Evaluator, a raw string, a raw PNode list, or the
 * literal boolean True (empty-expression case). */
typedef enum { ES_EV, ES_STR, ES_LIST, ES_TRUE } EvSlotKind;
typedef struct {
    EvSlotKind kind;
    Ev        *ev;
    char      *str;
    const PNode *list;
} EvSlot;
typedef VEC(EvSlot) EvSlotVec;

struct Ev {
    EvSlotVec expr;
};

/* RE_SVAR.match(val): ^\$(\$|[0-9]+) ... start-anchored only. */
static bool re_svar_match(const char *v) {
    if (v[0] != '$') return false;
    if (v[1] == '$') return true;
    return isdigit((unsigned char)v[1]) != 0;
}

static Ev *ev_alloc(Arena *a) {
    Ev *e = (Ev *)arena_alloc(a, sizeof(Ev));
    vec_init(e->expr);
    return e;
}

static EvSlot slot_ev(Ev *e)        { EvSlot s; s.kind=ES_EV; s.ev=e; s.str=NULL; s.list=NULL; return s; }
static EvSlot slot_str(char *v)     { EvSlot s; s.kind=ES_STR; s.ev=NULL; s.str=v; s.list=NULL; return s; }
static EvSlot slot_list(const PNode *p){ EvSlot s; s.kind=ES_LIST; s.ev=NULL; s.str=NULL; s.list=p; return s; }
static EvSlot slot_true(void)       { EvSlot s; s.kind=ES_TRUE; s.ev=NULL; s.str=NULL; s.list=NULL; return s; }

/* forward */
static Ev *ev_build(Arena *a, const PNode *expr, bool *verr);

/* Evaluator.__init__ */
static Ev *ev_build(Arena *a, const PNode *expr0, bool *verr) {
    Ev *self = ev_alloc(a);

    /* if not isinstance(expression, list): expression = [expression] */
    const PNode *exprlist;
    PNode tmp;
    if (expr0->kind != PN_LIST) {
        tmp.kind = PN_LIST; tmp.str = NULL; vec_init(tmp.list);
        vec_push(tmp.list, (PNode *)expr0);
        exprlist = &tmp;
    } else {
        exprlist = expr0;
    }
    int n = exprlist->list.len;

    if (n == 0) {
        /* self.expression = [True] */
        vec_push(self->expr, slot_true());
        return self;
    }
    if (n == 1) {
        /* self.expression = expression  (the single raw element kept) */
        PNode *el = exprlist->list.data[0];
        if (el->kind == PN_STR) vec_push(self->expr, slot_str(el->str));
        else vec_push(self->expr, slot_list(el));
        return self;
    }
    if (n == 2) {
        PNode *op = exprlist->list.data[0];
        FN f;
        if (op->kind != PN_STR || !fn_lookup(op->str, &f) || !fn_is_unary(f)) {
            *verr = true; return NULL;
        }
        vec_push(self->expr, slot_str(op->str));
        Ev *c = ev_build(a, exprlist->list.data[1], verr);
        if (*verr) return NULL;
        vec_push(self->expr, slot_ev(c));
        return self;
    }
    /* len(expression) == 3 and expression[1] != FN.OP_COMMA */
    if (n == 3) {
        PNode *mid = exprlist->list.data[1];
        bool is_comma = (mid->kind == PN_STR && strcmp(mid->str, ",") == 0);
        if (!is_comma) {
            FN f;
            if (mid->kind != PN_STR || !fn_lookup(mid->str, &f) || !fn_is_binary(f)) {
                *verr = true; return NULL;
            }
            Ev *l = ev_build(a, exprlist->list.data[0], verr);
            if (*verr) return NULL;
            vec_push(self->expr, slot_ev(l));
            vec_push(self->expr, slot_str(mid->str));
            Ev *r = ev_build(a, exprlist->list.data[2], verr);
            if (*verr) return NULL;
            vec_push(self->expr, slot_ev(r));
            return self;
        }
    }
    /* else: It's a list. assert odd; odd indices are FN.OP_COMMA;
     * even -> Evaluator(x), odd -> kept as-is. */
    /* (Python assert; the parser only ever produces valid comma lists.) */
    for (int i = 0; i < n; i++) {
        PNode *x = exprlist->list.data[i];
        if (i % 2) {
            /* the "," separators (kept as raw strings) */
            vec_push(self->expr, slot_str(x->kind == PN_STR ? x->str
                                          : arena_strdup(a, ",")));
        } else {
            Ev *c = ev_build(a, x, verr);
            if (*verr) return NULL;
            vec_push(self->expr, slot_ev(c));
        }
    }
    return self;
}

Ev *ev_new(Arena *a, const PNode *expression, bool *value_error) {
    bool verr = false;
    Ev *e = ev_build(a, expression, &verr);
    if (value_error) *value_error = verr;
    return verr ? NULL : e;
}

/* -------------------------------------------------------------------- */
/* EvVal helpers */
static EvVal *vstr(Arena *a, const char *s) {
    EvVal *v = (EvVal *)arena_alloc(a, sizeof(EvVal));
    v->kind = EVV_STR; v->s = arena_strdup(a, s ? s : ""); v->b = false;
    vec_init(v->list);
    return v;
}
static EvVal *vbool(Arena *a, bool b) {
    EvVal *v = (EvVal *)arena_alloc(a, sizeof(EvVal));
    v->kind = EVV_BOOL; v->b = b; v->s = NULL; vec_init(v->list);
    return v;
}
static EvVal *vlist(Arena *a) {
    EvVal *v = (EvVal *)arena_alloc(a, sizeof(EvVal));
    v->kind = EVV_LIST; v->s = NULL; v->b = false; vec_init(v->list);
    return v;
}

bool evval_truthy(const EvVal *v) {
    if (!v) return false;
    switch (v->kind) {
    case EVV_BOOL: return v->b;
    case EVV_STR:  return v->s && v->s[0] != '\0';
    case EVV_LIST: return v->list.len > 0;
    }
    return false;
}

/* Python str(value): bool->"True"/"False", str->itself, list->repr.
 * str of a list element uses repr (quoted) — Python list __str__. */
static void list_repr(Arena *a, const EvVal *v, char **buf, size_t *cap, size_t *len);
static void sb_app(Arena *a, char **buf, size_t *cap, size_t *len, const char *s) {
    size_t sl = strlen(s);
    if (*len + sl + 1 > *cap) {
        size_t nc = (*cap ? *cap : 32);
        while (nc < *len + sl + 1) nc *= 2;
        char *nb = (char *)arena_alloc(a, nc);
        if (*buf) memcpy(nb, *buf, *len);
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, s, sl); *len += sl; (*buf)[*len] = '\0';
}
static void elem_repr(Arena *a, const EvVal *v, char **buf, size_t *cap, size_t *len) {
    if (v->kind == EVV_LIST) { list_repr(a, v, buf, cap, len); return; }
    if (v->kind == EVV_BOOL) { sb_app(a, buf, cap, len, v->b ? "True" : "False"); return; }
    /* str element -> Python repr is "'...'" */
    sb_app(a, buf, cap, len, "'");
    sb_app(a, buf, cap, len, v->s ? v->s : "");
    sb_app(a, buf, cap, len, "'");
}
static void list_repr(Arena *a, const EvVal *v, char **buf, size_t *cap, size_t *len) {
    sb_app(a, buf, cap, len, "[");
    for (int i = 0; i < v->list.len; i++) {
        if (i) sb_app(a, buf, cap, len, ", ");
        elem_repr(a, v->list.data[i], buf, cap, len);
    }
    sb_app(a, buf, cap, len, "]");
}

char *evval_str(Arena *a, const EvVal *v) {
    if (!v) return arena_strdup(a, "");
    if (v->kind == EVV_BOOL) return arena_strdup(a, v->b ? "True" : "False");
    if (v->kind == EVV_STR)  return arena_strdup(a, v->s ? v->s : "");
    char *buf = NULL; size_t cap = 0, len = 0;
    list_repr(a, v, &buf, &cap, &len);
    return buf ? buf : arena_strdup(a, "[]");
}

/* normalize(value): if not value -> ""; else str(value). */
static EvVal *normalize(Arena *a, const EvVal *value) {
    if (!evval_truthy(value)) return vstr(a, "");
    return vstr(a, evval_str(a, value));
}

/* -------------------------------------------------------------------- */
/* Number: utils.parse_int-backed nullable int. */
typedef struct { bool none; long v; } Number;
static Number num_make_str(const char *s) {
    Number n;
    int iv;
    if (s && parse_int(s, &iv)) { n.none = false; n.v = iv; }
    else { n.none = true; n.v = 0; }
    return n;
}
static char *num_str(Arena *a, Number n) {
    if (n.none) return arena_strdup(a, "");
    char b[32]; snprintf(b, sizeof(b), "%ld", n.v);
    return arena_strdup(a, b);
}

/* -------------------------------------------------------------------- */
/* UNARY application: returns a raw value (str|bool|list). The caller
 * applies normalize(). */
static EvVal *apply_unary(Arena *a, FN op, const EvVal *xv,
                          const HashMap *vars_, bool *unbound);

/* str.strip() / .lower() helpers */
static int e_ws(int c){return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v';}
static char *e_strip(Arena *a, const char *s) {
    const char *b=s; while(*b&&e_ws((unsigned char)*b))b++;
    const char *e=s+strlen(s); while(e>b&&e_ws((unsigned char)e[-1]))e--;
    return arena_strndup(a,b,(size_t)(e-b));
}
static char *e_lower(Arena *a, const char *s) {
    size_t n=strlen(s); char *r=(char*)arena_alloc(a,n+1);
    for(size_t i=0;i<n;i++) r[i]=(char)tolower((unsigned char)s[i]);
    r[n]='\0'; return r;
}
/* the value coerced to a string for ops that expect `x` to be a str */
static const char *as_str(const EvVal *v) {
    if (!v) return "";
    if (v->kind == EVV_STR) return v->s ? v->s : "";
    if (v->kind == EVV_BOOL) return v->b ? "True" : "False";
    return ""; /* list: only NEEDS indexes it (handled specially) */
}

static bool in_lset(const char *x, const char *const *s, size_t n){
    for(size_t i=0;i<n;i++) if(strcmp(x,s[i])==0) return true; return false;
}

static EvVal *apply_unary(Arena *a, FN op, const EvVal *xv,
                          const HashMap *vars_, bool *unbound) {
    (void)vars_; (void)unbound;
    const char *x = as_str(xv);
    switch (op) {
    case FN_OP_NOT: /* not x */
        return vbool(a, !evval_truthy(xv));
    case FN_IS_ASM: /* x.startswith("##ASM") */
        return vbool(a, strncmp(x, "##ASM", 5) == 0);
    case FN_IS_INDIR: { /* bool(RE_IXIY_IDX.match(x))
        RE_IXIY_IDX = r"^\([ \t]*i[xy][ \t]*[-+][ \t]*.+\)$" */
        const char *p = x;
        if (*p != '(') return vbool(a, false);
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (tolower((unsigned char)p[0]) != 'i') return vbool(a, false);
        char c1 = (char)tolower((unsigned char)p[1]);
        if (c1 != 'x' && c1 != 'y') return vbool(a, false);
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '-' && *p != '+') return vbool(a, false);
        p++;
        while (*p == ' ' || *p == '\t') p++;
        /* .+ then ')' at end-of-string ($) */
        size_t xl = strlen(x);
        if (xl == 0 || x[xl - 1] != ')') return vbool(a, false);
        const char *dotplus_end = x + xl - 1; /* must have >=1 char before ) */
        return vbool(a, dotplus_end > p);
    }
    case FN_IS_REG16: { /* x.strip().lower() in (af,bc,de,hl,ix,iy) */
        char *t = e_lower(a, e_strip(a, x));
        static const char *S[] = {"af","bc","de","hl","ix","iy"};
        return vbool(a, in_lset(t, S, 6));
    }
    case FN_IS_REG8: {
        char *t = e_lower(a, e_strip(a, x));
        static const char *S[] = {"a","b","c","d","e","h","l",
                                  "ixh","ixl","iyh","iyl"};
        return vbool(a, in_lset(t, S, 11));
    }
    case FN_IS_LABEL: { /* x.strip()[-1:] == ":" */
        char *t = e_strip(a, x);
        size_t tl = strlen(t);
        return vbool(a, tl > 0 && t[tl - 1] == ':');
    }
    case FN_IS_IMMED: { /* not x.strip().startswith("(") */
        char *t = e_strip(a, x);
        return vbool(a, t[0] != '(');
    }
    case FN_LEN: { /* str(len(x.split())) — whitespace split, no empties */
        int cnt = 0; const char *p = x;
        while (*p) {
            while (*p && e_ws((unsigned char)*p)) p++;
            if (!*p) break;
            cnt++;
            while (*p && !e_ws((unsigned char)*p)) p++;
        }
        char b[16]; snprintf(b, sizeof(b), "%d", cnt);
        return vstr(a, b);
    }
    case FN_INSTR: { /* x.strip().split()[0] */
        char *t = e_strip(a, x);
        const char *p = t;
        while (*p && e_ws((unsigned char)*p)) p++;
        const char *q = p;
        while (*q && !e_ws((unsigned char)*q)) q++;
        return vstr(a, arena_strndup(a, p, (size_t)(q - p)));
    }
    case FN_HIREG: case FN_LOREG: { /* dict.get(x.strip().lower(), "") */
        char *t = e_lower(a, e_strip(a, x));
        struct { const char *k, *hi, *lo; } M[] = {
            {"af","a","f"}, {"bc","b","c"}, {"de","d","e"},
            {"hl","h","l"}, {"ix","ixh","ixl"}, {"iy","iyh","iyl"}};
        for (size_t i = 0; i < 6; i++)
            if (strcmp(t, M[i].k) == 0)
                return vstr(a, op == FN_HIREG ? M[i].hi : M[i].lo);
        return vstr(a, "");
    }
    case FN_HIVAL: return vstr(a, z80h_HI16_val(a, x[0] ? x : NULL));
    case FN_LOVAL: return vstr(a, z80h_LO16_val(a, x[0] ? x : NULL));
    case FN_GVAL:
    case FN_FLAGVAL: /* lambda x: helpers.new_tmp_val() */
        return vstr(a, z80h_new_tmp_val(a));
    case FN_IS_REQUIRED: /* lambda x: True */
        return vbool(a, true);
    case FN_CTEST: { /* memcell.MemCell(x, 1).condition_flag */
        MemCell *m = memcell_new(a, x, 1);
        return m->cond ? vstr(a, m->cond) : vstr(a, ""); /* None -> "" via normalize */
    }
    case FN_NEEDS: { /* memcell.MemCell(x[0],1).needs(x[1]) */
        /* xv must be a list [instr, reglist]. */
        if (!xv || xv->kind != EVV_LIST || xv->list.len < 2)
            return vbool(a, false);
        const EvVal *e0 = xv->list.data[0];
        const EvVal *e1 = xv->list.data[1];
        const char *instr = (e0->kind == EVV_STR) ? e0->s : "";
        MemCell *m = memcell_new(a, instr, 1);
        Z80StrList rl; vec_init(rl);
        if (e1->kind == EVV_LIST) {
            for (int i = 0; i < e1->list.len; i++) {
                const EvVal *it = e1->list.data[i];
                vec_push(rl, arena_strdup(a, it->kind == EVV_STR ? it->s : ""));
            }
        } else if (e1->kind == EVV_STR) {
            vec_push(rl, arena_strdup(a, e1->s));
        }
        bool nd = memcell_needs(a, m, &rl);
        vec_free(rl);
        return vbool(a, nd);
    }
    case FN_OP1: { /* (x.strip().replace(",", " ", 1).split() + [""])[1] */
        char *t = e_strip(a, x);
        /* replace first ',' with ' ' */
        char *r = arena_strdup(a, t);
        char *comma = strchr(r, ',');
        if (comma) *comma = ' ';
        /* split() then [.. , ""][1] : 2nd whitespace token or "" */
        const char *p = r; int idx = 0; const char *tok = ""; size_t tl = 0;
        while (*p) {
            while (*p && e_ws((unsigned char)*p)) p++;
            if (!*p) break;
            const char *q = p; while (*q && !e_ws((unsigned char)*q)) q++;
            idx++;
            if (idx == 2) { tok = p; tl = (size_t)(q - p); break; }
            p = q;
        }
        return vstr(a, idx >= 2 ? arena_strndup(a, tok, tl) : "");
    }
    case FN_OP2: { /* (x.strip().replace(",", " ", 1).split() + ["",""])[2] */
        char *t = e_strip(a, x);
        char *r = arena_strdup(a, t);
        char *comma = strchr(r, ',');
        if (comma) *comma = ' ';
        const char *p = r; int idx = 0; const char *tok = ""; size_t tl = 0;
        while (*p) {
            while (*p && e_ws((unsigned char)*p)) p++;
            if (!*p) break;
            const char *q = p; while (*q && !e_ws((unsigned char)*q)) q++;
            idx++;
            if (idx == 3) { tok = p; tl = (size_t)(q - p); break; }
            p = q;
        }
        return vstr(a, idx >= 3 ? arena_strndup(a, tok, tl) : "");
    }
    default:
        return vstr(a, "");
    }
}

/* eval forward */
static EvVal *do_eval(Arena *a, Ev *e, const HashMap *vars_, bool *unbound);

/* Evaluate one slot to a value (a wrapped Evaluator, or a raw str/list). */
static EvVal *slot_eval(Arena *a, const EvSlot *s, const HashMap *vars_,
                        bool *unbound) {
    if (s->kind == ES_EV) return do_eval(a, s->ev, vars_, unbound);
    if (s->kind == ES_TRUE) return vbool(a, true);
    if (s->kind == ES_STR) return vstr(a, s->str);
    /* ES_LIST raw — Python returns the list object as-is (len-1 path) */
    EvVal *lv = vlist(a);
    if (s->list) {
        for (int i = 0; i < s->list->list.len; i++) {
            PNode *c = s->list->list.data[i];
            vec_push(lv->list, c->kind == PN_STR ? vstr(a, c->str) : vstr(a, ""));
        }
    }
    return lv;
}

/* BINARY application with lazy operands (thunks == deferred do_eval). */
static EvVal *apply_binary(Arena *a, FN op, Ev *lhs, Ev *rhs,
                           const HashMap *vars_, bool *unbound) {
    switch (op) {
    case FN_OP_EQ: { /* x() == y()
        Binary operands are wrapped Evaluators: each eval() yields a
        normalized string (unary/binary child), an atom string, a vars_
        string, or a list. Python `==` is value equality; for the string
        operands the .opt grammar produces this is strcmp. (list==list
        never occurs in the shipped patterns; list vs non-list -> False,
        which the kind check yields via differing string reprs.) */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *y = do_eval(a, rhs, vars_, unbound); if (*unbound) return NULL;
        if (x->kind == EVV_LIST || y->kind == EVV_LIST)
            return vbool(a, x->kind == EVV_LIST && y->kind == EVV_LIST &&
                            strcmp(evval_str(a, x), evval_str(a, y)) == 0);
        return vbool(a, strcmp(as_str(x), as_str(y)) == 0);
    }
    case FN_OP_PLUS: { /* x() + y()  (string concat in this AST) */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *y = do_eval(a, rhs, vars_, unbound); if (*unbound) return NULL;
        const char *xs = as_str(x), *ys = as_str(y);
        size_t xl = strlen(xs), yl = strlen(ys);
        char *r = (char *)arena_alloc(a, xl + yl + 1);
        memcpy(r, xs, xl); memcpy(r + xl, ys, yl); r[xl + yl] = '\0';
        return vstr(a, r);
    }
    case FN_OP_NE: { /* x() != y() */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *y = do_eval(a, rhs, vars_, unbound); if (*unbound) return NULL;
        return vbool(a, strcmp(as_str(x), as_str(y)) != 0);
    }
    case FN_OP_AND: { /* x() and y()  -> python: returns x() if falsy else y() */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        if (!evval_truthy(x)) return x;
        return do_eval(a, rhs, vars_, unbound);
    }
    case FN_OP_OR: { /* x() or y() */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        if (evval_truthy(x)) return x;
        return do_eval(a, rhs, vars_, unbound);
    }
    case FN_OP_IN: { /* x() in y() — y() is a list (or string) */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *y = do_eval(a, rhs, vars_, unbound); if (*unbound) return NULL;
        if (y->kind == EVV_LIST) {
            for (int i = 0; i < y->list.len; i++) {
                const EvVal *it = y->list.data[i];
                if (it->kind == EVV_STR && x->kind == EVV_STR &&
                    strcmp(it->s, x->s) == 0) return vbool(a, true);
            }
            return vbool(a, false);
        }
        /* substring containment for str rhs */
        return vbool(a, strstr(as_str(y), as_str(x)) != NULL);
    }
    case FN_OP_COMMA: { /* [x(), y()] */
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *y = do_eval(a, rhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *l = vlist(a);
        vec_push(l->list, x); vec_push(l->list, y);
        return l;
    }
    case FN_OP_NPLUS: case FN_OP_NSUB: case FN_OP_NMUL: case FN_OP_NDIV: {
        EvVal *x = do_eval(a, lhs, vars_, unbound); if (*unbound) return NULL;
        EvVal *y = do_eval(a, rhs, vars_, unbound); if (*unbound) return NULL;
        Number nx = num_make_str(as_str(x));
        Number ny = num_make_str(as_str(y));
        if (op == FN_OP_NDIV) {
            /* Number.__floordiv__: either None -> Number("");
             * other.value 0 -> returns Python None -> str(None)="None";
             * else true-division float -> Number(str(float)) -> None -> "" */
            if (nx.none || ny.none) return vstr(a, "");
            if (ny.v == 0) return vstr(a, "None");
            return vstr(a, ""); /* float repr never parses back to int */
        }
        if (nx.none || ny.none) return vstr(a, num_str(a, (Number){true, 0}));
        Number r; r.none = false;
        if (op == FN_OP_NPLUS) r.v = nx.v + ny.v;
        else if (op == FN_OP_NSUB) r.v = nx.v - ny.v;
        else r.v = nx.v * ny.v;
        return vstr(a, num_str(a, r));
    }
    default:
        return vstr(a, "");
    }
}

/* Evaluator.eval */
static EvVal *do_eval(Arena *a, Ev *self, const HashMap *vars_, bool *unbound) {
    int n = self->expr.len;

    if (n == 1) {
        const EvSlot *s0 = &self->expr.data[0];
        if (s0->kind == ES_TRUE) return vbool(a, true);
        if (s0->kind == ES_LIST) {
            /* val is not str -> return val (raw list) */
            EvVal *lv = vlist(a);
            if (s0->list)
                for (int i = 0; i < s0->list->list.len; i++) {
                    PNode *c = s0->list->list.data[i];
                    vec_push(lv->list,
                             c->kind == PN_STR ? vstr(a, c->str) : vstr(a, ""));
                }
            return lv;
        }
        if (s0->kind == ES_EV) {
            /* len-1 holding a wrapped Evaluator does not occur (Python
             * keeps the raw element); evaluate defensively. */
            return do_eval(a, s0->ev, vars_, unbound);
        }
        /* str atom */
        const char *val = s0->str;
        if (strcmp(val, "$") == 0) return vstr(a, "$");
        if (!re_svar_match(val)) return vstr(a, val);
        if (!vars_ || !hashmap_has(vars_, val)) { *unbound = true; return NULL; }
        return vstr(a, (const char *)hashmap_get(vars_, val));
    }

    if (n == 2) {
        FN op;
        if (self->expr.data[0].kind != ES_STR ||
            !fn_lookup(self->expr.data[0].str, &op) || !fn_is_unary(op)) {
            *unbound = true; /* ValueError path — propagate as failure */
            return NULL;
        }
        EvVal *operand = slot_eval(a, &self->expr.data[1], vars_, unbound);
        if (*unbound) return NULL;
        EvVal *raw = apply_unary(a, op, operand, vars_, unbound);
        if (*unbound) return NULL;
        return normalize(a, raw);
    }

    if (n == 3 && !(self->expr.data[1].kind == ES_STR &&
                    strcmp(self->expr.data[1].str, ",") == 0)) {
        FN op;
        if (self->expr.data[1].kind != ES_STR ||
            !fn_lookup(self->expr.data[1].str, &op) || !fn_is_binary(op)) {
            *unbound = true;
            return NULL;
        }
        Ev *lhs = self->expr.data[0].ev;
        Ev *rhs = self->expr.data[2].ev;
        EvVal *raw = apply_binary(a, op, lhs, rhs, vars_, unbound);
        if (*unbound) return NULL;
        return normalize(a, raw);
    }

    /* It's a list: [x.eval() for i,x in enumerate(expr) if not i%2] */
    EvVal *l = vlist(a);
    for (int i = 0; i < n; i++) {
        if (i % 2) continue;
        EvVal *ev = slot_eval(a, &self->expr.data[i], vars_, unbound);
        if (*unbound) return NULL;
        vec_push(l->list, ev);
    }
    return l;
}

EvVal *ev_eval(Arena *a, Ev *e, const HashMap *vars_, bool *unbound) {
    bool ub = false;
    EvVal *r = do_eval(a, e, vars_, &ub);
    if (unbound) *unbound = ub;
    return ub ? NULL : r;
}

void ev_helpers_init(void) { z80h_helpers_init(); }
