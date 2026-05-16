/*
 * parser.c — see parser.h. Faithful port of peephole/parser.py.
 */
#include "parser.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>

/* parse_str warnings (Python errmsg.warning(lineno, msg)). Not byte-
 * critical: no shipped .opt triggers them; the engine only sees the
 * NULL/struct result. Kept for diagnosability. */
static void pp_warn(int lineno, const char *msg) {
    fprintf(stderr, ":%d: warning: %s\n", lineno, msg);
}

static int pp_ws(int c){return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v';}

static char *pp_strip(Arena *a, const char *s) {
    const char *b=s; while(*b&&pp_ws((unsigned char)*b))b++;
    const char *e=s+strlen(s); while(e>b&&pp_ws((unsigned char)e[-1]))e--;
    return arena_strndup(a,b,(size_t)(e-b));
}
static char *pp_strndup(Arena *a, const char *s, size_t n){return arena_strndup(a,s,n);}
static char *pp_strdup(Arena *a, const char *s){return arena_strdup(a,s?s:"");}

/* -------------------------------------------------------------------- */
/* RE_SVAR.match  (pattern.RE_SVAR) — start-anchored ^\$(\$|[0-9]+) */
static bool re_svar_match(const char *v) {
    if (v[0] != '$') return false;
    if (v[1] == '$') return true;
    return isdigit((unsigned char)v[1]) != 0;
}

/* RE_INT = ^\d+$ */
static bool re_int_full(const char *s) {
    if (!*s) return false;
    for (const char *p = s; *p; p++) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

/* RE_REGION = r"([_a-zA-Z][a-zA-Z0-9]*)[ \t]*\{\{$"  (re.match) */
static bool re_region(Arena *a, const char *line, char **name) {
    const char *p = line;
    if (!(*p == '_' || isalpha((unsigned char)*p))) return false;
    const char *ns = p; p++;
    /* [a-zA-Z0-9]* — underscores NOT allowed after the first char */
    while (isalnum((unsigned char)*p)) p++;
    const char *ne = p;
    while (*p == ' ' || *p == '\t') p++;
    if (!(p[0] == '{' && p[1] == '{' && p[2] == '\0')) return false;
    *name = pp_strndup(a, ns, (size_t)(ne - ns));
    return true;
}

/* RE_DEF = r"([_a-zA-Z][a-zA-Z0-9]*)[ \t]*:[ \t]*(.*)"  (re.match) */
static bool re_def(Arena *a, const char *line, char **key, char **val) {
    const char *p = line;
    if (!(*p == '_' || isalpha((unsigned char)*p))) return false;
    const char *ks = p; p++;
    while (isalnum((unsigned char)*p)) p++;
    const char *ke = p;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *key = pp_strndup(a, ks, (size_t)(ke - ks));
    *val = pp_strdup(a, p); /* (.*) — to end of (already-stripped) line */
    return true;
}

/* -------------------------------------------------------------------- */
/* IF_OPERATORS precedence (lower number == higher priority). Returns -1
 * if op not in IF_OPERATORS. */
static int if_prec(const char *op) {
    struct { const char *o; int p; } T[] = {
        {".*",3},{"./",3},{"+",5},{".+",5},{".-",5},{"<>",10},{"==",10},
        {"&&",15},{"||",20},{"IN",25},{",",30}
    };
    for (size_t i = 0; i < sizeof(T)/sizeof(T[0]); i++)
        if (strcmp(op, T[i].o) == 0) return T[i].p;
    return -1;
}

/* -------------------------------------------------------------------- */
/* Tokenizer (RE_IFPARSE / RE_ID / OPERS). */
typedef struct { const char *src; int lineno; } Tok;

/* RE_ID = \b[_a-zA-Z]+\b ; .match (prefix). Succeeds iff t starts with
 * [_a-zA-Z] AND the char after the maximal [_a-zA-Z]+ run is not a word
 * char (the only possibility being a digit, since letters/underscore are
 * consumed). So "INSTR" -> true, "OP1" -> false. */
static bool re_id_match(const char *t) {
    if (!(*t == '_' || isalpha((unsigned char)*t))) return false;
    size_t i = 0;
    while (t[i] == '_' || isalpha((unsigned char)t[i])) i++;
    return !isdigit((unsigned char)t[i]);
}

/* RE_IFPARSE = '"(""|[^"])*"|[(),]|\b[_a-zA-Z]+\b|[^," \t()]+'
 * Returns the matched token length at start of s, or 0 if no match. */
static size_t re_ifparse(const char *s, size_t *mlen) {
    if (*s == '"') {
        size_t i = 1;
        for (;;) {
            if (s[i] == '\0') return 0;        /* unterminated -> no match */
            if (s[i] == '"') {
                if (s[i+1] == '"') { i += 2; continue; } /* "" */
                *mlen = i + 1; return 1;
            }
            i++;                               /* [^"] */
        }
    }
    if (*s == '(' || *s == ')' || *s == ',') { *mlen = 1; return 1; }
    /* alt 3: \b[_a-zA-Z]+\b — the trailing \b requires the char AFTER the
     * maximal [_a-zA-Z]+ run to be a non-word char (or EOL). Since the run
     * already consumed every [_a-zA-Z], the only way the trailing \b fails
     * is a following digit (word char) -> alt 3 fails, fall to alt 4
     * (so "OP1" tokenises whole, not "OP"). */
    if (*s == '_' || isalpha((unsigned char)*s)) {
        size_t i = 0;
        while (s[i] == '_' || isalpha((unsigned char)s[i])) i++;
        if (!isdigit((unsigned char)s[i])) { *mlen = i; return 1; }
        /* else: trailing \b fails -> try alt 4 below */
    }
    /* [^," \t()]+ */
    {
        size_t i = 0;
        while (s[i] && s[i] != ',' && s[i] != ' ' && s[i] != '\t' &&
               s[i] != '(' && s[i] != ')') i++;
        if (i == 0) return 0;
        *mlen = i; return 1;
    }
}

/* OPERS prefix list (BINARY ∪ UNARY names) for the tok-trim step. The
 * only relevant prefixes for an alt-4 run are the symbolic operators;
 * none is a prefix of another for a single run (verified). */
static const char *OPER_NAMES[] = {
    "!","+","==","<>","&&","||","IN",",",".+",".-",".*","./",
    "IS_ASM","IS_INDIR","IS_REG16","IS_REG8","IS_LABEL","IS_IMMED",
    "LEN","INSTR","HIREG","LOREG","HIVAL","LOVAL","GVAL","IS_REQUIRED",
    "CTEST","NEEDS","FLAGVAL","OP1","OP2"
};

/* lookahead(): strips src, returns token (and trims to oper if needed). */
static bool tok_lookahead(Arena *a, Tok *t, char **out) {
    /* self.source = self.source.strip() */
    t->src = pp_strip(a, t->src);
    if (*t->src == '\0') { *out = pp_strdup(a, ""); return true; }
    size_t mlen = 0;
    if (!re_ifparse(t->src, &mlen)) {
        pp_warn(t->lineno, "Syntax error in line");
        return false; /* PeepholeParserSyntaxError */
    }
    char *tok = pp_strndup(a, t->src, mlen);
    if (!re_id_match(tok)) {
        for (size_t i = 0; i < sizeof(OPER_NAMES)/sizeof(OPER_NAMES[0]); i++) {
            size_t ol = strlen(OPER_NAMES[i]);
            if (strncmp(tok, OPER_NAMES[i], ol) == 0) {
                tok = pp_strndup(a, tok, ol);
                break;
            }
        }
    }
    *out = tok;
    return true;
}

/* get_token(): tok = lookahead(); src = src[len(tok):] (after strip). */
static bool tok_get(Arena *a, Tok *t, char **out) {
    char *tok;
    if (!tok_lookahead(a, t, &tok)) return false;
    /* lookahead already stripped t->src */
    size_t tl = strlen(tok);
    t->src = t->src + tl;
    *out = tok;
    return true;
}
static bool tok_finished(Tok *t) { return t->src[0] == '\0'; }

/* -------------------------------------------------------------------- */
/* PNode list helpers reflecting Python list ops on `expr`/`stack`. */
static PNode *pn_last(PNode *l) {
    return l->list.len ? l->list.data[l->list.len - 1] : NULL;
}
static bool pn_is_str_eq(const PNode *n, const char *s) {
    return n && n->kind == PN_STR && strcmp(n->str, s) == 0;
}

/* simplify_expr */
static PNode *simplify_expr(Arena *a, PNode *expr) {
    if (expr->kind != PN_LIST) return expr;
    if (expr->list.len == 1 && expr->list.data[0]->kind == PN_LIST)
        return simplify_expr(a, expr->list.data[0]);
    PNode *r = pnode_list(a);
    for (int i = 0; i < expr->list.len; i++)
        vec_push(r->list, simplify_expr(a, expr->list.data[i]));
    return r;
}

PNode *peep_parse_ifline(Arena *a, const char *if_line, int lineno,
                         bool *error, bool *empty) {
    *error = false;
    if (empty) *empty = false;

    typedef VEC(PNode *) Stack;
    Stack stack; vec_init(stack);
    PNode *expr = pnode_list(a);
    int paren = 0;
    Tok tk; tk.src = pp_strdup(a, if_line); tk.lineno = lineno;

    while (!tok_finished(&tk)) {
        char *tok;
        if (!tok_get(a, &tk, &tok)) { *error = true; vec_free(stack); return NULL; }
        if (tok[0] == '\0') break; /* "" == EOL */

        if (strcmp(tok, "(") == 0) {
            paren += 1;
            vec_push(stack, expr);
            expr = pnode_list(a);
            continue;
        }
        if (fn_is_unary_name(tok)) {
            vec_push(stack, expr);
            expr = pnode_list(a);
            vec_push(expr->list, pnode_str(a, tok));
            continue;
        }
        if (strcmp(tok, ")") == 0) {
            paren -= 1;
            if (paren < 0) { pp_warn(lineno, "Too much closed parenthesis");
                             *error = true; vec_free(stack); return NULL; }
            if (expr->list.len && pn_is_str_eq(pn_last(expr), ",")) {
                pp_warn(lineno, "missing element in list");
                *error = true; vec_free(stack); return NULL;
            }
            /* any(x != ',' for i,x in enumerate(expr) if i%2) */
            for (int i = 1; i < expr->list.len; i += 2) {
                if (!pn_is_str_eq(expr->list.data[i], ",")) {
                    pp_warn(lineno, "Invalid list");
                    *error = true; vec_free(stack); return NULL;
                }
            }
            if (stack.len == 0) { /* stack[-1] would IndexError */
                pp_warn(lineno, "unbalanced parenthesis");
                *error = true; vec_free(stack); return NULL;
            }
            vec_push(stack.data[stack.len - 1]->list, expr);
            expr = stack.data[stack.len - 1];
            stack.len--;
        } else {
            /* if len(tok)>1 and tok[0]==tok[-1]=='"': tok = tok[1:-1] */
            size_t tl = strlen(tok);
            if (tl > 1 && tok[0] == '"' && tok[tl - 1] == '"')
                tok = pp_strndup(a, tok + 1, tl - 2);
            vec_push(expr->list, pnode_str(a, tok));
        }

        /* if tok == ',' : */
        if (strcmp(tok, ",") == 0) {
            if (expr->list.len < 2 ||
                pn_is_str_eq(expr->list.data[expr->list.len - 2], ",")) {
                pp_warn(lineno, "Unexpected , in list");
                *error = true; vec_free(stack); return NULL;
            }
        }

        /* while len(expr)==2 and isinstance(expr[0],str): */
        while (expr->list.len == 2 && expr->list.data[0]->kind == PN_STR) {
            const char *op = expr->list.data[0]->str;
            if (fn_is_unary_name(op)) {
                if (stack.len == 0) {
                    pp_warn(lineno, "unbalanced parenthesis");
                    *error = true; vec_free(stack); return NULL;
                }
                vec_push(stack.data[stack.len - 1]->list, expr);
                expr = stack.data[stack.len - 1];
                stack.len--;
            } else {
                break;
            }
        }

        /* if len(expr)==3 and expr[1]!=',': */
        if (expr->list.len == 3 && !pn_is_str_eq(expr->list.data[1], ",")) {
            PNode *left_ = expr->list.data[0];
            PNode *opn   = expr->list.data[1];
            PNode *right_= expr->list.data[2];
            if (opn->kind != PN_STR || if_prec(opn->str) < 0) {
                pp_warn(lineno, "Unexpected binary operator");
                *error = true; vec_free(stack); return NULL;
            }
            const char *op = opn->str;
            if (left_->kind == PN_LIST && left_->list.len == 3) {
                PNode *op2n = left_->list.data[left_->list.len - 2];
                if (op2n->kind == PN_STR && if_prec(op2n->str) >= 0 &&
                    if_prec(op2n->str) > if_prec(op)) {
                    /* expr = [[left_[:-2], left_[-2], [left_[-1], op, right_]]] */
                    PNode *lhead = pnode_list(a);
                    for (int i = 0; i < left_->list.len - 2; i++)
                        vec_push(lhead->list, left_->list.data[i]);
                    PNode *rsub = pnode_list(a);
                    vec_push(rsub->list, left_->list.data[left_->list.len - 1]);
                    vec_push(rsub->list, pnode_str(a, op));
                    vec_push(rsub->list, right_);
                    PNode *inner = pnode_list(a);
                    vec_push(inner->list, lhead);
                    vec_push(inner->list, left_->list.data[left_->list.len - 2]);
                    vec_push(inner->list, rsub);
                    PNode *outer = pnode_list(a);
                    vec_push(outer->list, inner);
                    expr = outer;
                    continue;
                }
            }
            /* expr = [expr] */
            PNode *wrap = pnode_list(a);
            vec_push(wrap->list, expr);
            expr = wrap;
        }
    }

    if (paren) {
        pp_warn(lineno, "unclosed parenthesis in IF section");
        *error = true; vec_free(stack); return NULL;
    }

    while (stack.len > 0) {
        vec_push(stack.data[stack.len - 1]->list, expr);
        expr = stack.data[stack.len - 1];
        stack.len--;
        if (expr->list.len == 2) {
            PNode *opn = expr->list.data[0];
            if (opn->kind != PN_STR || !fn_is_unary_name(opn->str)) {
                pp_warn(lineno, "unexpected unary operator");
                *error = true; vec_free(stack); return NULL;
            }
        } else if (expr->list.len == 3) {
            PNode *opn = expr->list.data[1];
            if (opn->kind != PN_STR || !fn_is_binary_name(opn->str)) {
                pp_warn(lineno, "unexpected binary operator");
                *error = true; vec_free(stack); return NULL;
            }
        }
    }
    vec_free(stack);

    expr = simplify_expr(a, expr);

    if (expr->list.len == 2 && expr->list.data[1]->kind == PN_STR &&
        fn_is_binary_name(expr->list.data[1]->str)) {
        pp_warn(lineno, "Unexpected binary operator");
        *error = true; return NULL;
    }
    if (expr->list.len == 3) {
        PNode *m = expr->list.data[1];
        bool bad = (m->kind != PN_STR) || !fn_is_binary_name(m->str) ||
                   pn_is_str_eq(m, ",");
        if (bad) {
            pp_warn(lineno, "Unexpected binary operator");
            *error = true; return NULL;
        }
    }
    if (expr->list.len > 3) {
        pp_warn(lineno, "Lists not allowed in IF section condition");
        *error = true; return NULL;
    }

    if (empty && expr->list.len == 0) *empty = true;
    return expr;
}

/* -------------------------------------------------------------------- */
/* parse_define_line: "$nnn = <expr>" */
static bool parse_define_line(Arena *a, const char *line, int lineno,
                              char **var_out, PNode **expr_out) {
    const char *eq = strchr(line, '=');
    if (!eq) { pp_warn(lineno, "assignation '=' not found"); return false; }
    /* split("=", 1): result[0]=before, result[1]=after (first '=' only) */
    char *left = pp_strip(a, pp_strndup(a, line, (size_t)(eq - line)));
    char *right = pp_strip(a, eq + 1);
    if (!re_svar_match(left)) {
        pp_warn(lineno, "not a variable name");
        return false;
    }
    bool err = false, empty = false;
    PNode *rp = peep_parse_ifline(a, right, lineno, &err, &empty);
    if (err || rp == NULL) return false;
    *var_out = left;
    *expr_out = rp;
    return true;
}

/* -------------------------------------------------------------------- */
/* defaultdict(list) region accumulation. We only need the 6 known keys
 * + DEFINE source-lines list. Region content is kept as SourceLine
 * (lineno,line); the non-DEFINE regions later collapse to [.line]. */
typedef struct { int lineno; char *line; } SrcLine;
typedef VEC(SrcLine) SrcVec;

PeepholeParsed *peep_parse_str(Arena *a, const char *spec) {
    /* States */
    enum { ST_INITIAL = 0, ST_REGION = 1 } state = ST_INITIAL;

    SrcVec rep, wit, iff, def; vec_init(rep); vec_init(wit);
    vec_init(iff); vec_init(def);
    bool seen[6]; memset(seen, 0, sizeof(seen)); /* dup-def detection */
    /* indices: 0=REPLACE 1=WITH 2=IF 3=DEFINE 4=OLEVEL 5=OFLAG */
    int olevel = 0, oflag = 0; bool has_ol = false, has_of = false;
    int line_num = 0;
    char *region_name = NULL;
    int region_idx = -1;
    bool is_ok = true;

    /* iterate spec.split("\n") */
    const char *p = spec;
    while (is_ok) {
        const char *nl = strchr(p, '\n');
        const char *lend = nl ? nl : p + strlen(p);
        char *raw = pp_strndup(a, p, (size_t)(lend - p));
        char *line = pp_strip(a, raw);
        line_num += 1;

        if (line[0] != '\0') {
            if (state == ST_INITIAL) {
                if (strncmp(line, ";;", 2) == 0) {
                    goto next_line;
                }
                char *rn;
                if (re_region(a, line, &rn)) {
                    /* add_entry(region_name, []) */
                    char up[32];
                    size_t ul = strlen(rn);
                    for (size_t i = 0; i < ul && i < sizeof(up)-1; i++)
                        up[i] = (char)toupper((unsigned char)rn[i]);
                    up[ul < sizeof(up)-1 ? ul : sizeof(up)-1] = '\0';
                    int idx = -1;
                    if (!strcmp(up,"REPLACE")) idx=0;
                    else if (!strcmp(up,"WITH")) idx=1;
                    else if (!strcmp(up,"IF")) idx=2;
                    else if (!strcmp(up,"DEFINE")) idx=3;
                    if (idx < 0) {
                        /* not a REGION/SCALAR: add_entry -> unknown param */
                        pp_warn(line_num, "unknown definition parameter");
                        is_ok = false; break;
                    }
                    if (seen[idx]) {
                        pp_warn(line_num, "duplicated definition");
                        is_ok = false; break;
                    }
                    seen[idx] = true;
                    region_name = rn;
                    region_idx = idx;
                    state = ST_REGION;
                    goto next_line;
                }
                char *k, *v;
                if (re_def(a, line, &k, &v)) {
                    /* add_entry(key, value) */
                    char up[32];
                    size_t kl = strlen(k);
                    for (size_t i = 0; i < kl && i < sizeof(up)-1; i++)
                        up[i] = (char)toupper((unsigned char)k[i]);
                    up[kl < sizeof(up)-1 ? kl : sizeof(up)-1] = '\0';
                    int idx = -1;
                    if (!strcmp(up,"REPLACE")) idx=0;
                    else if (!strcmp(up,"WITH")) idx=1;
                    else if (!strcmp(up,"IF")) idx=2;
                    else if (!strcmp(up,"DEFINE")) idx=3;
                    else if (!strcmp(up,"OLEVEL")) idx=4;
                    else if (!strcmp(up,"OFLAG")) idx=5;
                    if (idx < 0) {
                        pp_warn(line_num, "unknown definition parameter");
                        is_ok = false; break;
                    }
                    if (seen[idx]) {
                        pp_warn(line_num, "duplicated definition");
                        is_ok = false; break;
                    }
                    if (idx == 4 || idx == 5) { /* NUMERIC: RE_INT, int() */
                        if (!re_int_full(v)) {
                            pp_warn(line_num, "field must be integer");
                            is_ok = false; break;
                        }
                        int iv = (int)strtol(v, NULL, 10);
                        if (idx == 4) { olevel = iv; has_ol = true; }
                        else          { oflag  = iv; has_of = true; }
                    }
                    seen[idx] = true;
                    goto next_line;
                }
                /* fell through: syntax error */
                pp_warn(line_num, "syntax error. Cannot parse file");
                is_ok = false; break;
            } else { /* ST_REGION */
                char *l2 = line;
                size_t l2l = strlen(l2);
                if (l2l >= 2 && strcmp(l2 + l2l - 2, "}}") == 0) {
                    l2 = pp_strip(a, pp_strndup(a, l2, l2l - 2));
                    state = ST_INITIAL;
                }
                if (l2[0] != '\0') {
                    SrcLine sl; sl.lineno = line_num; sl.line = l2;
                    SrcVec *tgt = NULL;
                    switch (region_idx) {
                    case 0: tgt = &rep; break;
                    case 1: tgt = &wit; break;
                    case 2: tgt = &iff; break;
                    case 3: tgt = &def; break;
                    default: tgt = NULL; break;
                    }
                    if (tgt) vec_push(*tgt, sl);
                }
                if (state == ST_INITIAL) { region_name = NULL; region_idx = -1; }
                goto next_line;
            }
            /* (ST_REGION already 'continue's; ST_INITIAL handled/branched) */
        }
    next_line:
        if (!nl) break;
        p = nl + 1;
    }
    (void)region_name;

    PeepholeParsed *res = (PeepholeParsed *)arena_alloc(a, sizeof(PeepholeParsed));
    vec_init(res->replace); vec_init(res->with_); vec_init(res->defines);
    res->if_tree = NULL; res->if_is_empty = false;
    res->has_olevel = has_ol; res->has_oflag = has_of;
    res->olevel = olevel; res->oflag = oflag;

    /* DEFINE region -> [ (var, Evaluator(expr), lineno) ] */
    if (is_ok) {
        /* defined_vars dedupe */
        ParStrVec defined; vec_init(defined);
        for (int i = 0; i < def.len; i++) {
            char *var_; PNode *exprp;
            if (!parse_define_line(a, def.data[i].line, def.data[i].lineno,
                                   &var_, &exprp)) {
                is_ok = false; break;
            }
            bool dup = false;
            for (int j = 0; j < defined.len; j++)
                if (strcmp(defined.data[j], var_) == 0) { dup = true; break; }
            if (dup) {
                pp_warn(def.data[i].lineno, "duplicated variable");
                is_ok = false; break;
            }
            bool verr = false;
            Ev *ev = ev_new(a, exprp, &verr);
            if (verr || !ev) {
                /* Evaluator(expr) raised -> read_opt try/except drops it.
                 * Surface as parse failure here (engine drops pattern). */
                is_ok = false; break;
            }
            PeepDefine pd; pd.var = var_; pd.expr = ev;
            pd.lineno = def.data[i].lineno;
            vec_push(res->defines, pd);
            vec_push(defined, var_);
        }
        vec_free(defined);
    }

    /* non-DEFINE regions -> [x.line] */
    if (is_ok) {
        for (int i = 0; i < rep.len; i++) vec_push(res->replace, rep.data[i].line);
        for (int i = 0; i < wit.len; i++) vec_push(res->with_, wit.data[i].line);
    }

    /* reg_if = parse_ifline(" ".join(x for x in result[REG_IF]), line_num) */
    if (is_ok) {
        size_t cap = 1;
        for (int i = 0; i < iff.len; i++) cap += strlen(iff.data[i].line) + 1;
        char *joined = (char *)arena_alloc(a, cap);
        size_t jl = 0;
        for (int i = 0; i < iff.len; i++) {
            if (i) joined[jl++] = ' ';
            size_t sl = strlen(iff.data[i].line);
            memcpy(joined + jl, iff.data[i].line, sl); jl += sl;
        }
        joined[jl] = '\0';
        bool err = false, empty = false;
        PNode *t = peep_parse_ifline(a, joined, line_num, &err, &empty);
        if (err || t == NULL) {
            is_ok = false;
        } else {
            res->if_tree = t;
            res->if_is_empty = empty;
        }
    }

    /* is_ok = is_ok and all(check_entry(x) for x in REQUIRED)
     * REQUIRED = (REPLACE, WITH, OLEVEL, OFLAG) */
    if (is_ok) {
        if (!seen[0]) { pp_warn(line_num,"undefined section REPLACE"); is_ok=false; }
        else if (!seen[1]) { pp_warn(line_num,"undefined section WITH"); is_ok=false; }
        else if (!has_ol) { pp_warn(line_num,"undefined section OLEVEL"); is_ok=false; }
        else if (!has_of) { pp_warn(line_num,"undefined section OFLAG"); is_ok=false; }
    }

    /* if not result[REG_REPLACE]: empty REPLACE region */
    if (is_ok && res->replace.len == 0) {
        pp_warn(line_num, "empty region REPLACE");
        is_ok = false;
    }

    vec_free(rep); vec_free(wit); vec_free(iff); vec_free(def);

    if (!is_ok) {
        pp_warn(line_num, "this optimizer template will be ignored due to errors");
        return NULL;
    }
    return res;
}
