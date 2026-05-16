/*
 * template.c — see template.h. Faithful port of template.py.
 */
#include "template.h"

#include <ctype.h>
#include <string.h>

static int t_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

static char *t_strip(Arena *a, const char *s) {
    const char *b = s;
    while (*b && t_ws((unsigned char)*b)) b++;
    const char *e = s + strlen(s);
    while (e > b && t_ws((unsigned char)e[-1])) e--;
    return arena_strndup(a, b, (size_t)(e - b));
}

LineTemplate *line_template_new(Arena *a, const char *line) {
    LineTemplate *t = (LineTemplate *)arena_alloc(a, sizeof(LineTemplate));
    t->base = line_pattern_new(a, line); /* BasicLinePattern.__init__ */
    return t;
}

/* LineTemplate.filter:
 *   for tok in self.output:
 *     if len(tok) > 1 and tok[0] == '$':
 *        val = vars_.get(tok); if val is None: raise UnboundVarError
 *        result += val
 *     else: result += tok
 *   return result.strip()
 */
char *line_template_filter(Arena *a, const LineTemplate *t,
                           const HashMap *vars_, bool *unbound) {
    const PStrVec *out = &t->base->output;
    size_t cap = 1;
    for (int i = 0; i < out->len; i++) {
        const char *tok = out->data[i];
        if (strlen(tok) > 1 && tok[0] == '$') {
            const char *v = vars_ ? (const char *)hashmap_get(vars_, tok) : NULL;
            cap += v ? strlen(v) : 0;
        } else {
            cap += strlen(tok);
        }
    }
    char *buf = (char *)arena_alloc(a, cap);
    size_t bl = 0;
    for (int i = 0; i < out->len; i++) {
        const char *tok = out->data[i];
        if (strlen(tok) > 1 && tok[0] == '$') {
            const char *v = vars_ ? (const char *)hashmap_get(vars_, tok) : NULL;
            if (v == NULL) { *unbound = true; return NULL; }
            size_t vl = strlen(v);
            memcpy(buf + bl, v, vl); bl += vl;
        } else {
            size_t tl = strlen(tok);
            memcpy(buf + bl, tok, tl); bl += tl;
        }
    }
    buf[bl] = '\0';
    return t_strip(a, buf);
}

BlockTemplate *block_template_new(Arena *a, const char *const *lines, int n) {
    BlockTemplate *bt = (BlockTemplate *)arena_alloc(a, sizeof(BlockTemplate));
    vec_init(bt->templates);
    /* lines = [x.strip() for x in lines]; templates = [LineTemplate(x)
     * for x in lines if x] */
    for (int i = 0; i < n; i++) {
        char *st = t_strip(a, lines[i]);
        if (st[0] == '\0') continue;
        vec_push(bt->templates, line_template_new(a, st));
    }
    return bt;
}

void block_template_filter(Arena *a, const BlockTemplate *bt,
                           const HashMap *vars_, TplStrVec *out,
                           bool *unbound) {
    for (int i = 0; i < bt->templates.len; i++) {
        char *r = line_template_filter(a, bt->templates.data[i], vars_, unbound);
        if (*unbound) return;
        if (r && r[0] != '\0') vec_push(*out, r); /* if y */
    }
}
