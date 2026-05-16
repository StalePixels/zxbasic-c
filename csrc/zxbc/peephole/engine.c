/*
 * engine.c — see engine.h. Faithful port of peephole/engine.py.
 *
 * read_opt / read_opts (over the embedded table, mirroring a directory
 * listing) / init / main (MAXLEN cache) / apply_match. The loader sorts
 * survivors by the parsed OFLAG integer with a STABLE sort (Python
 * `sorted(result, key=lambda x: x.flag)`), and MAXLEN is
 * `max(len(pattern.patt), MAXLEN or 0)` over ALL loaded patterns.
 */
#include "engine.h"
#include "opts_embedded.h"
#include "parser.h"
#include "pattern.h"
#include "template.h"
#include "evaluator.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

/* OptPattern (NamedTuple) */
typedef struct OptPattern {
    int            level;   /* O_LEVEL */
    int            flag;    /* O_FLAG  */
    BlockPattern  *patt;
    Ev            *cond;
    BlockTemplate *templ;
    PeepholeParsed *parsed;
    const PeepDefVec *defines;
    const char    *fname;
} OptPattern;

typedef VEC(OptPattern) OptVec;

/* Module state (Python module globals PATTERNS / MAXLEN). */
static Arena  *g_arena = NULL;
static OptVec  g_patterns;
static int     g_maxlen = 0;
static bool    g_inited = false;

void peephole_init(Arena *arena) {
    g_arena = arena;
    if (!g_inited) { vec_init(g_patterns); g_inited = true; }
    /* engine.init(): MAXLEN=0, PATTERNS.clear() */
    g_maxlen = 0;
    vec_clear(g_patterns);
}

/* read_opt(content, fname) -> OptPattern or "not added". MAXLEN updated
 * on success. Returns true and fills *out on success. */
static bool read_opt(const char *content, const char *fname, OptPattern *out) {
    Arena *a = g_arena;
    PeepholeParsed *pr = peep_parse_str(a, content);
    if (pr == NULL) return false; /* parse_file None -> read_opt None */

    /* try: OptPattern(...); except (ValueError,KeyError,TypeError): warn */
    /* BlockPattern(REPLACE), BlockTemplate(WITH), Evaluator(IF) */
    const char **rep = (const char **)pr->replace.data;
    const char **wit = (const char **)pr->with_.data;
    BlockPattern  *patt  = block_pattern_new(a, rep, pr->replace.len);
    BlockTemplate *templ = block_template_new(a, wit, pr->with_.len);

    /* Evaluator(parsed_result[REG_IF]) — empty IF -> [] -> [True] */
    PNode *iftree = pr->if_tree;
    if (iftree == NULL) { iftree = pnode_list(a); } /* defensive: -> [True] */
    bool verr = false;
    Ev *cond = ev_new(a, iftree, &verr);
    if (verr || cond == NULL) {
        fprintf(stderr, "%s:1: warning: There is an error in this template "
                        "and it will be ignored\n", fname);
        return false;
    }

    /* for var_, define_ in defines: if var_ in patt.vars: warn x2; None */
    for (int i = 0; i < pr->defines.len; i++) {
        const char *var_ = pr->defines.data[i].var;
        for (int j = 0; j < patt->vars.len; j++) {
            if (strcmp(patt->vars.data[j], var_) == 0) {
                fprintf(stderr, "%s:%d: warning: variable '%s' already "
                        "defined in pattern\n", fname,
                        pr->defines.data[i].lineno, var_);
                fprintf(stderr, "%s:%d: warning: this template will be "
                        "ignored\n", fname, pr->defines.data[i].lineno);
                return false;
            }
        }
    }

    out->level   = pr->olevel;
    out->flag    = pr->oflag;
    out->patt    = patt;
    out->cond    = cond;
    out->templ   = templ;
    out->parsed  = pr;
    out->defines = &pr->defines;
    out->fname   = fname;

    /* MAXLEN = max(len(pattern_.patt), MAXLEN or 0) */
    int pl = block_pattern_len(patt);
    if (pl > g_maxlen) g_maxlen = pl;
    return true;
}

/* read_opts: iterate the embedded table (== os.listdir filtered to .opt,
 * here in sorted-filename order), read_opt each, drop None, then a
 * STABLE sort by .flag. */
static void read_opts(void) {
    for (int i = 0; i < PEEP_EMBEDDED_OPTS_COUNT; i++) {
        OptPattern op;
        memset(&op, 0, sizeof(op));
        if (read_opt(PEEP_EMBEDDED_OPTS[i].content,
                     PEEP_EMBEDDED_OPTS[i].name, &op)) {
            vec_push(g_patterns, op);
        }
    }
    /* stable sort by flag (insertion sort: stable, n=52) */
    for (int i = 1; i < g_patterns.len; i++) {
        OptPattern key = g_patterns.data[i];
        int j = i - 1;
        while (j >= 0 && g_patterns.data[j].flag > key.flag) {
            g_patterns.data[j + 1] = g_patterns.data[j];
            j--;
        }
        g_patterns.data[j + 1] = key;
    }
}

void peephole_main(void) {
    /* if not force and MAXLEN: return  (cache) */
    if (g_maxlen) return;
    assert(g_arena && "peephole_init() must be called first");
    /* init() */
    g_maxlen = 0;
    vec_clear(g_patterns);
    read_opts();
}

void peephole_main_force(void) {
    assert(g_arena && "peephole_init() must be called first");
    g_maxlen = 0;
    vec_clear(g_patterns);
    read_opts();
}

int peephole_maxlen(void) { return g_maxlen; }
int peephole_pattern_count(void) { return g_patterns.len; }
int peephole_pattern_level(int idx) { return g_patterns.data[idx].level; }
int peephole_pattern_pattlen(int idx) {
    return block_pattern_len(g_patterns.data[idx].patt);
}
const char *peephole_pattern_fname(int idx) {
    return g_patterns.data[idx].fname;
}

/* StrVec splice: replace [index, index+cnt) with `repl` (TplStrVec). */
static void strvec_splice(StrVec *v, int index, int cnt,
                          const TplStrVec *repl) {
    int new_len = v->len - cnt + repl->len;
    if (new_len > v->cap) {
        int nc = v->cap ? v->cap : 8;
        while (nc < new_len) nc *= 2;
        v->data = (char **)realloc(v->data, (size_t)nc * sizeof(char *));
        if (!v->data) { fprintf(stderr, "peephole: OOM\n"); exit(1); }
        v->cap = nc;
    }
    /* shift tail */
    int tail_from = index + cnt;
    int tail_to   = index + repl->len;
    int tail_n    = v->len - tail_from;
    if (tail_to != tail_from && tail_n > 0)
        memmove(v->data + tail_to, v->data + tail_from,
                (size_t)tail_n * sizeof(char *));
    for (int i = 0; i < repl->len; i++)
        v->data[index + i] = repl->data[i];
    v->len = new_len;
}

/* engine.apply_match over the level-filtered pattern list. */
bool peephole_apply_match(StrVec *asm_, int level_cap, int index) {
    Arena *a = g_arena;
    for (int pi = 0; pi < g_patterns.len; pi++) {
        OptPattern *p = &g_patterns.data[pi];
        if (p->level > level_cap) continue; /* level filter (caller's min) */

        HashMap *match = block_pattern_match(
            a, p->patt, (const char *const *)asm_->data, asm_->len, index);
        if (match == NULL) continue; /* {} is a valid match (non-NULL) */

        /* for var, defline in p.defines: match[var]=defline.expr.eval(match)
         * Python stores the raw eval value; every shipped DEFINE evaluates
         * to a string and the only consumer (cond.eval atom lookup ->
         * as_str) is byte-identical to storing evval_str(result). */
        bool unbound = false;
        for (int di = 0; di < p->defines->len; di++) {
            const PeepDefine *d = &p->defines->data[di];
            EvVal *dv = ev_eval(a, d->expr, match, &unbound);
            if (unbound) break;
            hashmap_set(match, d->var, evval_str(a, dv));
        }
        if (unbound) {
            hashmap_free(match);
            continue; /* UnboundVarError in a DEFINE: Python would raise;
                       * no shipped pattern reaches this — treat as no-fire
                       * (documented; never observed for valid asm). */
        }

        /* if not p.cond.eval(match): continue */
        unbound = false;
        EvVal *cv = ev_eval(a, p->cond, match, &unbound);
        if (unbound || !evval_truthy(cv)) { hashmap_free(match); continue; }

        /* applied = p.template.filter(match)
         * asm_list[index:index+len(p.patt)] = applied */
        TplStrVec applied; vec_init(applied);
        bool tub = false;
        block_template_filter(a, p->templ, match, &applied, &tub);
        if (tub) { /* UnboundVarError in WITH — Python raises; unreached */
            vec_free(applied); hashmap_free(match); continue;
        }
        int cnt = block_pattern_len(p->patt);
        strvec_splice(asm_, index, cnt, &applied);
        vec_free(applied);
        hashmap_free(match);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------- */
/* Internal smoke — no tests/ dependency. Asserts the S5.2 028-on-_end
 * transform and the two NEEDS facts. Returns 0 on success. */
#include "memcell.h"
#include "z80asm.h"

int peephole_selfcheck(void) {
    Arena a;
    arena_init(&a, 0);
    Arena *save = g_arena;
    OptVec save_p = g_patterns;
    int save_ml = g_maxlen;
    bool save_in = g_inited;

    g_inited = false;
    peephole_init(&a);
    peephole_main_force();

    int rc = 0;

    /* NEEDS("exx",["sp","iy"]) and NEEDS("exx",["sp","ix"]) -> false */
    {
        MemCell *m = memcell_new(&a, "exx", 1);
        Z80StrList rl; vec_init(rl);
        vec_push(rl, arena_strdup(&a, "sp"));
        vec_push(rl, arena_strdup(&a, "iy"));
        if (memcell_needs(&a, m, &rl)) rc = 1;
        vec_clear(rl);
        vec_push(rl, arena_strdup(&a, "sp"));
        vec_push(rl, arena_strdup(&a, "ix"));
        if (memcell_needs(&a, m, &rl)) rc = 2;
        vec_free(rl);
    }

    /* 028_o2 on the _end emitter sequence, level_cap=2. Fire repeatedly
     * (mirrors _output_join's per-index while-loop) and count fires. */
    {
        StrVec asmv; vec_init(asmv);
        const char *seq[] = {"exx","pop hl","exx","pop iy","pop ix"};
        for (int i = 0; i < 5; i++) vec_push(asmv, arena_strdup(&a, seq[i]));

        int fires = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < asmv.len; i++) {
                if (peephole_apply_match(&asmv, 2, i)) {
                    fires++; changed = true; break;
                }
            }
        }
        const char *want[] = {"exx","pop hl","pop iy","pop ix","exx"};
        if (fires != 2) rc = rc ? rc : 3;
        if (asmv.len != 5) rc = rc ? rc : 4;
        else for (int i = 0; i < 5; i++)
            if (strcmp(asmv.data[i], want[i]) != 0) { rc = rc ? rc : 5; break; }
        vec_free(asmv);
    }

    /* restore module state */
    vec_free(g_patterns);
    g_arena = save; g_patterns = save_p; g_maxlen = save_ml; g_inited = save_in;
    arena_destroy(&a);
    return rc;
}
