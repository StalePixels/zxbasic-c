/*
 * pattern.c — see pattern.h. Faithful port of pattern.py.
 */
#include "pattern.h"

#include <ctype.h>
#include <string.h>

/* Python re's \s : [ \t\n\r\f\v]  (ASCII; the .opt data is ASCII). */
static int re_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

static char *p_strndup(Arena *a, const char *s, size_t n) {
    return arena_strndup(a, s, n);
}
static char *p_strdup(Arena *a, const char *s) {
    return arena_strdup(a, s ? s : "");
}

/* Python str.strip() — ASCII whitespace both ends. */
static char *py_strip(Arena *a, const char *s) {
    const char *b = s;
    while (*b && re_ws((unsigned char)*b)) b++;
    const char *e = s + strlen(s);
    while (e > b && re_ws((unsigned char)e[-1])) e--;
    return p_strndup(a, b, (size_t)(e - b));
}

/* --------------------------------------------------------------------
 * RE_PARSE = re.compile(r'(\s+|"(?:[^"]|"")*")')  — re.split semantics
 * with one capturing group: returns [text, sep, text, sep, ..., text].
 * Separators never match empty (both alternatives require >=1 char).
 * ------------------------------------------------------------------ */

/* Try to match RE_PARSE at s+pos; on success set *mlen to match length. */
static bool re_parse_at(const char *s, size_t pos, size_t len, size_t *mlen) {
    char c = s[pos];
    if (re_ws((unsigned char)c)) {
        size_t i = pos;
        while (i < len && re_ws((unsigned char)s[i])) i++;
        *mlen = i - pos;
        return true;
    }
    if (c == '"') {
        size_t i = pos + 1;
        for (;;) {
            if (i >= len) return false;          /* unterminated -> no match */
            if (s[i] == '"') {
                if (i + 1 < len && s[i + 1] == '"') { i += 2; continue; } /* "" */
                /* closing quote */
                *mlen = (i + 1) - pos;
                return true;
            }
            i++;                                  /* [^"] */
        }
    }
    return false;
}

/* re.split — append alternating text/sep pieces (each arena-owned). */
static void re_parse_split(Arena *a, const char *s, PStrVec *out) {
    size_t len = strlen(s);
    size_t pos = 0, text_start = 0;
    while (pos < len) {
        size_t mlen = 0;
        if (re_parse_at(s, pos, len, &mlen) && mlen > 0) {
            /* preceding text */
            vec_push(*out, p_strndup(a, s + text_start, pos - text_start));
            /* the captured separator */
            vec_push(*out, p_strndup(a, s + pos, mlen));
            pos += mlen;
            text_start = pos;
        } else {
            pos++;
        }
    }
    vec_push(*out, p_strndup(a, s + text_start, len - text_start));
}

/* RE_SVAR = re.compile(r"(\$(?:\$|[0-9]+))")
 * - re_svar_match(tok): tok matches ^\$(\$|[0-9]+) at start AND the WHOLE
 *   token is exactly that (used as RE_SVAR.match(tok) where tok is a
 *   subtoken produced by RE_SVAR.split — so it's either "$$" or "$<num>"
 *   exactly). We treat it as: tok=="$$" OR (tok[0]=='$' && rest all digits
 *   && len>=2). */
static bool re_svar_full(const char *tok) {
    if (strcmp(tok, "$$") == 0) return true;
    if (tok[0] != '$' || tok[1] == '\0') return false;
    for (const char *p = tok + 1; *p; p++)
        if (!isdigit((unsigned char)*p)) return false;
    return true;
}

/* RE_SVAR.split(token): split on (\$(?:\$|[0-9]+)); capturing group keeps
 * the separators. Returns [text, svar, text, svar, ..., text]. */
static void re_svar_split(Arena *a, const char *s, PStrVec *out) {
    size_t len = strlen(s), pos = 0, text_start = 0;
    while (pos < len) {
        if (s[pos] == '$') {
            size_t mlen = 0;
            if (pos + 1 < len && s[pos + 1] == '$') {
                mlen = 2;
            } else if (pos + 1 < len && isdigit((unsigned char)s[pos + 1])) {
                size_t i = pos + 1;
                while (i < len && isdigit((unsigned char)s[i])) i++;
                mlen = i - pos;
            }
            if (mlen > 0) {
                vec_push(*out, p_strndup(a, s + text_start, pos - text_start));
                vec_push(*out, p_strndup(a, s + pos, mlen));
                pos += mlen;
                text_start = pos;
                continue;
            }
        }
        pos++;
    }
    vec_push(*out, p_strndup(a, s + text_start, len - text_start));
}

/* Append literal chars of `tok` as PE_LIT elements + record into output.
 * sanitize() escapes metachars for a real regex; our matcher compares the
 * literal char directly, so escaping is a no-op (same matched language). */
static void emit_literal(LinePattern *lp, const char *tok) {
    for (const char *p = tok; *p; p++) {
        PatElem e; e.kind = PE_LIT; e.ch = *p; e.var = NULL; e.ref = 0;
        vec_push(lp->elems, e);
    }
}

static bool pstr_contains(const PStrVec *v, const char *s) {
    for (int i = 0; i < v->len; i++)
        if (strcmp(v->data[i], s) == 0) return true;
    return false;
}
static int pstr_index(const PStrVec *v, const char *s) {
    for (int i = 0; i < v->len; i++)
        if (strcmp(v->data[i], s) == 0) return i;
    return -1;
}

LinePattern *line_pattern_new(Arena *a, const char *line0) {
    LinePattern *lp = (LinePattern *)arena_alloc(a, sizeof(LinePattern));
    vec_init(lp->output);
    vec_init(lp->elems);
    vec_init(lp->vars);
    vec_init(lp->group_order);

    /* self.line = "".join(x.strip() or " " for x in RE_PARSE.split(line)
     *                      if x).strip() */
    PStrVec parts; vec_init(parts);
    re_parse_split(a, line0, &parts);
    {
        size_t cap = 1;
        for (int i = 0; i < parts.len; i++) cap += strlen(parts.data[i]) + 1;
        char *buf = (char *)arena_alloc(a, cap);
        size_t bl = 0;
        for (int i = 0; i < parts.len; i++) {
            const char *x = parts.data[i];
            if (x[0] == '\0') continue;            /* `if x` */
            char *st = py_strip(a, x);
            const char *use = st[0] ? st : " ";    /* x.strip() or " " */
            size_t ul = strlen(use);
            memcpy(buf + bl, use, ul); bl += ul;
        }
        buf[bl] = '\0';
        lp->line = py_strip(a, buf);               /* trailing .strip() */
    }
    vec_free(parts);

    /* for token in RE_PARSE.split(self.line): */
    PStrVec toks; vec_init(toks);
    re_parse_split(a, lp->line, &toks);
    for (int ti = 0; ti < toks.len; ti++) {
        const char *token = toks.data[ti];
        if (strcmp(token, " ") == 0) {
            PatElem e; e.kind = PE_WS; e.ch = 0; e.var = NULL; e.ref = 0;
            vec_push(lp->elems, e);
            vec_push(lp->output, p_strdup(a, " "));
            continue;
        }
        /* subtokens = [x for x in RE_SVAR.split(token) if x] */
        PStrVec subs; vec_init(subs);
        re_svar_split(a, token, &subs);
        for (int si = 0; si < subs.len; si++) {
            const char *tok = subs.data[si];
            if (tok[0] == '\0') continue;          /* `if x` */
            if (strcmp(tok, "$$") == 0) {
                PatElem e; e.kind = PE_LIT; e.ch = '$'; e.var = NULL; e.ref = 0;
                vec_push(lp->elems, e);
                vec_push(lp->output, p_strdup(a, "$"));
            } else if (re_svar_full(tok)) {
                vec_push(lp->output, p_strdup(a, tok));      /* "$N" */
                /* mvar = "_%s" % tok[1:] */
                size_t tl = strlen(tok);
                char *mvar = (char *)arena_alloc(a, tl + 1);
                mvar[0] = '_';
                memcpy(mvar + 1, tok + 1, tl - 1);
                mvar[tl] = '\0';
                if (!pstr_contains(&lp->group_order, mvar)) {
                    vec_push(lp->group_order, mvar);
                    PatElem e; e.kind = PE_GROUP; e.ch = 0;
                    e.var = mvar; e.ref = 0;
                    vec_push(lp->elems, e);
                } else {
                    int idx = pstr_index(&lp->group_order, mvar);
                    PatElem e; e.kind = PE_BACKREF; e.ch = 0;
                    e.var = NULL; e.ref = idx + 1;
                    vec_push(lp->elems, e);
                }
            } else {
                vec_push(lp->output, p_strdup(a, tok));
                emit_literal(lp, tok);
            }
        }
        vec_free(subs);
    }
    vec_free(toks);

    /* self.vars = {x.replace("_", "$") for x in self.vars(group_order)} */
    for (int i = 0; i < lp->group_order.len; i++) {
        const char *gv = lp->group_order.data[i]; /* "_N" */
        size_t gl = strlen(gv);
        char *dv = (char *)arena_alloc(a, gl + 1);
        for (size_t j = 0; j < gl; j++) dv[j] = (gv[j] == '_') ? '$' : gv[j];
        dv[gl] = '\0';
        if (!pstr_contains(&lp->vars, dv)) vec_push(lp->vars, dv);
    }
    return lp;
}

/* --------------------------------------------------------------------
 * Backtracking matcher: re.match (start-anchored, end UNanchored).
 * Greedy '.*' groups; '.' = any char except '\n'. Captures into `caps`
 * keyed by group var ("_N"); backrefs compare against an earlier group's
 * captured text.
 * ------------------------------------------------------------------ */
typedef struct {
    const char *var;   /* "_N" */
    const char *start; /* into the subject string */
    size_t      len;
} Cap;
typedef VEC(Cap) CapVec;

static const char *cap_get(const CapVec *caps, const char *var, size_t *out_len) {
    for (int i = caps->len - 1; i >= 0; i--) {
        if (strcmp(caps->data[i].var, var) == 0) {
            *out_len = caps->data[i].len;
            return caps->data[i].start;
        }
    }
    *out_len = 0;
    return NULL;
}

/* Recursive match of elems[ei..] against subj[si..end]. Returns true on
 * full element-list consumption (subject may have trailing chars). */
static bool m_rec(const PatElemVec *elems, int ei,
                  const char *subj, const char *p, const char *end,
                  CapVec *caps) {
    if (ei == elems->len) return true; /* re.match: end NOT anchored */

    const PatElem *e = &elems->data[ei];
    switch (e->kind) {
    case PE_LIT:
        if (p < end && *p == e->ch)
            return m_rec(elems, ei + 1, subj, p + 1, end, caps);
        return false;
    case PE_WS: {
        /* \s+ : at least one, then greedy with backtracking */
        const char *q = p;
        while (q < end && re_ws((unsigned char)*q)) q++;
        if (q == p) return false;       /* need >=1 */
        for (const char *t = q; t >= p + 1; t--) {
            if (m_rec(elems, ei + 1, subj, t, end, caps)) return true;
        }
        return false;
    }
    case PE_GROUP: {
        /* (?P<var>.*) greedy: '.' != '\n'. Try longest first. */
        const char *q = p;
        while (q < end && *q != '\n') q++;
        for (const char *t = q; ; t--) {
            Cap c; c.var = e->var; c.start = p; c.len = (size_t)(t - p);
            vec_push(*caps, c);
            if (m_rec(elems, ei + 1, subj, t, end, caps)) {
                /* keep capture */
                return true;
            }
            caps->len--; /* pop trial capture */
            if (t == p) break;
        }
        return false;
    }
    case PE_BACKREF: {
        /* find the ref-th GROUP's var by scanning declaration order */
        int seen = 0; const char *bvar = NULL;
        for (int i = 0; i < elems->len; i++) {
            if (elems->data[i].kind == PE_GROUP) {
                seen++;
                if (seen == e->ref) { bvar = elems->data[i].var; break; }
            }
        }
        if (!bvar) return false;
        size_t blen = 0;
        const char *bs = cap_get(caps, bvar, &blen);
        if (!bs) return false;
        if ((size_t)(end - p) < blen) return false;
        if (memcmp(p, bs, blen) != 0) return false;
        return m_rec(elems, ei + 1, subj, p + blen, end, caps);
    }
    }
    return false;
}

bool line_pattern_match(Arena *a, const LinePattern *lp,
                        const char *line, HashMap *vars_) {
    const char *end = line + strlen(line);
    CapVec caps; vec_init(caps);
    if (!m_rec(&lp->elems, 0, line, line, end, &caps)) {
        vec_free(caps);
        return false;
    }

    /* mdict = match.groupdict() — last value wins per group name (Python
     * groupdict returns the last group of that name). Build mdict. */
    HashMap mdict; hashmap_init(&mdict);
    for (int i = 0; i < lp->group_order.len; i++) {
        const char *var = lp->group_order.data[i]; /* "_N" */
        size_t glen = 0;
        const char *gs = cap_get(&caps, var, &glen);
        char *val = gs ? p_strndup(a, gs, glen) : NULL;
        hashmap_set(&mdict, var, val);
    }
    vec_free(caps);

    /* if any(mdict.get(k, v) != v for k, v in vars_.items()): return False
     * vars_ keys here are "_N" (univars). */
    bool conflict = false;
    for (int i = 0; i < vars_->capacity; i++) {
        if (!vars_->entries[i].occupied) continue;
        const char *k = vars_->entries[i].key;
        const char *v = (const char *)vars_->entries[i].value;
        const char *mv = hashmap_has(&mdict, k)
                         ? (const char *)hashmap_get(&mdict, k)
                         : v; /* mdict.get(k, v) */
        const char *vv = v ? v : "";
        const char *mvv = mv ? mv : "";
        if (strcmp(mvv, vv) != 0) { conflict = true; break; }
    }
    if (conflict) { hashmap_free(&mdict); return false; }

    /* vars_.update(mdict) */
    for (int i = 0; i < mdict.capacity; i++) {
        if (!mdict.entries[i].occupied) continue;
        hashmap_set(vars_, mdict.entries[i].key, mdict.entries[i].value);
    }
    hashmap_free(&mdict);
    return true;
}

/* --------------------------------------------------------------------
 * BlockPattern
 * ------------------------------------------------------------------ */
BlockPattern *block_pattern_new(Arena *a, const char *const *lines, int n) {
    BlockPattern *bp = (BlockPattern *)arena_alloc(a, sizeof(BlockPattern));
    vec_init(bp->patterns);
    vec_init(bp->lines);
    vec_init(bp->vars);

    /* lines = [x.strip() for x in lines]
     * self.patterns = [LinePattern(x) for x in lines if x] */
    for (int i = 0; i < n; i++) {
        char *st = py_strip(a, lines[i]);
        if (st[0] == '\0') continue;
        LinePattern *lp = line_pattern_new(a, st);
        vec_push(bp->patterns, lp);
        vec_push(bp->lines, lp->line);
        for (int j = 0; j < lp->vars.len; j++) {
            if (!pstr_contains(&bp->vars, lp->vars.data[j]))
                vec_push(bp->vars, lp->vars.data[j]);
        }
    }
    return bp;
}

HashMap *block_pattern_match(Arena *a, const BlockPattern *bp,
                             const char *const *instructions, int n_instr,
                             int start) {
    /* lines = instructions[start:] ; if len(self) > len(lines): None */
    int avail = n_instr - start;
    if (avail < 0) avail = 0;
    if (block_pattern_len(bp) > avail) return NULL;

    HashMap *univars = (HashMap *)arena_alloc(a, sizeof(HashMap));
    hashmap_init(univars);

    /* all(patt.match(line, vars_=univars) for patt,line in zip(patterns,
     * lines)) — zip stops at the shorter; patterns is <= avail here. */
    for (int i = 0; i < bp->patterns.len; i++) {
        const char *line = instructions[start + i];
        if (!line_pattern_match(a, bp->patterns.data[i], line, univars)) {
            hashmap_free(univars);
            return NULL;
        }
    }

    /* return {"$" + k[1:]: v for k, v in univars.items()} */
    HashMap *res = (HashMap *)arena_alloc(a, sizeof(HashMap));
    hashmap_init(res);
    for (int i = 0; i < univars->capacity; i++) {
        if (!univars->entries[i].occupied) continue;
        const char *k = univars->entries[i].key; /* "_N" */
        size_t kl = strlen(k);
        char *nk = (char *)arena_alloc(a, kl + 1);
        nk[0] = '$';
        memcpy(nk + 1, k + 1, kl - 1);
        nk[kl] = '\0';
        hashmap_set(res, nk, univars->entries[i].value);
    }
    hashmap_free(univars);
    return res;
}
