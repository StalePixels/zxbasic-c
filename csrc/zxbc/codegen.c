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

/* S5.6 — locate the parser-built ARRAYDECL node whose child[0] is the
 * given array symbol-table entry. The parser emits one ARRAYDECL per
 * `DIM name(...)` (parser.c:2156-2172) with the entry id_node as child[0];
 * recursively search the program AST for it. Array-confined: only ever
 * called for CLASS_array entries from the array drain. */
static AstNode *codegen_find_arraydecl(AstNode *node, AstNode *entry) {
    if (node == NULL) return NULL;
    if (node->tag == AST_ARRAYDECL && node->child_count > 0 &&
        node->children[0] == entry)
        return node;
    for (int i = 0; i < node->child_count; i++) {
        AstNode *r = codegen_find_arraydecl(node->children[i], entry);
        if (r != NULL) return r;
    }
    return NULL;
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
    b.opt_strategy = (int)cs->opts.opt_strategy;
    peephole_init(a);
    peephole_main();

    /* ---- S5.3 (N1): build data_ast + seed the temp counter ------------
     *
     * data_ast: a BLOCK of VARDECL(entry) over the global CLASS_var
     * symbol-table entries in first-textual-appearance order — the
     * faithful analogue of p_start:543-545 + make_var_declaration:263-267
     * (sym.VARDECL(entry)) drained from SYMBOL_TABLE.vars_
     * (symboltable.py:776-782, filter_by_opt=False → ALL declared vars
     * regardless of O-level; the O2 unused-drop happens in VarTranslator,
     * not here). Built here (not in parser.c) to keep parser churn
     * minimal; the drain point is faithful (it reads the final symbol
     * table, exactly as p_start does after the whole program parsed).
     *
     * Temp-counter seed: Python's global optemps counter is bumped at
     * ref-object construction during parse + p_start (symbolref.py:25 —
     * every SymbolRef.__init__, incl. VarRef/LabelRef). For the S5.3
     * integer-scalar corpus the only entries are global scalars + the 2
     * fixed p_start namespace data labels (.ZXBASIC_USER_DATA / _LEN):
     *   per var/const entry      = ID(SymbolRef) + to_var/to_const = 2
     *   2 p_start ns data labels = (ID SymbolRef + to_label) ×2     = 4
     * so the pre-Translator counter = 2*N_var_const + 4. Verified live
     * against let0.bas / calib.bas / multi-var fixtures (the BINARY's .t
     * is t6 for let0, t8 for the 2-var case). User LABELs add +3 each
     * (non-namespace: extra symboltable.py:620 to_label) — out of the
     * S5.3 integer-scalar slice (no GOTO/labels in the S5.3 corpus);
     * extended faithfully when the label sprints land. */
    {
        int nvc = 0;
        AstNode *e;
        vec_foreach(cs->sym_entries_ordered, e) {
            if (e && (e->u.id.class_ == CLASS_var ||
                      e->u.id.class_ == CLASS_const))
                nvc++;
        }
        cs->temp_counter = 2 * nvc + 4;

        AstNode *dast = ast_new(cs, AST_BLOCK, 1);
        vec_foreach(cs->sym_entries_ordered, e) {
            /* SYMBOL_TABLE.vars_ : global scope, class_ == CLASS.var
             * (symboltable.py:776-782). make_var_declaration = VARDECL
             * with the entry as its sole child (vardecl.py:18-20). */
            if (!e || e->u.id.class_ != CLASS_var ||
                e->u.id.scope != SCOPE_global)
                continue;
            AstNode *vd = ast_new(cs, AST_VARDECL, e->lineno);
            ast_add_child(cs, vd, e);
            vd->type_ = e->type_;
            ast_add_child(cs, dast, vd);
        }

        /* S5.6 — array declarations second pass (zxbparser.py:549-551):
         *   for var in SYMBOL_TABLE.arrays:
         *       data_ast.append_child(make_array_declaration(var))
         * make_array_declaration(entry) == sym.ARRAYDECL(entry)
         * (zxbparser.py:270-272). SYMBOL_TABLE.arrays is the scope-
         * insertion-order list of CLASS.array entries
         * (symboltable.py:794-802); sym_entries_ordered mirrors that
         * single insert point. The geometry (bounds / init / addr) lives
         * on the parser-built ARRAYDECL node whose child[0] *is* this
         * entry (parser.c:2166-2172) — we relink that node into data_ast
         * (the faithful analogue of Python rebuilding ARRAYDECL(entry)
         * from the entry's ArrayRef). This pass is array-confined: it
         * touches only CLASS_array global entries and never alters the
         * scalar VARDECL drain above. */
        vec_foreach(cs->sym_entries_ordered, e) {
            if (!e || e->u.id.class_ != CLASS_array ||
                e->u.id.scope != SCOPE_global)
                continue;
            AstNode *adecl = codegen_find_arraydecl(cs->ast, e);
            if (adecl != NULL)
                ast_add_child(cs, dast, adecl);
        }
        cs->data_ast = dast;
    }

    /* 6  translator.visit(ast)  (zxbc.py:125-126) */
    Translator tr; tr.cs = cs; tr.backend = &b;
    translator_visit(&tr, ast);

    /* 7  FunctionTranslator(...).start()  (zxbc.py:132-133)
     * S5.7a core: FIFO-drain the pending-function queue filled by the
     * deferred Translator.visit_FUNCDECL during step 6, emitting each
     * function's label/ic_enter/body/__leave/ic_leave. Mirrors Python's
     * order exactly (Translator.visit then FunctionTranslator.start).
     * gl.DATA_IS_USED -> gl.FUNCTIONS.extend(gl.DATA_FUNCTIONS)
     * (zxbc.py:128-129) is the DATA-funcptr path: out of the S5.7a core
     * slice (no DATA in scope). No-op when no functions were declared. */
    translator_function_start(&tr);

    /* 8  emit_data_blocks / emit_strings (zxbc.py:144-146): no DATA/
     * strings -> no-op. Real S5.8. */

    /* 8  translator.emit_jump_tables() (zxbc.py:148): drains the ON
     * GOTO/GOSUB JUMP_TABLES (S5.5). No-op when none appeared. */
    translator_emit_jump_tables(&tr);

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

    /* 12 VarTranslator over data_ast (zxbc.py:192-203):
     *   backend.MEMORY[:] = []                      (192)
     *   VariableVisitor().visit(data_ast)           (195-196)
     *   VarTranslator(backend).visit(data_ast)      (197-198)
     *   tmp = [x for x in backend.emit(optimize=False)
     *          if x.strip()[0] != "#"]              (203)
     *
     * VariableVisitor (optimize.py:497/546-556) is an error-only pass: it
     * checks aliasing-var circular dependencies and yields the node
     * unchanged — no accessed/IC side-effects. The S5.3 integer-scalar
     * corpus has no aliased vars, so it is a faithful no-op here (the
     * alias-dependency graph is out of the integer-scalar slice). */
    StrVec tmp; vec_init(tmp);
    {
        /* backend.MEMORY[:] = [] — main asm already emitted (step 9); the
         * data path emits into a fresh MEMORY. */
        vec_free(b.memory);
        vec_init(b.memory);
        /* common.FLAG_end_emitted is NOT reset (Python only resets it in
         * common.init); the data path emits no `end` quad so it has no
         * observable effect either way — left unchanged for fidelity. */

        var_translator_visit(&tr, cs->data_ast);

        if (cs->error_count > 0) {
            /* zxbc.py:199-201: gl.has_errors -> return 1 */
            vec_free(tmp);
            backend_free(&b);
            return 1;
        }

        StrVec data_emit = backend_emit(&b, false);
        for (int i = 0; i < data_emit.len; i++) {
            const char *x = data_emit.data[i];
            const char *p = x;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
            /* Python x.strip()[0] != "#": a stripped-empty line would
             * IndexError in Python — the data emit never yields one
             * (vard/var/varx always emit "LABEL:"/"DEFB ..."); keep
             * non-'#' lines (the '#include once <...>' REQUIRES lines,
             * which start with '#', are dropped here as in Python). */
            if (*p == '#') continue;
            vec_push(tmp, (char *)x);
        }
        vec_free(data_emit);
    }

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
