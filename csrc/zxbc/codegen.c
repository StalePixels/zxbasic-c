/*
 * Codegen driver. See codegen.h. Faithful transcription of zxbc.py main()
 * (the no-parse-only, output_file_type==ASM path) for the S5.2 slice.
 *
 * Byte-fidelity strategy: every step is a faithful transcription of the
 * Python algorithm over the same data, so C output() ≡ Python output()
 * by construction; the M7 gate (`cmp -s` C vs Python, same host) is the
 * definitive oracle.
 */
#include "codegen.h"
#include "backend.h"
#include "translator.h"
#include "errmsg.h"
#include "peephole/engine.h"

#include <stdio.h>
#include <string.h>

/* Python str.split("\n"): empty segments preserved; "a\n" -> ["a",""]. */
static StrVec split_nl(Arena *a, const char *s) {
    StrVec v; vec_init(v);
    const char *start = s;
    for (;;) {
        const char *nl = strchr(start, '\n');
        size_t seg = nl ? (size_t)(nl - start) : strlen(start);
        char *piece = arena_alloc(a, seg + 1);
        memcpy(piece, start, seg);
        piece[seg] = '\0';
        vec_push(v, piece);
        if (!nl) break;
        start = nl + 1;
    }
    return v;
}

/* "sep".join(list) into the arena. */
static char *join_sep(Arena *a, StrVec v, const char *sep) {
    size_t sl = strlen(sep), total = 1;
    for (int i = 0; i < v.len; i++) total += strlen(v.data[i]) + (i ? sl : 0);
    char *out = arena_alloc(a, total);
    size_t w = 0;
    for (int i = 0; i < v.len; i++) {
        if (i) { memcpy(out + w, sep, sl); w += sl; }
        size_t l = strlen(v.data[i]);
        memcpy(out + w, v.data[i], l); w += l;
    }
    out[w] = '\0';
    return out;
}

/* RE_PRAGMA = ^#[ \t]?pragma[ \t]opt[ \t]  (optimizer/patterns.py:39) */
static bool re_pragma(const char *s) {
    const char *p = s;
    if (*p != '#') return false;
    p++;
    if (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "pragma", 6) != 0) return false;
    p += 6;
    if (*p != ' ' && *p != '\t') return false;
    p++;
    if (strncmp(p, "opt", 3) != 0) return false;
    p += 3;
    return (*p == ' ' || *p == '\t');
}

/* RE_LABEL = ^[ \t]*[_a-zA-Z][a-zA-Z\d]*[ \t]*:  (optimizer/patterns.py:27)
 * Returns the match end (index past ':') or -1; *group_len = length of the
 * matched text incl. ':'. NOTE the repeat class is [a-zA-Z\d] (no '_'). */
static int re_label_end(const char *s, int *group_len) {
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    char c = s[i];
    if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        return -1;
    i++;
    while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
           (s[i] >= '0' && s[i] <= '9'))
        i++;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] != ':') return -1;
    *group_len = i + 1;
    return i + 1;
}

/* str.rstrip(chars) for chars = "\r\n\t " */
static char *rstrip_ws(Arena *a, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n-1];
        if (c=='\r'||c=='\n'||c=='\t'||c==' ') n--; else break;
    }
    char *r = arena_alloc(a, n + 1);
    memcpy(r, s, n); r[n] = '\0';
    return r;
}

/* _cleanup_mem (optimizer/main.py:86-99): split a "LABEL: insn" element
 * into "LABEL:" + "insn". Identity for the calib asm (its .core.* labels
 * start with '.' so RE_LABEL never matches); ported faithfully for S5.x. */
static StrVec cleanup_mem(Arena *a, StrVec mem) {
    StrVec out; vec_init(out);
    for (int i = 0; i < mem.len; i++) {
        const char *tmp = mem.data[i];
        int glen;
        int end = re_label_end(tmp, &glen);
        if (end >= 0) {
            char *rstr = rstrip_ws(a, tmp);
            char *grp = arena_strndup(a, tmp, (size_t)glen);
            if (strcmp(rstr, grp) != 0) {
                /* insert "LABEL:" then the remainder.strip() */
                char *lab = arena_strndup(a, tmp, (size_t)glen - 1); /* drop ':' */
                /* match.group()[:-1].strip() + ":" */
                const char *ls = lab; size_t ll = strlen(lab);
                while (*ls==' '||*ls=='\t') ls++;
                while (ll>0 && (ls[ll-1]==' '||ls[ll-1]=='\t')) ll--;
                char *labc = arena_alloc(a, ll + 2);
                memcpy(labc, ls, ll); labc[ll]=':'; labc[ll+1]='\0';
                vec_push(out, labc);
                const char *rem = tmp + end;
                while (*rem==' '||*rem=='\t'||*rem=='\r'||*rem=='\n') rem++;
                char *remc = rstrip_ws(a, rem);
                vec_push(out, remc);
                continue;
            }
        }
        vec_push(out, (char *)tmp);
    }
    return out;
}

/* Optimizer().optimize(mem) at OPTIONS.optimization_level <= 2
 * (optimizer/main.py:194-200): _cleanup_mem then
 * "\n".join(x for x in mem if not RE_PRAGMA.match(x)). */
static char *optimizer_optimize_le2(Arena *a, StrVec mem) {
    StrVec cm = cleanup_mem(a, mem);
    StrVec keep; vec_init(keep);
    for (int i = 0; i < cm.len; i++)
        if (!re_pragma(cm.data[i])) vec_push(keep, cm.data[i]);
    char *res = join_sep(a, keep, "\n");
    vec_free(cm); vec_free(keep);
    return res;
}

/* output() (zxbc.py:45-68): #-lines pass through; non-':' lines get a
 * leading '\t'; every element written + '\n' (element may contain '\n'). */
static void emit_output(FILE *f, StrVec mem, Arena *a) {
    for (int i = 0; i < mem.len; i++) {
        char *m = rstrip_ws(a, mem.data[i]);
        if (m[0] != '\0' && m[0] == '#') {
            fprintf(f, "%s\n", m);
            continue;
        }
        if (m[0] != '\0' && strchr(m, ':') == NULL)
            fputc('\t', f);
        fprintf(f, "%s\n", m);
    }
}

int codegen_emit(CompilerState *cs, AstNode *ast) {
    Arena *a = &cs->arena;

    /* backend.init() (zxbc.py:91-93): common.init + MEMORY clear +
     * engine.main() (peephole load). */
    Backend b;
    backend_init(&b, a);
    b.org = cs->opts.org;
    b.headerless = cs->opts.headerless;
    b.autorun = cs->opts.autorun;
    b.opt_level = cs->opts.optimization_level;
    peephole_init(a);
    peephole_main();

    /* 6  translator.visit(ast)  (zxbc.py:125-126) */
    Translator tr; tr.cs = cs; tr.backend = &b;
    translator_visit(&tr, ast);

    /* 7  FunctionTranslator(...).start()  (zxbc.py:132-133)
     * No functions in the calibration program -> genuine no-op (the
     * Python loop iterates gl.FUNCTIONS, empty here). Real in S5.5+. */

    /* 8  emit_data_blocks / emit_strings / emit_jump_tables
     * (zxbc.py:144-148): no DATA/strings/jumptables -> no-op. Real S5.8. */

    /* 8  translator.ic_inline(";; --- end of user code ---") (zxbc.py:150) */
    translator_ic_inline(&tr, ";; --- end of user code ---");

    /* 9  asm_output = backend.emit(optimize = O > 0)  (zxbc.py:171) */
    StrVec asm_inner = backend_emit(&b, cs->opts.optimization_level > 0);

    /* 10 asm_output = Optimizer().optimize(asm_output) + "\n" (zxbc.py:172) */
    char *joined = optimizer_optimize_le2(a, asm_inner);
    vec_free(asm_inner);
    size_t jl = strlen(joined);
    char *asm_str = arena_alloc(a, jl + 2);
    memcpy(asm_str, joined, jl); asm_str[jl] = '\n'; asm_str[jl+1] = '\0';

    /* 10b ASMS expansion (zxbc.py:174-180): split, replace ##ASMn with
     * "\n".join(body), join, (re-filter identity), split. */
    StrVec lines = split_nl(a, asm_str);
    for (int i = 0; i < lines.len; i++) {
        AsmsBody *bd = hashmap_get(&b.asms, lines.data[i]);
        if (bd != NULL) {
            StrVec body; vec_init(body);
            for (int k = 0; k < bd->n; k++) vec_push(body, bd->lines[k]);
            lines.data[i] = join_sep(a, body, "\n");
            vec_free(body);
        }
    }
    char *rejoined = join_sep(a, lines, "\n");
    vec_free(lines);

    /* 11 zxbpp ASM-mode re-filter + get_inits (zxbc.py:183-191):
     * identity for directive-free asm (calib has no '#' directives;
     * REQUIRES empty). Full re-filter is S7 (runtime #include) scope. */
    StrVec asm_lines = split_nl(a, rejoined);

    /* 12 VarTranslator over data_ast (zxbc.py:195-203): C has no data_ast
     * (deferred S5.3, recorded); the calib var is unused @O2 so this
     * contributes nothing -> tmp = []. */
    StrVec tmp; vec_init(tmp);

    /* 13 final = emit_prologue() + tmp + [DATA_END:, MAIN:] + asm + epilogue
     * (zxbc.py:204-210) */
    StrVec prologue = backend_emit_prologue(&b);
    StrVec epilogue = backend_emit_epilogue(&b);

    StrVec final; vec_init(final);
    for (int i = 0; i < prologue.len; i++) vec_push(final, prologue.data[i]);
    for (int i = 0; i < tmp.len; i++)      vec_push(final, tmp.data[i]);
    {
        char *de = arena_alloc(a, strlen(LBL_DATA_END) + 2);
        sprintf(de, "%s:", LBL_DATA_END);
        char *ml = arena_alloc(a, strlen(LBL_MAIN) + 2);
        sprintf(ml, "%s:", LBL_MAIN);
        vec_push(final, de);
        vec_push(final, ml);
    }
    for (int i = 0; i < asm_lines.len; i++) vec_push(final, asm_lines.data[i]);
    for (int i = 0; i < epilogue.len; i++)  vec_push(final, epilogue.data[i]);

    /* 14 output(asm_output, output_file)  (zxbc.py:212-214) */
    int rc = 0;
    FILE *of = fopen(cs->opts.output_filename, "wb");
    if (!of) {
        zxbc_error(cs, 0, "cannot open output file '%s'",
                   cs->opts.output_filename);
        rc = 1;
    } else {
        emit_output(of, final, a);
        fclose(of);
    }

    vec_free(prologue); vec_free(epilogue); vec_free(asm_lines);
    vec_free(tmp); vec_free(final);
    backend_free(&b);
    return rc;
}
