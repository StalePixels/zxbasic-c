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
#include "optimizer/optimizer.h"
#include "asm_bridge.h"
#include "zxbpp.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
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

/* OptLe2Fn adapter: optimizer_optimize() calls this for the verbatim
 * O<=2 early-return (main.py:199-200). Z80StrList and StrVec are both
 * `VEC(char*)` (same {data,len,cap} layout); this views the Z80StrList
 * as a StrVec and runs the UNCHANGED, byte-proven O<=2 routine above —
 * the inertness guarantee (the -O2 codegen meter output is identical to
 * HEAD because this exact code still produces it). */
static char *codegen_le2_adapter(Arena *a, Z80StrList mem) {
    StrVec sv;
    sv.data = mem.data;
    sv.len  = mem.len;
    sv.cap  = mem.cap;
    return optimizer_optimize_le2(a, sv);
}

/* output() (zxbc.py:45-68): #-lines pass through; non-':' lines get a
 * leading '\t'; every element written + '\n' (element may contain '\n').
 *
 * emit_output_str renders `mem` to a single arena-allocated NUL-terminated
 * string that is byte-identical to the bytes emit_output would fwrite.
 * emit_output is now a thin fwrite wrapper over it so the asm-text path
 * stays byte-for-byte unchanged. */
static char *emit_output_str(StrVec mem, Arena *a) {
    StrBuf sb;
    strbuf_init(&sb);
    for (int i = 0; i < mem.len; i++) {
        char *m = rstrip_ws(a, mem.data[i]);
        if (m[0] != '\0' && m[0] == '#') {
            strbuf_append(&sb, m);
            strbuf_append_char(&sb, '\n');
            continue;
        }
        if (m[0] != '\0' && strchr(m, ':') == NULL)
            strbuf_append_char(&sb, '\t');
        strbuf_append(&sb, m);
        strbuf_append_char(&sb, '\n');
    }
    const char *cs = strbuf_cstr(&sb);
    size_t n = strlen(cs);
    char *out = arena_alloc(a, n + 1);
    memcpy(out, cs, n + 1);
    strbuf_free(&sb);
    return out;
}

static void emit_output(FILE *f, StrVec mem, Arena *a) {
    const char *s = emit_output_str(mem, a);
    fwrite(s, 1, strlen(s), f);
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

/* RE_INIT (zxbc.py:28-29):
 *   re.compile(
 *     r'^#[ \t]*init[ \t]+'
 *     r'((?:[._a-zA-Z][._a-zA-Z0-9]*)|(?:"[._a-zA-Z][._a-zA-Z0-9]*"))'
 *     r'[ \t]*$', re.IGNORECASE)
 *
 * Python `RE_INIT.match(m)` anchors `^...$` over one whole asm line (no
 * embedded newline — `memory` is the post-filter asm vector split on
 * "\n"). `re.IGNORECASE` only has an observable effect on the literal
 * `init` (the NAME class [._a-zA-Z][._a-zA-Z0-9]* already spans both
 * cases), so we match `init`/`INIT`/`Init`/... case-insensitively and
 * everything else exactly. The capture group is one of two alternatives:
 * an unquoted NAME, or a double-quoted NAME; the caller applies Python's
 * `.strip('"')` (strips leading/trailing '"', only present when the
 * quoted alternative matched).
 *
 * On match: returns 1 and writes the captured name (WITHOUT surrounding
 * quotes — mirroring `.strip('"')`) into `out` (cap-bounded). On no
 * match returns 0 and leaves `out` untouched. Over-match guards: the
 * `[ \t]+` after `init` is mandatory (so `#initialize`, `#init` directly
 * followed by a non-space, etc. do NOT match), the line must end exactly
 * after the trailing `[ \t]*` (the `$` anchor — nothing else allowed),
 * and the NAME must start with one of [._a-zA-Z]. */
static int re_init_name(char c) {
    return c == '.' || c == '_' ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int re_init_name_cont(char c) {
    return re_init_name(c) || (c >= '0' && c <= '9');
}
static int re_init(const char *s, char *out, size_t cap) {
    int i = 0;
    /* ^# */
    if (s[i] != '#') return 0;
    i++;
    /* [ \t]* */
    while (s[i] == ' ' || s[i] == '\t') i++;
    /* init  (re.IGNORECASE: only this literal has letters) */
    {
        static const char kw[] = "init";
        for (int k = 0; k < 4; k++) {
            char c = s[i + k];
            char lc = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            if (lc != kw[k]) return 0;
        }
        i += 4;
    }
    /* [ \t]+  (mandatory — over-match guard for #initialize / #init<x>) */
    if (!(s[i] == ' ' || s[i] == '\t')) return 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    /* ( NAME | "NAME" ) */
    int quoted = 0;
    if (s[i] == '"') { quoted = 1; i++; }
    int name_start = i;
    if (!re_init_name(s[i])) return 0;
    i++;
    while (re_init_name_cont(s[i])) i++;
    int name_end = i;
    if (quoted) {
        if (s[i] != '"') return 0;
        i++;
    }
    /* [ \t]* */
    while (s[i] == ' ' || s[i] == '\t') i++;
    /* $  (end of line — nothing else; mirrors Python `$` on a
     * newline-free single line) */
    if (s[i] != '\0') return 0;

    /* `.strip('"')`: the capture group is name_start..name_end; quotes
     * (if the quoted alternative matched) are outside [name_start,
     * name_end) by construction, so the slice is already quote-free. */
    size_t n = (size_t)(name_end - name_start);
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, s + name_start, n);
    out[n] = '\0';
    return 1;
}

/* get_inits (zxbc.py:33-42):
 *
 *   def get_inits(memory):
 *       arch.target.backend.INITS.union(zxbparser.INITS)
 *       i = 0
 *       for m in memory:
 *           init = RE_INIT.match(m)
 *           if init is not None:
 *               arch.target.backend.INITS.add(init.groups()[0].strip('"'))
 *               memory[i] = ""
 *           i += 1
 *
 * Line 34 `arch.target.backend.INITS.union(zxbparser.INITS)`:
 * `arch.target.backend.INITS` is `src/arch/z80/backend/common.py:118`
 * `INITS: set[str] = set()`; `zxbparser.INITS` is `zxbparser.py:75
 * INITS = gl.INITS` => `src/api/global_.py:104 INITS: set[str] = set()`
 * — two DISTINCT plain `set` objects. Built-in `set.union(other)`
 * RETURNS A NEW SET and DOES NOT mutate the receiver; the result here is
 * not assigned, so line 34 is a pure NO-OP (the parser-collected inits,
 * `gl.INITS` / our `cs->inits`, are never read into the emitted output —
 * `emit_prologue` (backend/main.py:643,674) consumes ONLY `common.INITS`
 * = our `b->inits`). Mirrored faithfully as a no-op (cs->inits is left
 * untouched and is NOT merged into b->inits — doing so would diverge
 * from the Python).
 *
 * Lines 36-42: scan the post-filter asm vector; for every RE_INIT match
 * add the quote-stripped name to backend.INITS (our `b->inits`, the set
 * `emit_prologue` reads) and blank that asm element (`memory[i] = ""`).
 * Non-matching lines are left untouched. */
static void get_inits(Arena *a, Backend *b, StrVec *memory) {
    /* zxbc.py:34 — see header: built-in set.union returns a discarded
     * new set; no mutation. Faithful no-op. */
    char name[256];
    for (int i = 0; i < memory->len; i++) {
        if (re_init(memory->data[i], name, sizeof name)) {
            /* backend.INITS.add(name) — b->inits is a string-keyed set */
            hashmap_set(&b->inits, arena_strdup(a, name), (void *)1);
            /* memory[i] = "" */
            memory->data[i] = arena_strdup(a, "");
        }
    }
}

int codegen_emit(CompilerState *cs, AstNode *ast) {
    Arena *a = &cs->arena;

    /* backend.init() (zxbc.py:91-93): common.init + MEMORY clear +
     * engine.main() (peephole load). */
    Backend b;
    backend_init(&b, a);
    b.org = cs->opts.org;
    b.heap_size = cs->opts.heap_size;
    b.heap_address = cs->opts.heap_address;
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

    /* 7  zxbc.py:128-133  if gl.DATA_IS_USED:
     *          gl.FUNCTIONS.extend(gl.DATA_FUNCTIONS)
     *      FunctionTranslator(...).start()
     * S5.7a core: FIFO-drain the pending-function queue filled by the
     * deferred Translator.visit_FUNCDECL during step 6, emitting each
     * function's label/ic_enter/body/__leave/ic_leave. Mirrors Python's
     * order exactly (Translator.visit then FunctionTranslator.start).
     * S5.8d: the gl.FUNCTIONS.extend(gl.DATA_FUNCTIONS) merge
     * (zxbc.py:128-129) is now LIVE — performed at the top of
     * translator_function_start (the faithful "extend then start"
     * site), so the __DATA__FUNCPTR__N thunks drain like ordinary
     * pending functions. No-op when no functions/DATA-funcptrs exist. */
    translator_function_start(&tr);

    /* 8  translator.emit_data_blocks() (zxbc.py:144). S5.8d: LIVE.
     * Emits the .DATA.__DATA__N blocks (type byte + value/funcptr ptr),
     * the missing-RESTORE-label bare labels, and the __DATA__END
     * sentinel. MUST run BEFORE translator_emit_strings (zxbc.py:144
     * before :146) — a CONST-string DATA item calls add_string_label
     * here, which the emit_strings drain then emits. No-op unless
     * gl.DATA_IS_USED and gl.DATAS is non-empty. */
    translator_emit_data_blocks(&tr);

    /* 8  translator.emit_strings() (zxbc.py:146): drains the STRING_LABELS
     * dedup store (populated by visit_STRING during steps 6/7 and by
     * emit_data_blocks' CONST-string-DATA add_string_label) into ic_vard
     * quads, in insertion order. Same site Python calls it: after
     * emit_data_blocks, before emit_jump_tables. No-op when no constant
     * string was visited. */
    translator_emit_strings(&tr);

    /* 8  translator.emit_jump_tables() (zxbc.py:148): drains the ON
     * GOTO/GOSUB JUMP_TABLES (S5.5). No-op when none appeared. */
    translator_emit_jump_tables(&tr);

    /* 8  translator.ic_inline(";; --- end of user code ---") (zxbc.py:150) */
    translator_ic_inline(&tr, ";; --- end of user code ---");

    /* 9  asm_output = backend.emit(optimize = O > 0)  (zxbc.py:171) */
    StrVec asm_inner = backend_emit(&b, cs->opts.optimization_level > 0);

    /* 10 asm_output = Optimizer().optimize(asm_output) + "\n" (zxbc.py:172)
     *
     * S5.9b: optimizer_optimize() is the faithful Optimizer.optimize()
     * (optimizer/optmain.c) INCLUDING the verbatim main.py:199
     * early-return — for OPTIONS.optimization_level <= 2 it delegates to
     * codegen_le2_adapter (which calls the unchanged, byte-proven
     * optimizer_optimize_le2 below), so the O<=2 asm output is
     * byte-identical to HEAD; the O3 basic-block / CPU-state / flow-graph
     * machinery runs only when opt_level > 2 (Z80StrList and StrVec are
     * both `VEC(char*)` — identical layout; the adapter views one as the
     * other without a copy). */
    Z80StrList asm_zl;
    asm_zl.data = asm_inner.data;
    asm_zl.len  = asm_inner.len;
    asm_zl.cap  = asm_inner.cap;
    Optimizer opt;
    optimizer_init(&opt, a);
    opt.debug_level = cs->opts.debug_level;  /* OPTIONS.debug_level mirror */
    char *joined = optimizer_optimize(&opt, a, asm_zl,
                                      cs->opts.optimization_level,
                                      codegen_le2_adapter);
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

    /* 11 zxbpp ASM-mode re-filter (zxbc.py:183-187): a FRESH ASM-mode
     * preprocessor pass over the just-generated asm string, keyed on the
     * original input filename, whose OUTPUT (with `#include once` bodies
     * inlined) replaces asm_output. Faithful to:
     *
     *   set_option_defines()                # zxbpp.init() prep
     *   zxbpp.reset_id_table()
     *   zxbpp.setMode(zxbpp.PreprocMode.ASM)
     *   zxbpp.OUTPUT = ""
     *   zxbpp.filter_(asm_output, filename=input_filename)
     *   asm_output = zxbpp.OUTPUT.split("\n")
     *
     * A fresh PreprocState is the C analogue of the reset_id_table()+""
     * OUTPUT reset; ppf.in_asm = true is the setMode(ASM) toggle;
     * preproc_string(.., input_filename) is filter_(asm_output,
     * filename=input_filename) (zxbpp.py:912-925 LEXER.input(input_,
     * filename)); ppf.output (read via strbuf_cstr) is the zxbpp.OUTPUT
     * global. Include search path = the executable-anchored ABSOLUTE
     * src/lib/arch/<arch>/{stdlib,runtime} (stdlib first, runtime
     * second — zxbpp.py:142-164,182-205; Python's <...> form =>
     * local_first=False, pure path search, first-match-wins) then the
     * colon-split OPTIONS.include_path (zxbpp.py:193) — built EXACTLY as
     * the first pass does (main.c, same arch/include_path sources, same
     * unconditional-push, same order, same -I split). The absolute
     * built-in anchor is CWD-independent (get_include_path uses
     * os.path.dirname(__file__), zxbpp.py:144-152); the EXISTING #line
     * transform (preproc.c:1371-1382, get_relative_filename_path
     * zxbpp.py:220 / api/utils.py:95-107) then re-relativises it to the
     * CWD when the file is under CWD and keeps it absolute otherwise —
     * byte-matching Python from BOTH project-root and scratch CWDs by
     * construction. The original input filename is
     * cs->opts.input_filename — the same value main.c:71 passes to the
     * first-pass preproc_file (zxbc.py:187 filename=input_filename).
     *
     * NOTE (S7.1c-i scope): the inlined bodies still carry their #init
     * lines and the prologue still lacks the call .core.__PRINT_INIT /
     * __MEM_INIT lines — that is the get_inits harvest, deferred to
     * S7.1c-ii (zxbc.py:188-191); the heap EQU/DEFS specifics are
     * S7.1c-iii. Not implemented here, by design. */
    PreprocState ppf;
    preproc_init(&ppf);
    ppf.in_asm = true;       /* zxbpp.setMode(PreprocMode.ASM) analogue:
                              * still needed for the #line-silent-consume
                              * and the backslash-continuation join — both
                              * part of the existing byte-correct #line
                              * machinery (kept, not reverted). */
    ppf.asm_filter_mode = true; /* select the dedicated ASM-filter line
                              * model — emit `;` comments + their bodies +
                              * whitespace + asm body VERBATIM, process
                              * only #-directives + macro-expand IDs;
                              * mirrors src/zxbpp/zxbasmpplex.py (distinct
                              * from in_asm, the BASIC asm..end asm
                              * tracker). zxbc.py:181-189 second pass. */
    ppf.debug_level = cs->opts.debug_level;
    /* BYTE-IDENTICAL to the first-pass block in main.c (codegen.c
     * mandate above): executable-anchored ABSOLUTE built-in pair pushed
     * UNCONDITIONALLY (no access() gate — set_include_path is
     * unconditional, zxbpp.py:158-164; only per-file lookup skips
     * missing dirs, zxbpp.py:195-205), then the colon-SPLIT -I dirs
     * appended after (zxbpp.py:193). argv[0] is unavailable this deep in
     * the pipeline; get_executable_dir uses the OS exe-path APIs
     * (_NSGetExecutablePath / /proc/self/exe / GetModuleFileNameA) which
     * need no argv — the argv0 fallback is unreachable on supported
     * platforms, so passing NULL is sound. */
    {
        const char *arch = cs->opts.architecture ? cs->opts.architecture : "zx48k";
        char exe_dir[PATH_MAX];
        char raw_path[PATH_MAX];
        char real_path[PATH_MAX];

        if (get_executable_dir(NULL, exe_dir, sizeof(exe_dir))) {
            snprintf(raw_path, sizeof(raw_path),
                     "%s/../../../src/lib/arch/%s/stdlib", exe_dir, arch);
            if (realpath(raw_path, real_path))
                vec_push(ppf.include_paths, arena_strdup(&ppf.arena, real_path));
            snprintf(raw_path, sizeof(raw_path),
                     "%s/../../../src/lib/arch/%s/runtime", exe_dir, arch);
            if (realpath(raw_path, real_path))
                vec_push(ppf.include_paths, arena_strdup(&ppf.arena, real_path));
        }
    }

    if (cs->opts.include_path && cs->opts.include_path[0]) {
        char ipbuf[PATH_MAX];
        if (strlen(cs->opts.include_path) < sizeof(ipbuf)) {
            strcpy(ipbuf, cs->opts.include_path);
            char *save = NULL;
            for (char *seg = strtok_r(ipbuf, ":", &save);
                 seg != NULL;
                 seg = strtok_r(NULL, ":", &save)) {
                if (seg[0])
                    vec_push(ppf.include_paths, arena_strdup(&ppf.arena, seg));
            }
        }
    }

    /* set_option_defines() + reset_id_table() (zxbc.py:183-184): Python
     * re-seeds zxbpp's ID_TABLE from config.OPTIONS.__DEFINES before the
     * ASM filter_ (zxbpp.py:106-113 reset_id_table iterates
     * OPTIONS.__DEFINES). __DEFINES holds (a) the option-driven defines
     * from set_option_defines() (src/zxbc/args_config.py:177-188:
     * __MEMORY_CHECK__ if memory_check, __CHECK_ARRAY_BOUNDARY__ if
     * array_check, __ENABLE_BREAK__ if enable_break, __OPT_STRATEGY__
     * always = opt_strategy) and (b) ___PRINT_IS_USED___ = 1 added by
     * p_start when the program used PRINT (src/zxbc/zxbparser.py:512-513,
     * PRINT_IS_USED). The fresh ppf is the cleared ID_TABLE; these
     * preproc_define calls are the reset_id_table() re-seed. Faithful to
     * the exact set: empty bodies for the flags (Python "" value),
     * "1" for ___PRINT_IS_USED___, the opt_strategy number for
     * __OPT_STRATEGY__. ___PRINT_IS_USED___ is the byte-critical one:
     * copy_attr.asm:1 `#ifdef ___PRINT_IS_USED___` gates its
     * `#include once <print.asm>` (copy_attr.asm:2), so without this the
     * runtime include-resolution ORDER diverges from Python. */
    if (cs->opts.memory_check)
        preproc_define(&ppf, "__MEMORY_CHECK__", "", 0, NULL);
    if (cs->opts.array_check)
        preproc_define(&ppf, "__CHECK_ARRAY_BOUNDARY__", "", 0, NULL);
    if (cs->opts.enable_break)
        preproc_define(&ppf, "__ENABLE_BREAK__", "", 0, NULL);
    {
        char optstrat[16];
        snprintf(optstrat, sizeof(optstrat), "%d", (int)cs->opts.opt_strategy);
        preproc_define(&ppf, "__OPT_STRATEGY__", optstrat, 0, NULL);
    }
    if (cs->print_is_used)
        preproc_define(&ppf, "___PRINT_IS_USED___", "1", 0, NULL);

    /* User -D/--define defines re-seeded for the ASM filter_ pass too.
     * Python's OPTIONS.__DEFINES (populated by args_config.py:91-96) is
     * PERSISTENT, so reset_id_table() (zxbpp.py:106-113) re-seeds these
     * into the cleared ID_TABLE for the ASM pass exactly as for the
     * BASIC pass. Identical split-on-first-'=' semantics to main.c
     * (compiler_split_define = Python i.split("=", 1)). */
    for (int di = 0; di < cs->opts.defines_count; di++) {
        const char *raw = cs->opts.defines[di];
        /* Heap scratch sized to the arg (see main.c): Python has no
         * length cap on -D, so neither do we. */
        char *scratch = malloc(strlen(raw) + 1);
        const char *dname, *dval;
        if (scratch) {
            compiler_split_define(raw, scratch, &dname, &dval);
            preproc_define(&ppf, dname, dval, 0, NULL);
            free(scratch);
        }
    }

    (void)preproc_string(&ppf, rejoined, cs->opts.input_filename);

    /* Extract the produced text exactly as main.c:80 extracts the
     * first-pass output (char *source = strbuf_cstr(&pp.output)). This
     * inlined string replaces `rejoined` as the input to split_nl —
     * asm_output = zxbpp.OUTPUT.split("\n") (zxbc.py:190).
     *
     * preproc_string (preproc.c:2136) prepends a leading
     * `#line 1 "<input_filename>"` to its output (the baseline-#line that
     * the BASIC first pass / preproc_file:2052 also emit, and which IS
     * faithful to the BASIC first pass — verified: standalone
     * `zxbpp.py hi.bas` => first line `#line 1 "/tmp/hi.bas"`). But
     * Python's ASM-mode `filter_` does NOT emit it: LEXER.input
     * (base_pplex.py:148-152) only sets the input string — the first
     * `#line` Python emits is the begin-of-include marker
     * (base_pplex.py:111), so hi_py.asm starts directly with the asm
     * (line 1 = `org 32768`). Mirror Python's filter_: drop exactly that
     * single synthetic leading line if present, so the C OUTPUT begins
     * with the asm content like zxbpp.OUTPUT does (zxbc.py:186-190). */
    const char *filtered = arena_strdup(a, strbuf_cstr(&ppf.output));
    {
        size_t fl = strlen("#line 1 \"") + strlen(cs->opts.input_filename) + 2;
        char *lead = arena_alloc(a, fl + 1);
        snprintf(lead, fl + 1, "#line 1 \"%s\"\n", cs->opts.input_filename);
        size_t ll = strlen(lead);
        if (strncmp(filtered, lead, ll) == 0)
            filtered += ll;
    }
    preproc_destroy(&ppf);

    StrVec asm_lines = split_nl(a, filtered);

    /* zxbc.py:191 — get_inits(asm_output) immediately after
     * `asm_output = zxbpp.OUTPUT.split("\n")` and BEFORE the asm vector
     * is consumed (VarTranslator/emit_prologue/MAIN assembly). Unions
     * (no-op) the parser inits, harvests every surviving `#init`
     * directive into b->inits (the set emit_prologue reads) and blanks
     * those lines so zero `#init` survive into the final output. */
    get_inits(a, &b, &asm_lines);

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

    /* output(asm_output, ...) then optional assemble  (zxbc.py:212-228) */
    int rc = 0;
    if (strcmp(cs->opts.output_file_type, "asm") == 0) {
        /* zxbc.py:212-214: output_file_type == FileType.ASM -> write the
         * rendered asm text straight to the output file. */
        FILE *of = fopen(cs->opts.output_filename, "wb");
        if (!of) {
            zxbc_error(cs, 0, "cannot open output file '%s'",
                       cs->opts.output_filename);
            rc = 1;
        } else {
            emit_output(of, final, a);
            fclose(of);
        }
    } else {
        /* zxbc.py:215-228 (every other output_file_type — bin/tap/tzx/
         * sna/z80): render asm_output to text, assemble it, then
         * generate_binary with the requested format passed through
         * verbatim; gl.has_errors -> return 5. codegen_emit is only
         * reached when not parse_only (main.c:143-148 short-circuits
         * parse_only before calling here), mirroring Python's
         * `elif not options.parse_only`. */
        const char *asm_text = emit_output_str(final, a);
        if (zxbc_asm_to_binary(asm_text, cs->opts.output_filename,
                               cs->opts.output_file_type,
                               cs->opts.use_basic_loader,
                               cs->opts.autorun,
                               cs->opts.append_binary,
                               cs->opts.append_binary_count,
                               cs->opts.append_headless_binary,
                               cs->opts.append_headless_binary_count) != 0)
            rc = 5;
    }

    vec_free(prologue); vec_free(epilogue); vec_free(asm_lines);
    vec_free(tmp); vec_free(final);
    backend_free(&b);
    return rc;
}
