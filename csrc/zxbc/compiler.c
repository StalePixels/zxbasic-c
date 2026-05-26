/*
 * compiler.c — Compiler state initialization and management
 *
 * Ported from src/api/global_.py and src/zxbc/zxbparser.py init()
 */
#include "zxbc.h"
#include "errmsg.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Helper: ASCII-lowercase a string into the arena. Python uses
 * str.lower() which on the names actually produced by the lexer
 * (BASIC identifiers) is ASCII-only — tolower() is faithful. */
static char *arena_strlower(Arena *a, const char *s) {
    size_t n = strlen(s);
    char *out = arena_alloc(a, n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        out[i] = (char)tolower(c);
    }
    out[n] = '\0';
    return out;
}

/* Python Scope.__setitem__ (scope.py:53-57). Also mirrors the
 * entry into the scope's caseins map keyed by lower(name) when
 * the entry was declared case-insensitive. */
static void scope_set_symbol(SymbolTable *st, Scope_ *scope,
                             const char *name, AstNode *node) {
    hashmap_set(&scope->symbols, name, node);
    if (node->tag == AST_ID && node->u.id.caseins) {
        char *lower = arena_strlower(st->arena, name);
        hashmap_set(&scope->caseins, lower, node);
    }
}

/* Python Scope.__getitem__ (scope.py:50-51): exact-case lookup,
 * falling back to caseins[name.lower()]. Pure passthrough when no
 * caseins entries exist in the scope. */
static AstNode *scope_get_symbol(Scope_ *scope, const char *name) {
    AstNode *n = hashmap_get(&scope->symbols, name);
    if (n) return n;
    if (scope->caseins.count == 0) return NULL;
    /* Stack buffer for the lowercase key — names are bounded by
     * the lexer's identifier length. */
    char buf[256];
    size_t len = strlen(name);
    if (len + 1 > sizeof(buf)) return NULL;
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[len] = '\0';
    return hashmap_get(&scope->caseins, buf);
}

/* Python Scope.__delitem__ (scope.py:59-61). */
static void scope_del_symbol(Scope_ *scope, const char *name) {
    hashmap_remove(&scope->symbols, name);
    if (scope->caseins.count == 0) return;
    char buf[256];
    size_t len = strlen(name);
    if (len + 1 > sizeof(buf)) return;
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)name[i]);
    buf[len] = '\0';
    hashmap_remove(&scope->caseins, buf);
}

/* ----------------------------------------------------------------
 * Symbol table
 * ---------------------------------------------------------------- */

SymbolTable *symboltable_new(CompilerState *cs) {
    SymbolTable *st = arena_calloc(&cs->arena, 1, sizeof(SymbolTable));
    st->arena = &cs->arena;

    /* Create global scope */
    st->global_scope = arena_calloc(&cs->arena, 1, sizeof(Scope_));
    hashmap_init(&st->global_scope->symbols);
    hashmap_init(&st->global_scope->caseins);
    st->global_scope->parent = NULL;
    st->global_scope->level = 0;
    st->global_scope->namespace_ = ""; /* Python current_namespace="" (symboltable.py:54) */
    st->current_scope = st->global_scope;

    /* Initialize type registry */
    hashmap_init(&st->type_registry);

    /* Register all basic types */
    for (int i = 0; i < TYPE_COUNT; i++) {
        st->basic_types[i] = type_new_basic(cs, (BasicType)i);
        hashmap_set(&st->type_registry, st->basic_types[i]->name, st->basic_types[i]);
    }

    return st;
}

/* SymbolTable.make_child_namespace (symboltable.py:140-149). */
char *make_child_namespace(CompilerState *cs, const char *parent,
                           const char *child) {
    if (!child) child = "";
    if (parent == NULL || parent[0] == '\0') {
        /* MANGLE_CHR ("_") + child (global_.py:167). */
        size_t cl = strlen(child);
        char *r = arena_alloc(&cs->arena, cl + 2);
        r[0] = '_';
        memcpy(r + 1, child, cl + 1);
        return r;
    }
    /* parent + NAMESPACE_SEPARATOR (".") + child (global_.py:168). */
    size_t pl = strlen(parent), cl = strlen(child);
    char *r = arena_alloc(&cs->arena, pl + cl + 2);
    memcpy(r, parent, pl);
    r[pl] = '.';
    memcpy(r + pl + 1, child, cl + 1);
    return r;
}

void symboltable_enter_scope(SymbolTable *st, CompilerState *cs,
                             const char *namespace_name) {
    Scope_ *scope = arena_calloc(st->arena, 1, sizeof(Scope_));
    hashmap_init(&scope->symbols);
    hashmap_init(&scope->caseins);
    scope->parent = st->current_scope;
    scope->level = st->current_scope->level + 1;
    /* Python enter_scope: current_namespace = make_child_namespace(
     * current_namespace, namespace) (symboltable.py:228); the new Scope
     * stores it (scope.py:45). */
    scope->namespace_ = make_child_namespace(
        cs, st->current_scope->namespace_, namespace_name ? namespace_name : "");
    st->current_scope = scope;
}

/* S5.7d — append an ID to the current scope's insertion-ordered list
 * (mirrors Python self.current_scope[id] = entry into the OrderedDict,
 * scope.py:43-44). Arena-doubling growth; old buffers stay in the arena
 * (freed wholesale at compile end) — the project's arena discipline. */
static void scope_push_ordered(SymbolTable *st, Scope_ *scope, AstNode *node) {
    if (scope->ordered_count >= scope->ordered_cap) {
        int ncap = scope->ordered_cap ? scope->ordered_cap * 2 : 8;
        AstNode **nb = arena_alloc(st->arena, (size_t)ncap * sizeof(AstNode *));
        if (scope->ordered_count > 0)
            memcpy(nb, scope->ordered,
                   (size_t)scope->ordered_count * sizeof(AstNode *));
        scope->ordered = nb;
        scope->ordered_cap = ncap;
    }
    scope->ordered[scope->ordered_count++] = node;
}

void symboltable_exit_scope(SymbolTable *st) {
    Scope_ *leaving = st->current_scope;

    /* Python leave_scope (symboltable.py:279-281): every CLASS.unknown
     * entry remaining in the scope being left is moved to the GLOBAL
     * scope via move_to_global_scope. This is the load-bearing
     * def-LATER mechanism: a call like `r(q$)` inside `SUB p` where `r`
     * is not yet declared implicitly declares `r` as CLASS_unknown in
     * p's scope (access_func). On leaving p's scope Python relocates
     * that entry to global so the later top-level `SUB r` definition's
     * declare_func (get_entry searches all scopes) finds and mutates
     * THE SAME entry object — making the CALL's callee and the
     * FUNCDECL's entry identical (shared accessed/class/body). Without
     * this the C kept two distinct `r` entries: the call's orphaned
     * CLASS_unknown one (dropped by the callee-class codegen gate) and
     * the FUNCDECL's global one. (paramstr4/paramstr5 mechanism 1.)
     *
     * move_to_global_scope (symboltable.py:289-305): scope=global_; for
     * non-labels mangled = make_child_namespace(global_ns "", name) ==
     * "_name"; insert into global scope, delete from the leaving scope.
     * Iterate the insertion-ordered list (Python current_scope.values()
     * order); guard `len(table) > 1` is implied (we only relocate when
     * leaving a non-global scope, i.e. parent != NULL). */
    if (leaving->parent) {
        for (int i = 0; i < leaving->ordered_count; i++) {
            AstNode *e = leaving->ordered[i];
            if (!e || e->tag != AST_ID ||
                e->u.id.class_ != CLASS_unknown)
                continue;
            const char *nm = e->u.id.name;
            /* Still keyed in the leaving scope? (Python `id_ in
             * current_scope`.) */
            if (hashmap_get(&leaving->symbols, nm) != e)
                continue;
            e->u.id.scope = SCOPE_global;
            e->u.id.offset_set = false; /* symbol.ref.offset = None */
            /* mangled = make_child_namespace("", name) == "_" + name
             * (global scope namespace is ""). Recompute off the
             * arena (st->arena) — no signature change. */
            {
                size_t nl = strlen(nm);
                char *m = arena_alloc(st->arena, nl + 2);
                m[0] = '_';
                memcpy(m + 1, nm, nl + 1);
                e->u.id.mangled = m;
            }
            scope_set_symbol(st, st->global_scope, nm, e);
            scope_del_symbol(leaving, nm);
            /* Python `del self.current_scope[id_]` (symboltable.py:304)
             * removes the entry from the leaving scope's OrderedDict — and
             * since func.ref.local_symbol_table IS that same Scope object
             * (zxbparser.py:2910, assigned by reference BEFORE leave_scope),
             * the promoted entry vanishes from the values() the
             * FunctionTranslator later iterates. The C captures local_entries
             * from leaving->ordered, so the entry must also be COMPACTED out
             * of ordered here; otherwise a forward `@Map` inside a function
             * (creating CLASS_unknown _q.Map, later promoted to global _Map
             * by a top-level `Dim Map = 1`) would linger in the function's
             * local_entries and emit a spurious local-init image of the
             * global's default value (dimconst2d). i is decremented so the
             * compacted-in successor is still visited. */
            for (int j = i; j < leaving->ordered_count - 1; j++)
                leaving->ordered[j] = leaving->ordered[j + 1];
            leaving->ordered_count--;
            i--;
            /* Keep it discoverable in global insertion order too (the
             * faithful analogue of global_scope[id_] = symbol adding it
             * to the global OrderedDict). */
            scope_push_ordered(st, st->global_scope, e);
        }
    }

    if (st->current_scope->parent) {
        /* Note: we don't free the scope — arena handles that */
        st->current_scope = st->current_scope->parent;
    }
}

/* S5.7d — SymbolTable.compute_offsets (symboltable.py:235-266), verbatim.
 *
 * entry_size: global|addr-set|const -> 0; non-array -> var size; array ->
 * memsize. The list is the scope's INSERTION order, then a STABLE sort
 * ascending by entry_size (Python sorted(..., key=entry_size) over the
 * OrderedDict — equal keys keep insertion order). Running `offset`:
 * scalar-local => offset += sz; ref.offset = offset; array-local =>
 * ref.offset = sz + offset; offset = ref.offset. function/label/type and
 * params are skipped (params' offset is the PARAMLIST cumulative, set at
 * scope entry). Returns total frame bytes (== locals_size, the ic_enter
 * operand). PTR_TYPE size 2, PARAM_ALIGN 2 on zx48k (parser.c:2451-2452).
 *
 * varref.py:44-57 size: scalar local => raw type_.size (no even-round);
 *   parameter byref => PTR(2), byval => ts + ts%PARAM_ALIGN.
 * arrayref.py:39-59: array size = count*elem (param => PTR), memsize =
 *   ptr_size*(3 + ubound_used). compute_offsets uses memsize for the slot.
 */
static int s_entry_size(SymbolTable *st, AstNode *e) {
    if (e->u.id.scope == SCOPE_global || e->u.id.addr_expr != NULL ||
        e->u.id.addr_set || e->u.id.class_ == CLASS_const)
        return 0;
    if (e->u.id.class_ != CLASS_array) {
        if (e->type_ == NULL) return 0;             /* varref.py:46-47 */
        if (e->u.id.scope == SCOPE_parameter) {
            if (e->u.id.byref) return 2;            /* PTR_TYPE size */
            int ts = type_size(e->type_);
            return ts + (ts % 2);                   /* even (Float/Byte) */
        }
        return type_size(e->type_);                 /* scalar local: raw */
    }
    /* array: memsize = PTR(2) * (3 + ubound_used) (arrayref.py:230-233) */
    (void)st;
    return 2 * (3 + (e->u.id.ubound_used ? 1 : 0));
}

int symboltable_compute_offsets(SymbolTable *st, Scope_ *scope, int opt_level) {
    int n_raw = scope->ordered_count;
    /* Python Scope.values(filter_by_opt=True) at O>1 (scope.py:63-66):
     * un-accessed entries are excluded from the offset-assignment pass
     * (and therefore from locals_size). leave_scope force-marks params
     * accessed before calling compute_offsets (symboltable.py:270-277),
     * so the filter only ever drops LOCAL CLASS_var/array entries that
     * the program never read — the const7 / opt2_func_call shape. */
    AstNode **a = arena_alloc(st->arena, (size_t)(n_raw ? n_raw : 1) * sizeof(AstNode *));
    int n = 0;
    for (int i = 0; i < n_raw; i++) {
        AstNode *e = scope->ordered[i];
        if (opt_level > 1 && e && e->tag == AST_ID && !e->u.id.accessed)
            continue;
        a[n++] = e;
    }
    /* Stable sort ascending by entry_size (insertion order tie-break).
     * Insertion sort keeps it stable and n is tiny (locals per function). */
    for (int i = 1; i < n; i++) {
        AstNode *key = a[i];
        int ksz = s_entry_size(st, key);
        int j = i - 1;
        while (j >= 0 && s_entry_size(st, a[j]) > ksz) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }

    int offset = 0;
    for (int i = 0; i < n; i++) {
        AstNode *e = a[i];
        SymbolClass c = e->u.id.class_;
        if (c == CLASS_function || c == CLASS_sub || c == CLASS_label ||
            c == CLASS_type)
            continue;
        if (c == CLASS_var && e->u.id.scope == SCOPE_local) {
            offset += s_entry_size(st, e);
            e->u.id.offset = offset;
            e->u.id.offset_set = true;
        }
        if (c == CLASS_array && e->u.id.scope == SCOPE_local) {
            e->u.id.offset = s_entry_size(st, e) + offset;
            e->u.id.offset_set = true;
            offset = e->u.id.offset;
        }
    }
    return offset;
}

AstNode *symboltable_declare(SymbolTable *st, CompilerState *cs,
                              const char *name, int lineno, SymbolClass class_) {
    /* Check if already declared in current scope */
    AstNode *existing = hashmap_get(&st->current_scope->symbols, name);
    if (existing) {
        return existing;  /* Caller decides whether to error */
    }

    /* Create new ID node */
    AstNode *node = ast_new(cs, AST_ID, lineno);
    node->u.id.name = arena_strdup(&cs->arena, name);
    /* Python SymbolTable.declare (symboltable.py:122):
     *   entry.mangled = make_child_namespace(current_namespace, name)
     * The global scope's namespace is "" so make_child_namespace("",name)
     * == "_name" — identical to the prior MANGLE_CHR+name for every
     * global entry. Only a nested-scope declaration (current namespace
     * non-empty, e.g. a SUB defined inside another SUB) differs:
     * "_parent.child" instead of "_child" — the faithful nested-sub
     * mangle rule (opt2_labelinfunc4/5, paramstr5). */
    node->u.id.mangled =
        make_child_namespace(cs, st->current_scope->namespace_, name);
    /* Python ID.filename (_id.py:58): self.filename = global_.FILENAME at
     * first creation. Stamp cs->current_file here so the R11 emit
     * (compiler.c:2063) — and any other entry.filename-attributed Python
     * diagnostic ported later — reports the file that was #line-active
     * when the symbol was FIRST seen, not the file active at error-emit
     * time. arena_strdup snapshots the value (cs->current_file is mutated
     * mid-parse by #line directives). */
    node->u.id.filename = (cs->current_file)
                              ? arena_strdup(&cs->arena, cs->current_file)
                              : NULL;
    node->u.id.class_ = class_;
    node->u.id.scope = (st->current_scope->level == 0) ? SCOPE_global : SCOPE_local;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
    /* Python symboltable.py:115 — stamp the current pragma value
     * onto the entry at declare time. The scope insertion below
     * mirrors it into the caseins map when TRUE. */
    node->u.id.caseins = cs->opts.case_insensitive;

    scope_set_symbol(st, st->current_scope, name, node);
    /* N1: the single symbol-table insertion point (mirrors Python
     * symboltable.py:116 self.current_scope[id2] = entry). Record every
     * entry once, in first-textual-appearance order, regardless of class
     * or O-level — data_ast is filtered from this later (codegen.c). */
    vec_push(cs->sym_entries_ordered, node);
    scope_push_ordered(st, st->current_scope, node);  /* S5.7d per-scope */
    return node;
}

AstNode *symboltable_lookup(SymbolTable *st, const char *name) {
    /* Search from current scope up to global. Python Scope.__getitem__
     * (scope.py:50-51) falls back to the caseins map keyed by
     * lower(name) when the exact-case key misses — handled by
     * scope_get_symbol. */
    for (Scope_ *s = st->current_scope; s != NULL; s = s->parent) {
        AstNode *node = scope_get_symbol(s, name);
        if (node) return node;
    }
    return NULL;
}

AstNode *symboltable_get_entry(SymbolTable *st, const char *name) {
    /* Python get_entry (symboltable.py:76-77) strips a trailing
     * deprecated suffix ($/%/&) before the table lookup, so `c$`
     * resolves to the array entry stored under `c`.  symboltable_lookup
     * uses the raw key; mirror the strip here so the lexer's ARRAY_ID
     * promotion (lexer.c:1186-1188) fires for a suffixed array name —
     * without it `c$ = a$` parses as a scalar LET, not ARRAYCOPY
     * (arraycopy5). */
    size_t len = name ? strlen(name) : 0;
    if (len > 0 && is_deprecated_suffix(name[len - 1])) {
        char buf[256];
        if (len - 1 < sizeof(buf)) {
            memcpy(buf, name, len - 1);
            buf[len - 1] = '\0';
            return symboltable_lookup(st, buf);
        }
    }
    return symboltable_lookup(st, name);
}

TypeInfo *symboltable_get_type(SymbolTable *st, const char *name) {
    return hashmap_get(&st->type_registry, name);
}

/* ----------------------------------------------------------------
 * Helper: strip deprecated suffix from name, return base name
 * ---------------------------------------------------------------- */
static const char *strip_suffix(Arena *arena, const char *name, char *out_suffix) {
    size_t len = strlen(name);
    *out_suffix = '\0';
    if (len > 0 && is_deprecated_suffix(name[len - 1])) {
        *out_suffix = name[len - 1];
        char *base = arena_alloc(arena, len);
        memcpy(base, name, len - 1);
        base[len - 1] = '\0';
        return base;
    }
    return name;
}

/* ----------------------------------------------------------------
 * Higher-level declaration functions
 * Ported from src/api/symboltable/symboltable.py
 * ---------------------------------------------------------------- */

/* Resolve a TypeInfo to its final basic type */
static BasicType resolve_basic_type(const TypeInfo *t) {
    if (!t) return TYPE_unknown;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    return f->basic_type;
}

AstNode *symboltable_declare_variable(SymbolTable *st, CompilerState *cs,
                                       const char *name, int lineno, TypeInfo *typeref) {
    char suffix = '\0';
    const char *base_name = strip_suffix(&cs->arena, name, &suffix);

    /* Check suffix type matches declared type */
    if (suffix != '\0') {
        BasicType suffix_type = suffix_to_type(suffix);
        BasicType decl_type = resolve_basic_type(typeref);
        if (decl_type != TYPE_unknown && decl_type != suffix_type) {
            zxbc_error(cs, lineno, "expected type %s for '%s', got %s",
                       basictype_to_string(suffix_type), name,
                       basictype_to_string(decl_type));
            zxbc_error(cs, lineno, "'%s' suffix is for type '%s' but it was declared as '%s'",
                       name, basictype_to_string(suffix_type),
                       basictype_to_string(decl_type));
            return NULL;
        }
    }

    /* Check if already declared in current scope */
    AstNode *existing = hashmap_get(&st->current_scope->symbols, base_name);
    if (existing && existing->u.id.declared) {
        zxbc_error(cs, lineno, "Variable '%s' already declared at %s:%d",
                   name, cs->current_file ? cs->current_file : "(stdin)",
                   existing->lineno);
        return NULL;
    }

    /* Create new ID node */
    AstNode *node = ast_new(cs, AST_ID, lineno);
    node->u.id.name = arena_strdup(&cs->arena, base_name);
    {
        size_t nl = strlen(base_name);
        char *m = arena_alloc(&cs->arena, nl + 2);
        m[0] = '_';
        memcpy(m + 1, base_name, nl + 1);
        node->u.id.mangled = m;   /* Python ID.mangled (_id.py:60) */
    }
    node->u.id.class_ = CLASS_var;
    node->u.id.scope = (st->current_scope->level == 0) ? SCOPE_global : SCOPE_local;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
    node->u.id.caseins = cs->opts.case_insensitive;  /* symboltable.py:115 */
    node->type_ = typeref;

    /* Generate temp label: string params get "$" prefix */
    if (resolve_basic_type(typeref) == TYPE_string) {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "$%s", base_name);
        node->t = arena_strdup(&cs->arena, tbuf);
    } else {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "_%s", base_name);
        node->t = arena_strdup(&cs->arena, tbuf);
    }

    scope_set_symbol(st, st->current_scope, base_name, node);
    vec_push(cs->sym_entries_ordered, node);  /* N1 (symboltable.py:116) */
    scope_push_ordered(st, st->current_scope, node);  /* S5.7d per-scope */
    return node;
}

AstNode *symboltable_declare_param(SymbolTable *st, CompilerState *cs,
                                    const char *name, int lineno, TypeInfo *typeref) {
    /* Check if already declared in current scope */
    AstNode *existing = hashmap_get(&st->current_scope->symbols, name);
    if (existing && existing->u.id.declared) {
        zxbc_error(cs, lineno, "Duplicated parameter \"%s\" (previous one at %s:%d)",
                   name, cs->current_file ? cs->current_file : "(stdin)",
                   existing->lineno);
        return NULL;
    }

    /* Create new ID node as parameter */
    AstNode *node = ast_new(cs, AST_ID, lineno);
    node->u.id.name = arena_strdup(&cs->arena, name);
    {
        size_t nl = strlen(name);
        char *m = arena_alloc(&cs->arena, nl + 2);
        m[0] = '_';
        memcpy(m + 1, name, nl + 1);
        node->u.id.mangled = m;   /* Python ID.mangled (_id.py:60) */
    }
    node->u.id.class_ = CLASS_var;
    node->u.id.scope = SCOPE_parameter;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
    node->u.id.caseins = cs->opts.case_insensitive;  /* symboltable.py:115 */
    node->type_ = typeref;

    /* String params get "$" prefix in t */
    if (resolve_basic_type(typeref) == TYPE_string) {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "$%s", name);
        node->t = arena_strdup(&cs->arena, tbuf);
    } else {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "_%s", name);
        node->t = arena_strdup(&cs->arena, tbuf);
    }

    scope_set_symbol(st, st->current_scope, name, node);
    vec_push(cs->sym_entries_ordered, node);  /* N1 (symboltable.py:116) */
    scope_push_ordered(st, st->current_scope, node);  /* S5.7d per-scope */
    return node;
}

AstNode *symboltable_declare_array(SymbolTable *st, CompilerState *cs,
                                    const char *name, int lineno,
                                    TypeInfo *typeref, AstNode *bounds) {
    /* Type must be a TYPEREF */
    if (!typeref || typeref->tag != AST_TYPEREF) {
        return NULL;  /* assertion failure in Python */
    }

    /* Bounds must be a BOUNDLIST */
    if (!bounds || bounds->tag != AST_BOUNDLIST) {
        return NULL;  /* assertion failure in Python */
    }

    AstNode *node = ast_new(cs, AST_ID, lineno);
    node->u.id.name = arena_strdup(&cs->arena, name);
    {
        size_t nl = strlen(name);
        char *m = arena_alloc(&cs->arena, nl + 2);
        m[0] = '_';
        memcpy(m + 1, name, nl + 1);
        node->u.id.mangled = m;   /* Python ID.mangled (_id.py:60) */
    }
    node->u.id.class_ = CLASS_array;
    node->u.id.scope = (st->current_scope->level == 0) ? SCOPE_global : SCOPE_local;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
    node->u.id.caseins = cs->opts.case_insensitive;  /* symboltable.py:115 */
    node->type_ = typeref;

    scope_set_symbol(st, st->current_scope, name, node);
    vec_push(cs->sym_entries_ordered, node);  /* N1 (symboltable.py:116) */
    scope_push_ordered(st, st->current_scope, node);  /* S5.7d per-scope */
    return node;
}

/* ----------------------------------------------------------------
 * Check functions
 * ---------------------------------------------------------------- */

bool symboltable_check_is_declared(SymbolTable *st, const char *name, int lineno,
                                    const char *classname, bool show_error,
                                    CompilerState *cs) {
    /* Strip suffix for lookup */
    char suffix;
    Arena tmp;
    arena_init(&tmp, 256);
    const char *base = strip_suffix(&tmp, name, &suffix);

    AstNode *entry = symboltable_lookup(st, base);
    arena_destroy(&tmp);

    if (!entry || !entry->u.id.declared) {
        if (show_error && cs) {
            zxbc_error(cs, lineno, "Undeclared %s \"%s\"", classname, name);
        }
        return false;
    }
    return true;
}

bool symboltable_check_is_undeclared(SymbolTable *st, const char *name, int lineno,
                                      bool show_error, CompilerState *cs) {
    /* Strip suffix for lookup */
    char suffix;
    Arena tmp;
    arena_init(&tmp, 256);
    const char *base = strip_suffix(&tmp, name, &suffix);

    /* Check current scope only (matching Python's scope= parameter behavior) */
    AstNode *entry = hashmap_get(&st->current_scope->symbols, base);
    arena_destroy(&tmp);

    if (!entry || !entry->u.id.declared) {
        return true;  /* is undeclared */
    }
    return false;
}

/* ----------------------------------------------------------------
 * Check module (from src/api/check.py)
 * ---------------------------------------------------------------- */

bool is_temporary_value(const AstNode *node) {
    if (!node) return false;

    /* STRING constants are not temporary */
    if (node->tag == AST_STRING) return false;

    /* Variables (ID with class var) are not temporary */
    if (node->tag == AST_ID && node->u.id.class_ == CLASS_var) return false;

    /* Nodes with t starting with "_" or "#" are not temporary */
    if (node->t && (node->t[0] == '_' || node->t[0] == '#'))
        return false;

    return true;
}

/* ----------------------------------------------------------------
 * Check predicates (from src/api/check.py)
 *
 * These mirror the Python check module's is_* functions exactly.
 * ---------------------------------------------------------------- */

bool check_is_number(const AstNode *node) {
    /* Python: is_number() — NUMBER or CONST with numeric type */
    if (!node) return false;
    if (node->tag == AST_NUMBER) return true;
    if (node->tag == AST_ID && node->u.id.class_ == CLASS_const
        && node->type_ && type_is_numeric(node->type_))
        return true;
    return false;
}

bool check_is_const(const AstNode *node) {
    /* Python: is_const() — a CONST declaration (CLASS_const) */
    if (!node) return false;
    return node->tag == AST_ID && node->u.id.class_ == CLASS_const;
}

bool check_is_CONST(const AstNode *node) {
    /* Python: is_CONST() — a CONSTEXPR node */
    if (!node) return false;
    return node->tag == AST_CONSTEXPR;
}

bool check_is_static(const AstNode *node) {
    /* Python: is_static() — CONSTEXPR, NUMBER, or CONST */
    if (!node) return false;
    return check_is_CONST(node) || check_is_number(node) || check_is_const(node);
}

/* check.is_dynamic (api/check.py:370-379), single-entry form as called from
 * p_addr_of_id (zxbparser.py:2682):
 *     try:
 *         return not any(i.scope == SCOPE.global_ and i.is_basic
 *                        and i.type_ != string for i in p)
 *     except Exception:
 *         pass
 *     return False
 * CRITICAL: `i.is_basic` is delegated (Id.__getattr__ -> ref) and NO ref
 * (VarRef/LabelRef/FuncRef/...) actually defines `is_basic`, so reading it
 * ALWAYS raises AttributeError. The generator's `and` short-circuits on
 * `i.scope == global_` first:
 *   - scope != global  -> condition is False, is_basic never read, no raise
 *                         -> any()==False -> is_dynamic == True  (bare UNARY)
 *   - scope == global  -> is_basic is read -> raises -> except -> return False
 *                         -> is_dynamic == False (CONSTEXPR-wrapped)
 * The is_basic / type_!=string sub-conditions are therefore DEAD: a GLOBAL
 * entry of ANY class (var incl. string, label, function, sub) is is_dynamic
 * == False and gets CONSTEXPR-wrapped; only a local/param entry stays a bare
 * UNARY. Probed against the Python oracle (opt2 `@<sub>` -> False; `@<local>`
 * -> True; `@<global string>` -> False). The prior `type_is_basic` form
 * wrongly returned True for `@<sub>`/`@<function>` (non-basic type), leaving a
 * runtime UNARY that emitted an extra push/pop vs Python's folded
 * `storeu16 #_<name>`. */
bool check_is_dynamic(const AstNode *entry) {
    if (!entry || entry->tag != AST_ID)
        return true;
    return entry->u.id.scope != SCOPE_global;
}

/* LabelRef.accessed setter cascade (labelref.py:48-55) + the
 * scope_owner.setter "refresh" (labelref.py:42-45): mark the label
 * entry accessed and propagate to every captured scope_owner function
 * entry (so a SUB/FUNCTION whose only use is `@label` is not
 * O>1-pruned). Idempotent. */
void mark_label_accessed(AstNode *label) {
    if (!label || label->tag != AST_ID)
        return;
    label->u.id.accessed = true;
    for (int i = 0; i < label->u.id.scope_owner_count; i++) {
        AstNode *fn = label->u.id.scope_owner[i];
        if (fn)
            fn->u.id.accessed = true;
    }
}

/* symboltable.access_label scope_owner capture (symboltable.py:621-623):
 *   if gl.FUNCTION_LEVEL: entry.ref.scope_owner = list(gl.FUNCTION_LEVEL)
 * Snapshot the current function_level stack onto the label entry; the
 * setter refreshes accessed if the label was already accessed
 * (labelref.py:45 -> :48-55). Called at every label def/access site. */
void label_capture_scope_owner(CompilerState *cs, AstNode *label) {
    if (!label || label->tag != AST_ID)
        return;
    int n = (int)cs->function_level.len;
    if (n <= 0)
        return;
    AstNode **so = arena_alloc(&cs->arena, (size_t)n * sizeof(AstNode *));
    for (int i = 0; i < n; i++)
        so[i] = cs->function_level.data[i];
    label->u.id.scope_owner = so;
    label->u.id.scope_owner_count = n;
    /* labelref.py:45 — scope_owner.setter re-fires accessed. */
    if (label->u.id.accessed)
        mark_label_accessed(label);
}

bool check_is_numeric(const AstNode *a, const AstNode *b) {
    /* Python: is_numeric(a, b) — both have numeric type */
    if (!a || !b) return false;
    return type_is_numeric(a->type_) && type_is_numeric(b->type_);
}

bool check_is_string_node(const AstNode *a, const AstNode *b) {
    /* Python: is_string(a, b) — both are STRING constants */
    if (!a || !b) return false;
    bool a_str = (a->tag == AST_STRING) ||
                 (check_is_const(a) && type_is_string(a->type_));
    bool b_str = (b->tag == AST_STRING) ||
                 (check_is_const(b) && type_is_string(b->type_));
    return a_str && b_str;
}

/* Resolve a string-constant operand to its underlying AST_STRING value node.
 * Mirrors Python ConstRef.value (constref.py:35-40): for a CLASS.const id of
 * string type, the stored value is the declared STRING (folded onto the
 * entry's default_value at declaration). A bare AST_STRING resolves to
 * itself. Returns NULL for anything else (caller already gated via
 * check_is_string_node, so this is the value-extraction half). */
const AstNode *const_string_value_node(const AstNode *n) {
    if (!n) return NULL;
    if (n->tag == AST_STRING) return n;
    if (n->tag == AST_ID && n->u.id.class_ == CLASS_const) {
        const AstNode *dv = n->u.id.default_value_expr;
        /* Unwrap CONSTEXPR/TYPECAST wrappers defensively (a string const's
         * value is the bare STRING, but stay 1:1 with the numeric folder's
         * chain-unwrap above). */
        while (dv && (dv->tag == AST_CONSTEXPR || dv->tag == AST_TYPECAST) &&
               dv->child_count > 0)
            dv = dv->children[0];
        if (dv && dv->tag == AST_STRING) return dv;
    }
    return NULL;
}

bool check_is_null(const AstNode *node) {
    if (!node) return true;
    if (node->tag == AST_NOP) return true;
    if (node->tag == AST_BLOCK) {
        for (int i = 0; i < node->child_count; i++) {
            if (!check_is_null(node->children[i]))
                return false;
        }
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------
 * common_type — Type promotion (from src/api/check.py)
 *
 * Returns the "wider" type for a binary operation.
 * Matches Python's check.common_type() exactly.
 * ---------------------------------------------------------------- */

TypeInfo *check_common_type(CompilerState *cs, const AstNode *a, const AstNode *b) {
    if (!a || !b) return NULL;

    TypeInfo *at = a->type_;
    TypeInfo *bt = b->type_;
    if (!at || !bt) return NULL;

    /* Resolve through aliases/refs */
    const TypeInfo *af = at->final_type ? at->final_type : at;
    const TypeInfo *bf = bt->final_type ? bt->final_type : bt;

    if (type_equal(at, bt)) return at;

    SymbolTable *st = cs->symbol_table;

    /* unknown + unknown => default type */
    if (af->basic_type == TYPE_unknown && bf->basic_type == TYPE_unknown)
        return st->basic_types[cs->default_type->basic_type];

    if (af->basic_type == TYPE_unknown) return bt;
    if (bf->basic_type == TYPE_unknown) return at;

    /* float wins */
    if (af->basic_type == TYPE_float || bf->basic_type == TYPE_float)
        return st->basic_types[TYPE_float];

    /* fixed wins */
    if (af->basic_type == TYPE_fixed || bf->basic_type == TYPE_fixed)
        return st->basic_types[TYPE_fixed];

    /* string + non-string => unknown (error) */
    if (af->basic_type == TYPE_string || bf->basic_type == TYPE_string)
        return st->basic_types[TYPE_unknown];

    /* Both integral: larger size wins, signed if either is signed */
    TypeInfo *result = (type_size(at) > type_size(bt)) ? at : bt;
    BasicType rbt = result->final_type ? result->final_type->basic_type : result->basic_type;

    if (!basictype_is_unsigned(af->basic_type) || !basictype_is_unsigned(bf->basic_type)) {
        BasicType signed_bt = basictype_to_signed(rbt);
        result = st->basic_types[signed_bt];
    }

    return result;
}

/* ----------------------------------------------------------------
 * make_typecast — Insert a TYPECAST node (from src/symbols/typecast.py)
 *
 * Returns the node (possibly modified) or NULL on error.
 * ---------------------------------------------------------------- */

AstNode *make_typecast(CompilerState *cs, TypeInfo *new_type, AstNode *node, int lineno) {
    if (!node) return NULL;
    if (!new_type) return node;

    /* Same type — no cast needed. type_equal resolves through final_type
     * (mirrors Python SymbolTYPE.__eq__) — the only "skip the cast"
     * condition Python's SymbolTYPECAST.make_node has. A target-side
     * basic_type==TYPE_unknown guard must NOT live here: declared types
     * are TYPEREF wrappers whose own basic_type is TYPE_unknown (the
     * resolved type is in final_type), so it would wrongly skip the cast
     * for every DIM'd variable (the FOR/LET TYPECAST drift). */
    if (type_equal(new_type, node->type_))
        return node;

    /* Source type not yet resolved — skip. Retained as an accommodation
     * for the C port lacking Python's universal NOTYPE sentinel (Python
     * make_node has no analogue); removing it faithfully requires
     * guaranteeing every node carries a non-NULL type_, which is outside
     * Phase 1's measured scope. */
    if (!node->type_)
        return node;

    /* Array type mismatch */
    if (node->tag == AST_ID && node->u.id.class_ == CLASS_array) {
        if (type_size(new_type) == type_size(node->type_) &&
            !type_is_string(new_type) && !type_is_string(node->type_))
            return node;
        zxbc_error(cs, lineno, "Array %s type does not match parameter type",
                   node->u.id.name);
        return NULL;
    }

    TypeInfo *str_type = cs->symbol_table->basic_types[TYPE_string];

    /* Cannot convert string <-> number */
    if (type_equal(node->type_, str_type)) {
        zxbc_error(cs, lineno, "Cannot convert string to a value. Use VAL() function");
        return NULL;
    }
    if (type_equal(new_type, str_type)) {
        zxbc_error(cs, lineno, "Cannot convert value to string. Use STR() function");
        return NULL;
    }

    /* If it's a CONSTEXPR, cast the inner expression. Python's
     * SymbolCONSTEXPR.type_ is a @property returning self.expr.type_
     * (constexpr.py:36-38), so the outer node's effective type tracks
     * the inner expression. The C AST stores type_ as a fixed field —
     * mirror the property by writing the cast's new_type back onto the
     * CONSTEXPR after re-typing the inner expr; without this, downstream
     * visitors (e.g. visit_POKE -> ic_store(ch1.type_, ...)) read the
     * stale pre-cast type and pick the wrong store-width
     * (storeu16 vs storeu32 for ulong-cast values). */
    if (check_is_CONST(node)) {
        if (node->child_count > 0) {
            node->children[0] = make_typecast(cs, new_type, node->children[0], lineno);
        }
        if (node->child_count > 0 && node->children[0])
            node->type_ = node->children[0]->type_;
        else
            node->type_ = new_type;
        return node;
    }

    /* If it's not a number or const, wrap in TYPECAST */
    if (!check_is_number(node) && !check_is_const(node)) {
        AstNode *cast = ast_new(cs, AST_TYPECAST, lineno);
        cast->type_ = new_type;
        ast_add_child(cs, cast, node);
        return cast;
    }

    /* It's a number — perform static conversion */
    /* If it was a named CONST, convert to NUMBER for folding */
    if (check_is_const(node)) {
        /* Treat as number with same value */
        /* For now, just update the type directly */
    }

    TypeInfo *bool_type = cs->symbol_table->basic_types[TYPE_boolean];
    if (type_equal(new_type, bool_type)) {
        /* Boolean cast: non-zero => 1 */
        if (node->tag == AST_NUMBER) {
            node->u.number.value = (node->u.number.value != 0) ? 1 : 0;
        }
        new_type = cs->symbol_table->basic_types[TYPE_ubyte]; /* externally ubyte */
    } else {
        const TypeInfo *nf = new_type->final_type ? new_type->final_type : new_type;
        if (nf->tag == AST_BASICTYPE && !basictype_is_integral(nf->basic_type)) {
            /* Float/fixed: keep as float value */
            if (node->tag == AST_NUMBER)
                node->u.number.value = (double)node->u.number.value;
        } else if (nf->tag == AST_BASICTYPE) {
            /* Integer: mask to target size (typecast.py:93-103). Python
             * computes new_val = int(node.value) & mask, then compares the
             * ORIGINAL node.value against new_val to decide whether to
             * narrow + warn. The comparison MUST use the original (possibly
             * fractional) value, not the already-truncated int: a fixed/float
             * constant like 0.2 truncates to int 0 == (0 & mask), so an
             * `ival != new_val` test is false and the node keeps value 0.2
             * with an integer type — emitted as a runtime conversion
             * (`pop af`) instead of the folded `xor a`/0 Python produces
             * (const_expr: g/5 == 0.2 → ubyte 0). Compare orig_val. */
            int sz = basictype_size(nf->basic_type);
            if (node->tag == AST_NUMBER && sz > 0) {
                double orig_val = node->u.number.value;
                int64_t ival = (int64_t)orig_val;
                int64_t mask = ((int64_t)1 << (8 * sz)) - 1;
                int64_t new_val = ival & mask;

                if (orig_val >= 0 && orig_val != (double)new_val) {
                    warn_conversion_lose_digits(cs, lineno);
                    node->u.number.value = (double)new_val;
                } else if (orig_val < 0 &&
                           (double)(1LL << (sz * 8)) + orig_val != (double)new_val) {
                    warn_conversion_lose_digits(cs, lineno);
                    node->u.number.value = (double)(new_val - (1LL << (sz * 8)));
                }
            }
        }
    }

    /* Python SymbolNUMBER.t is a @property == str(self.value) (number.py:
     * 72-74), recomputed on every read — so a value/type mutation above is
     * always reflected at emit time. The C caches .t (set at ast_number and
     * tr_visit_number). Invalidate it here for a mutated NUMBER so
     * tr_visit_number re-renders from the new value+type; otherwise a
     * narrowed constant (0.2 -> ubyte 0) keeps its stale "0.2" string and
     * emits a runtime conversion (const_expr g/5). */
    if (node->tag == AST_NUMBER)
        node->t = NULL;

    node->type_ = new_type;
    return node;
}

/* ----------------------------------------------------------------
 * make_binary_node — Create a BINARY expression (from src/symbols/binary.py)
 *
 * Handles type coercion, constant folding, CONSTEXPR wrapping,
 * and string concatenation. Matches Python's SymbolBINARY.make_node().
 * ---------------------------------------------------------------- */

/* Helper: try constant folding for numeric binary ops */
static bool fold_numeric(const char *op, double lv, double rv, double *result) {
    if (strcmp(op, "PLUS") == 0)       { *result = lv + rv; return true; }
    if (strcmp(op, "MINUS") == 0)      { *result = lv - rv; return true; }
    if (strcmp(op, "MULT") == 0 || strcmp(op, "MUL") == 0) { *result = lv * rv; return true; }
    if (strcmp(op, "DIV") == 0) {
        if (rv == 0) return false;
        *result = lv / rv; return true;
    }
    if (strcmp(op, "MOD") == 0) {
        if (rv == 0) return false;
        *result = fmod(lv, rv); return true;
    }
    if (strcmp(op, "POW") == 0)        { *result = pow(lv, rv); return true; }
    if (strcmp(op, "LT") == 0)         { *result = (lv < rv) ? 1 : 0; return true; }
    if (strcmp(op, "GT") == 0)         { *result = (lv > rv) ? 1 : 0; return true; }
    if (strcmp(op, "EQ") == 0)         { *result = (lv == rv) ? 1 : 0; return true; }
    if (strcmp(op, "LE") == 0)         { *result = (lv <= rv) ? 1 : 0; return true; }
    if (strcmp(op, "GE") == 0)         { *result = (lv >= rv) ? 1 : 0; return true; }
    if (strcmp(op, "NE") == 0)         { *result = (lv != rv) ? 1 : 0; return true; }
    if (strcmp(op, "AND") == 0)        { *result = ((int64_t)lv && (int64_t)rv) ? 1 : 0; return true; }
    if (strcmp(op, "OR") == 0)         { *result = ((int64_t)lv || (int64_t)rv) ? 1 : 0; return true; }
    if (strcmp(op, "XOR") == 0)        { *result = ((!!(int64_t)lv) ^ (!!(int64_t)rv)) ? 1 : 0; return true; }
    if (strcmp(op, "BAND") == 0)       { *result = (double)((int64_t)lv & (int64_t)rv); return true; }
    if (strcmp(op, "BOR") == 0)        { *result = (double)((int64_t)lv | (int64_t)rv); return true; }
    if (strcmp(op, "BXOR") == 0)       { *result = (double)((int64_t)lv ^ (int64_t)rv); return true; }
    if (strcmp(op, "SHL") == 0)        { *result = (double)((int64_t)lv << (int64_t)rv); return true; }
    if (strcmp(op, "SHR") == 0)        { *result = (double)((int64_t)lv >> (int64_t)rv); return true; }
    return false;
}

static bool is_boolean_op(const char *op) {
    return strcmp(op, "AND") == 0 || strcmp(op, "OR") == 0 || strcmp(op, "XOR") == 0;
}

static bool is_comparison_op(const char *op) {
    return strcmp(op, "LT") == 0 || strcmp(op, "GT") == 0 || strcmp(op, "EQ") == 0 ||
           strcmp(op, "LE") == 0 || strcmp(op, "GE") == 0 || strcmp(op, "NE") == 0;
}

static bool is_bitwise_op(const char *op) {
    return strcmp(op, "BAND") == 0 || strcmp(op, "BOR") == 0 ||
           strcmp(op, "BXOR") == 0 || strcmp(op, "BNOT") == 0;
}

static bool is_string_forbidden_op(const char *op) {
    return strcmp(op, "BAND") == 0 || strcmp(op, "BOR") == 0 ||
           strcmp(op, "BXOR") == 0 || strcmp(op, "AND") == 0 ||
           strcmp(op, "OR") == 0 || strcmp(op, "XOR") == 0 ||
           strcmp(op, "MINUS") == 0 || strcmp(op, "MULT") == 0 || strcmp(op, "MUL") == 0 ||
           strcmp(op, "DIV") == 0 || strcmp(op, "SHL") == 0 ||
           strcmp(op, "SHR") == 0;
}

AstNode *make_binary_node(CompilerState *cs, const char *operator, AstNode *left,
                          AstNode *right, int lineno, TypeInfo *type_) {
    if (!left || !right) return NULL;

    /* If either operand has no type, skip all type coercion and just build the node.
     * This happens for builtins, function calls, etc. that don't have types
     * resolved yet during parse-only. */
    if (!left->type_ || !right->type_) {
        AstNode *binary = ast_new(cs, AST_BINARY, lineno);
        binary->u.binary.operator = arena_strdup(&cs->arena, operator);
        ast_add_child(cs, binary, left);
        ast_add_child(cs, binary, right);
        binary->type_ = type_ ? type_ : (left->type_ ? left->type_ : right->type_);
        return binary;
    }

    SymbolTable *st = cs->symbol_table;
    TypeInfo *bool_type = st->basic_types[TYPE_boolean];
    TypeInfo *ubyte_type = st->basic_types[TYPE_ubyte];

    /* String-forbidden operators */
    if (is_string_forbidden_op(operator) && !check_is_numeric(left, right)) {
        zxbc_error(cs, lineno, "Operator %s cannot be used with strings", operator);
        return NULL;
    }

    /* Non-boolean operators: coerce boolean operands to ubyte */
    if (!is_boolean_op(operator)) {
        if (type_equal(left->type_, bool_type))
            left = make_typecast(cs, ubyte_type, left, lineno);
        if (type_equal(right->type_, bool_type))
            right = make_typecast(cs, ubyte_type, right, lineno);
    }

    TypeInfo *c_type = check_common_type(cs, left, right);

    /* Constant folding for numeric types */
    if (c_type && type_is_numeric(c_type)) {
        if (check_is_numeric(left, right) &&
            (check_is_const(left) || check_is_number(left)) &&
            (check_is_const(right) || check_is_number(right))) {
            /* Both are compile-time numbers — fold. Bug C: a named CONST
             * is still an AST_ID after make_typecast (line 759-762 only
             * marks it as numeric-castable; tag stays AST_ID). Python
             * folds via `a.value` which on a CONST returns the stored
             * numeric value directly (symbols/binary.py:111). Mirror
             * faithfully: a CLASS_const id resolves to its NUMBER-valued
             * default_value_expr (chain through CONSTEXPR if wrapped). */
            AstNode *cl = make_typecast(cs, c_type, left, lineno);
            AstNode *cr = make_typecast(cs, c_type, right, lineno);
            double lv = 0, rv = 0;
            bool lvok = false, rvok = false;
            if (cl && cl->tag == AST_NUMBER) { lv = cl->u.number.value; lvok = true; }
            else if (cl && cl->tag == AST_ID &&
                     cl->u.id.class_ == CLASS_const &&
                     cl->u.id.default_value_expr) {
                AstNode *dv = cl->u.id.default_value_expr;
                while (dv && dv->tag == AST_CONSTEXPR && dv->child_count > 0)
                    dv = dv->children[0];
                while (dv && dv->tag == AST_TYPECAST && dv->child_count > 0)
                    dv = dv->children[0];
                if (dv && dv->tag == AST_NUMBER) { lv = dv->u.number.value; lvok = true; }
            }
            if (cr && cr->tag == AST_NUMBER) { rv = cr->u.number.value; rvok = true; }
            else if (cr && cr->tag == AST_ID &&
                     cr->u.id.class_ == CLASS_const &&
                     cr->u.id.default_value_expr) {
                AstNode *dv = cr->u.id.default_value_expr;
                while (dv && dv->tag == AST_CONSTEXPR && dv->child_count > 0)
                    dv = dv->children[0];
                while (dv && dv->tag == AST_TYPECAST && dv->child_count > 0)
                    dv = dv->children[0];
                if (dv && dv->tag == AST_NUMBER) { rv = dv->u.number.value; rvok = true; }
            }
            if (lvok && rvok) {
                double result;
                if (fold_numeric(operator, lv, rv, &result)) {
                    return ast_number(cs, result, lineno);
                }
            }
        }

        /* Static expressions → wrap in CONSTEXPR */
        if (check_is_static(left) && check_is_static(right)) {
            AstNode *cl = make_typecast(cs, c_type, left, lineno);
            AstNode *cr = make_typecast(cs, c_type, right, lineno);
            if (cl && cr) {
                AstNode *binary = ast_new(cs, AST_BINARY, lineno);
                binary->u.binary.operator = arena_strdup(&cs->arena, operator);
                ast_add_child(cs, binary, cl);
                ast_add_child(cs, binary, cr);
                binary->type_ = type_ ? type_ : c_type;

                AstNode *constexpr = ast_new(cs, AST_CONSTEXPR, lineno);
                ast_add_child(cs, constexpr, binary);
                constexpr->type_ = binary->type_;
                return constexpr;
            }
        }
    }

    /* String constant folding (concatenation). Python binary.py:118-120:
     *   if check.is_string(a, b) and func is not None:   # both string-CONST
     *       if operator == "PLUS":
     *           return SymbolSTRING(func(a.value, b.value), lineno)
     * `check.is_string` (check.py:286-290) is true for a bare STRING node OR
     * a CLASS.const id of string type — both resolve via ConstRef.value
     * (constref.py:35-40 -> the stored STRING's .value). So a `LET cs =
     * xs + ys` with two string CONSTs folds to ONE static STRING at compile
     * time (one __LABEL data row + ld de,LBL/__STORE_STR), not a runtime
     * __ADDSTR. The prior C gate only folded literal STRING<>STRING; a
     * const-string operand (AST_ID) slipped through to the runtime path.
     * (Only PLUS is reachable: the comparison branch at binary.py:122 uses
     * a.text, which SymbolSTRING does not define — dead for STRING constants
     * — so no string-comparison fold is ported.) NUL-safe: copy by the
     * tracked .length, never strlen/strcat. */
    if (check_is_string_node(left, right) && strcmp(operator, "PLUS") == 0) {
        const AstNode *ls = const_string_value_node(left);
        const AstNode *rs = const_string_value_node(right);
        if (ls && rs) {
            int llen = ls->u.string.length;
            int rlen = rs->u.string.length;
            const char *lv = ls->u.string.value ? ls->u.string.value : "";
            const char *rv = rs->u.string.value ? rs->u.string.value : "";
            size_t len = (size_t)llen + (size_t)rlen;
            char *buf = arena_alloc(&cs->arena, len + 1);
            memcpy(buf, lv, (size_t)llen);
            memcpy(buf + llen, rv, (size_t)rlen);
            buf[len] = '\0';
            AstNode *s = ast_new(cs, AST_STRING, lineno);
            s->u.string.value = buf;
            s->u.string.length = (int)len;
            s->type_ = st->basic_types[TYPE_string];
            return s;
        }
    }

    /* Bitwise ops with decimal type → promote to long */
    if (is_bitwise_op(operator) && c_type && basictype_is_decimal(
            c_type->final_type ? c_type->final_type->basic_type : c_type->basic_type)) {
        c_type = st->basic_types[TYPE_long];
    }

    /* String type mismatch: set c_type to the first operand's type (will error) */
    if (left->type_ != right->type_ &&
        (type_is_string(left->type_) || type_is_string(right->type_))) {
        c_type = left->type_;
    }

    /* Apply typecast to both operands (except SHL/SHR) */
    if (strcmp(operator, "SHR") != 0 && strcmp(operator, "SHL") != 0) {
        left = make_typecast(cs, c_type, left, lineno);
        right = make_typecast(cs, c_type, right, lineno);
    }

    if (!left || !right) return NULL;

    /* Determine result type */
    if (!type_) {
        if (is_comparison_op(operator) || is_boolean_op(operator))
            type_ = bool_type;
        else
            type_ = c_type;
    }

    /* Create the BINARY node */
    AstNode *binary = ast_new(cs, AST_BINARY, lineno);
    binary->u.binary.operator = arena_strdup(&cs->arena, operator);
    ast_add_child(cs, binary, left);
    ast_add_child(cs, binary, right);
    binary->type_ = type_;
    return binary;
}

/* ----------------------------------------------------------------
 * make_unary_node — Create a UNARY expression (from src/symbols/unary.py)
 * ---------------------------------------------------------------- */

AstNode *make_unary_node(CompilerState *cs, const char *operator, AstNode *operand,
                         int lineno) {
    if (!operand) return NULL;

    /* Constant folding for MINUS — src/symbols/unary.py:67-69 returns a
     * fresh SymbolNUMBER(func(value)) whose type_ is re-inferred from the
     * NEW value (the SymbolNUMBER ctor auto-types). Just flipping the
     * sign in place keeps the OLD type — so MINUS(NUMBER(1) as ubyte)
     * stays ubyte instead of becoming byte, mistyping DATA -1 (readokup
     * / readokdown DEFB 3 vs 2) and any downstream TSUFFIX-keyed quad. */
    if (strcmp(operator, "MINUS") == 0 && operand->tag == AST_NUMBER) {
        return ast_number(cs, -operand->u.number.value, lineno);
    }

    /* Constant folding for NOT */
    if (strcmp(operator, "NOT") == 0 && operand->tag == AST_NUMBER) {
        operand->u.number.value = (operand->u.number.value == 0) ? 1 : 0;
        operand->type_ = cs->symbol_table->basic_types[TYPE_ubyte];
        return operand;
    }

    /* Constant folding for BNOT */
    if (strcmp(operator, "BNOT") == 0 && operand->tag == AST_NUMBER) {
        operand->u.number.value = (double)(~(int64_t)operand->u.number.value);
        return operand;
    }

    /* Type of the result (mirrors src/symbols/unary.py:make_node 73-83):
     *   - default: operand.type_
     *   - MINUS over an unsigned operand: type promotes to signed and the
     *     operand is wrapped in a TYPECAST to that signed type
     *   - NOT: boolean
     * Without the MINUS promotion, ic_neg later emits e.g. 'negbool'
     * which has no QUAD_TABLE entry; with it, MINUS over boolean/ubyte
     * emits 'negi8' (the byte signed neg) — matching Python. */
    TypeInfo *result_type = operand->type_;
    if (strcmp(operator, "MINUS") == 0 && operand->type_) {
        BasicType obt = resolve_basic_type(operand->type_);
        if (basictype_is_unsigned(obt)) {
            BasicType sbt = basictype_to_signed(obt);
            TypeInfo *signed_t = cs->symbol_table->basic_types[sbt];
            operand = make_typecast(cs, signed_t, operand, lineno);
            if (!operand) return NULL;
            result_type = signed_t;
        }
    } else if (strcmp(operator, "NOT") == 0) {
        result_type = cs->symbol_table->basic_types[TYPE_boolean];
    }

    AstNode *n = ast_new(cs, AST_UNARY, lineno);
    n->u.unary.operator = arena_strdup(&cs->arena, operator);
    ast_add_child(cs, n, operand);
    n->type_ = result_type;
    return n;
}

/* ----------------------------------------------------------------
 * Symbol table access methods (from src/api/symboltable/symboltable.py)
 * ---------------------------------------------------------------- */

AstNode *symboltable_access_id(SymbolTable *st, CompilerState *cs,
                                const char *name, int lineno,
                                TypeInfo *default_type, SymbolClass default_class) {
    /* Handle deprecated suffix: a$ → string type, a% → integer type, etc.
     * Strip suffix for lookup, but use it to infer type.
     * Matches Python's declare_safe() suffix handling. */
    size_t len = strlen(name);
    const char *lookup_name = name;
    TypeInfo *suffix_type = NULL;
    char stripped_buf[256];

    if (len > 0 && is_deprecated_suffix(name[len - 1])) {
        BasicType bt = suffix_to_type(name[len - 1]);
        suffix_type = st->basic_types[bt];
        /* Strip suffix for symbol table lookup */
        if (len < sizeof(stripped_buf)) {
            memcpy(stripped_buf, name, len - 1);
            stripped_buf[len - 1] = '\0';
            lookup_name = stripped_buf;
        }
    }

    /* Check --explicit mode: report error but continue (matching Python).
     * Use "variable" classname for var/unknown context, "identifier" for others. */
    if (cs->opts.explicit_) {
        const char *classname = (default_class == CLASS_var || default_class == CLASS_unknown)
                                ? "variable" : "identifier";
        symboltable_check_is_declared(st, lookup_name, lineno, classname, true, cs);
    }

    AstNode *result = symboltable_lookup(st, lookup_name);
    if (!result) {
        /* Implicit declaration. Python access_id (symboltable.py:349-350)
         * uses DEFAULT_IMPLICIT_TYPE (= TYPE.unknown) when no default_type
         * is given AND default_class is CLASS_unknown (the @X path); for
         * CLASS_var / CLASS_function reads in `default_class != unknown`
         * contexts the existing cs->default_type fallback continues to
         * fire (p_id_expr promotes auto→DEFAULT_TYPE separately in
         * Python). Narrow gate avoids the read-side regression that the
         * blanket TYPE_unknown change caused. */
        if (suffix_type) {
            default_type = suffix_type;
        } else if (!default_type) {
            if (default_class == CLASS_unknown)
                default_type = type_new_ref(cs,
                                            st->basic_types[TYPE_unknown],
                                            lineno, true);
            else
                default_type = type_new_ref(cs, cs->default_type, lineno, true);
        }
        /* Strict mode: error if type was implicitly inferred */
        if (cs->opts.strict && default_type && default_type->implicit) {
            zxbc_error(cs, lineno, "strict mode: missing type declaration for '%s'", name);
        }
        result = symboltable_declare(st, cs, lookup_name, lineno, default_class);
        result->type_ = default_type;
        result->u.id.declared = false; /* implicitly declared */
        return result;
    }

    /* Entry exists. If its type is unknown and we have a default, update */
    if (default_type && result->type_ &&
        result->type_->final_type &&
        result->type_->final_type->basic_type == TYPE_unknown) {
        /* Boolean → ubyte for storage */
        if (default_type->final_type &&
            default_type->final_type->basic_type == TYPE_boolean) {
            default_type = st->basic_types[TYPE_ubyte];
        }
        result->type_ = default_type;
        warn_implicit_type(cs, lineno, name, default_type->name);
    }

    return result;
}

/* access_id(..., ignore_explicit_flag=True) variant — p_addr_of_id
 * (zxbparser.py:2670) accesses the @-operand with the explicit-flag
 * check suppressed (the `@` operator ignores #pragma explicit). Python
 * threads ignore_explicit_flag through access_id; the C explicit gate
 * keys off cs->opts.explicit_, so mask it for this single call only and
 * restore it (faithful net effect: the check_is_declared_explicit call
 * at symboltable.py:345 is skipped). */
AstNode *symboltable_access_id_noexplicit(SymbolTable *st, CompilerState *cs,
                                          const char *name, int lineno,
                                          TypeInfo *default_type,
                                          SymbolClass default_class) {
    bool saved = cs->opts.explicit_;
    cs->opts.explicit_ = false;
    AstNode *r = symboltable_access_id(st, cs, name, lineno,
                                       default_type, default_class);
    cs->opts.explicit_ = saved;
    return r;
}

AstNode *symboltable_access_var(SymbolTable *st, CompilerState *cs,
                                 const char *name, int lineno, TypeInfo *default_type) {
    AstNode *result = symboltable_access_id(st, cs, name, lineno, default_type, CLASS_var);
    if (!result) return NULL;

    /* Check class — const, array, function, sub are also readable in var context.
     * In ZX BASIC, function names are used as variables for return values,
     * and arrays are accessible as variables in contexts like LBOUND(). */
    if (result->u.id.class_ != CLASS_unknown && result->u.id.class_ != CLASS_var &&
        result->u.id.class_ != CLASS_const && result->u.id.class_ != CLASS_array &&
        result->u.id.class_ != CLASS_function && result->u.id.class_ != CLASS_sub) {
        syntax_error_unexpected_class(cs, lineno, name,
                                      result->u.id.class_, CLASS_var);
        return NULL;
    }

    if (result->u.id.class_ == CLASS_unknown)
        result->u.id.class_ = CLASS_var;

    return result;
}

AstNode *symboltable_access_func(SymbolTable *st, CompilerState *cs,
                                  const char *name, int lineno, TypeInfo *default_type) {
    AstNode *result = symboltable_lookup(st, name);
    if (!result) {
        /* Implicit function declaration */
        if (!default_type) {
            default_type = type_new_ref(cs, cs->default_type, lineno, true);
        }
        result = symboltable_declare(st, cs, name, lineno, CLASS_unknown);
        result->type_ = default_type;
        result->u.id.declared = false;
        return result;
    }

    if (result->u.id.class_ != CLASS_function && result->u.id.class_ != CLASS_sub &&
        result->u.id.class_ != CLASS_unknown) {
        syntax_error_unexpected_class(cs, lineno, name,
                                      result->u.id.class_, CLASS_function);
        return NULL;
    }

    return result;
}

AstNode *symboltable_access_call(SymbolTable *st, CompilerState *cs,
                                  const char *name, int lineno, TypeInfo *type_) {
    AstNode *entry = symboltable_access_id(st, cs, name, lineno, type_, CLASS_unknown);
    if (!entry) {
        return symboltable_access_func(st, cs, name, lineno, NULL);
    }

    /* Check if callable: function/sub/array are always callable.
     * CLASS_unknown might be a forward-declared function — allow it.
     * Variables/constants are only callable if they're strings (string slicing). */
    SymbolClass cls = entry->u.id.class_;
    if (cls == CLASS_function || cls == CLASS_sub || cls == CLASS_array) {
        return entry;
    }

    /* Variables/constants: callable only if string type (slicing) */
    if (cls == CLASS_var || cls == CLASS_const) {
        if (type_is_string(entry->type_)) {
            return entry;
        }
        err_not_array_nor_func(cs, lineno, name);
        return NULL;
    }

    /* CLASS_unknown: could be a forward-declared function. Allow it —
     * matching Python's make_call/CALL.make_node which uses access_func
     * (not access_call) for statement-level calls. */
    return entry;
}

AstNode *symboltable_access_array(SymbolTable *st, CompilerState *cs,
                                   const char *name, int lineno, TypeInfo *default_type) {
    if (!symboltable_check_is_declared(st, name, lineno, "array", true, cs))
        return NULL;

    AstNode *result = symboltable_lookup(st, name);
    if (!result) return NULL;

    if (result->u.id.class_ != CLASS_array && result->u.id.class_ != CLASS_unknown) {
        syntax_error_unexpected_class(cs, lineno, name,
                                      result->u.id.class_, CLASS_array);
        return NULL;
    }

    return result;
}

/* current_data_label (src/api/utils.py:110-114):
 *   f"{global_.DATAS_NAMESPACE}.__DATA__{len(global_.DATAS)}"
 * DATAS_NAMESPACE == ".DATA" (global_.py:134). */
char *current_data_label(CompilerState *cs) {
    char buf[48];
    snprintf(buf, sizeof(buf), ".DATA.__DATA__%d", (int)cs->datas.len);
    return arena_strdup(&cs->arena, buf);
}

/* make_label's DATA_LABELS side-effect (src/zxbc/zxbparser.py:452-458):
 * "gl.DATA_LABELS[id_] = gl.DATA_PTR_CURRENT — this label points to the
 * current DATA block index." Python's make_label is called from p_label,
 * the bare-ID label-ref path, and p_data. The C parser already owns the
 * declare_label/access bookkeeping at each of those sites, so the
 * faithful port adds ONLY the DATA_LABELS write inline at each (the
 * entry is already in hand there — no extra declare). No standalone
 * make_label helper is introduced (it would re-run access_label and
 * double-touch the entry); see parser.c BTOK_DATA + the two
 * label-declaration paths. current_data_label() below is the
 * gl.DATA_PTR_CURRENT producer they read. */

AstNode *symboltable_access_label(SymbolTable *st, CompilerState *cs,
                                   const char *name, int lineno) {
    AstNode *result = symboltable_lookup(st, name);
    if (!result) {
        /* Labels are always global in ZX BASIC (matching Python's
         * declare_label → move_to_global_scope). Temporarily switch to
         * global scope for declaration, then restore. */
        Scope_ *saved = st->current_scope;
        st->current_scope = st->global_scope;
        result = symboltable_declare(st, cs, name, lineno, CLASS_label);
        st->current_scope = saved;
        result->u.id.declared = false;
        return result;
    }

    if (result->u.id.class_ != CLASS_label && result->u.id.class_ != CLASS_unknown) {
        /* In Python, labels are always global and can coexist with functions/subs
         * (label namespace is separate). Don't error — just return the entry. */
        return result;
    }

    if (result->u.id.class_ == CLASS_unknown)
        result->u.id.class_ = CLASS_label;

    /* access_label -> move_to_global_scope (symboltable.py:289-305, called
     * unconditionally at :625). An existing entry implicitly created in a
     * FUNCTION's local scope by a forward `@label` (p_addr_of_id's
     * access_id, default_class=CLASS.unknown) must be relocated to the
     * global scope here — labels are always global. Without this the
     * entry stays in the (now-popped) function scope and post-parse
     * check_pending_labels' global symboltable_lookup cannot resolve it
     * ("Undeclared label"). Mirror: scope=global_, re-key into the
     * global scope hashmap, drop from the owning local scope. (A label's
     * mangled is left as-is — Python skips the make_child_namespace
     * rename for CLASS.label; the LabelRef-mangled `.LABEL._name` is
     * computed at translate time by tr_label_mangled.) */
    if (result->u.id.scope != SCOPE_global) {
        for (Scope_ *s = st->current_scope; s && s != st->global_scope;
             s = s->parent) {
            if (hashmap_get(&s->symbols, name) == result) {
                scope_del_symbol(s, name);
                break;
            }
        }
        result->u.id.scope = SCOPE_global;
        if (hashmap_get(&st->global_scope->symbols, name) == NULL) {
            scope_set_symbol(st, st->global_scope, name, result);
            if (st->global_scope->ordered_count >=
                st->global_scope->ordered_cap) {
                int nc = st->global_scope->ordered_cap
                             ? st->global_scope->ordered_cap * 2 : 16;
                AstNode **no = arena_alloc(&cs->arena,
                                           (size_t)nc * sizeof(AstNode *));
                if (st->global_scope->ordered_count > 0)
                    memcpy(no, st->global_scope->ordered,
                           (size_t)st->global_scope->ordered_count *
                               sizeof(AstNode *));
                st->global_scope->ordered = no;
                st->global_scope->ordered_cap = nc;
            }
            st->global_scope->ordered[
                st->global_scope->ordered_count++] = result;
        }
    }

    return result;
}

/* ----------------------------------------------------------------
 * Post-parse validation (from p_start in zxbparser.py, check.py)
 * ---------------------------------------------------------------- */

/* check_pending_labels: iteratively traverse AST looking for ID nodes
 * that still have CLASS_unknown in the symbol table.
 * Matches Python's src/api/check.py check_pending_labels().
 *
 * In Python, only nodes with token "ID" or "LABEL" are checked —
 * nodes that were resolved to VAR/FUNCTION/ARRAY have different tokens.
 * In our C code, we only check AST_ID nodes that remain CLASS_unknown
 * or CLASS_label in the symbol table. */
/* A CALL/FUNCCALL callee (child[0]) is the C analogue of a Python token
 * "FUNCTION" node — make_node/to_function flips an undeclared callee's token
 * away from "ID", so Python's check_pending_labels (which acts only on token
 * "ID"/"LABEL") skips it; the call's own undeclared-ness is owned by
 * check_pending_calls' "Undeclared function" instead. The C leaves the callee
 * AST_ID at CLASS_unknown (flipping its class here regressed the
 * forward-nested SUB redefinition path — paramstr5/opt2_* FALSE_POS), so
 * mark each CALL/FUNCCALL callee pointer and skip it in the ID scan below,
 * reproducing the token-"FUNCTION" exemption without mutating the shared
 * symbol entry. */
static bool pl_is_marked(AstNode **marks, int n, const AstNode *p) {
    for (int i = 0; i < n; i++) if (marks[i] == p) return true;
    return false;
}

bool check_pending_labels(CompilerState *cs, AstNode *ast) {
    if (!ast) return true;

    bool result = true;

    /* Iterative traversal to avoid stack overflow on deeply nested ASTs */
    int stack_cap = 256;
    int stack_len = 0;
    AstNode **stack = arena_alloc(&cs->arena, stack_cap * sizeof(AstNode *));
    stack[stack_len++] = ast;

    /* Callee-exemption set (CALL/FUNCCALL child[0] pointers). */
    int callee_cap = 64;
    int callee_len = 0;
    AstNode **callees = arena_alloc(&cs->arena, callee_cap * sizeof(AstNode *));

    while (stack_len > 0) {
        AstNode *node = stack[--stack_len];
        if (!node) continue;

        /* Record a CALL/FUNCCALL callee (child[0]) as token-"FUNCTION"
         * exempt before pushing children. */
        if ((node->tag == AST_CALL || node->tag == AST_FUNCCALL) &&
            node->child_count > 0 && node->children[0] &&
            node->children[0]->tag == AST_ID) {
            if (callee_len >= callee_cap) {
                int nc = callee_cap * 2;
                AstNode **n2 = arena_alloc(&cs->arena, nc * sizeof(AstNode *));
                memcpy(n2, callees, callee_len * sizeof(AstNode *));
                callees = n2; callee_cap = nc;
            }
            callees[callee_len++] = node->children[0];
        }

        /* Push children */
        for (int i = 0; i < node->child_count; i++) {
            if (stack_len >= stack_cap) {
                int new_cap = stack_cap * 2;
                AstNode **new_stack = arena_alloc(&cs->arena, new_cap * sizeof(AstNode *));
                memcpy(new_stack, stack, stack_len * sizeof(AstNode *));
                stack = new_stack;
                stack_cap = new_cap;
            }
            stack[stack_len++] = node->children[i];
        }

        /* Faithful port of Python check_pending_labels (api/check.py:199-230):
         * the traversal acts on nodes whose token is in ("ID", "LABEL"). A
         * RESOLVED variable carries token "VAR" (CLASS_var) and is skipped;
         * an UNRESOLVED scalar reference keeps token "ID" (CLASS_unknown);
         * a label reference carries token "LABEL" (CLASS_label). For each,
         * get_entry(name); if entry is None or its class is still unknown,
         * Python errors. Two distinct C messages are emitted to match the
         * oracle byte-for-byte:
         *   - CLASS_label  -> "Undeclared label"   (the C analogue of
         *     Python's SYMBOL_TABLE.check_labels(), symboltable.py:759-762,
         *     which runs first at zxbparser.py:529 over self.labels and owns
         *     the GOTO/GOSUB undeclared-target message).
         *   - CLASS_unknown -> "Undeclared identifier"  (api/check.py:224 —
         *     an unresolved bare ID, e.g. `@a` / `POKE @a` referencing a
         *     never-declared `a`; the `@` operator's access_id uses
         *     ignore_explicit_flag so it does not auto-declare, leaving the
         *     id CLASS_unknown for this catch — both in #pragma explicit and
         *     in the implicit default mode, exactly as the oracle rejects). */
        if (node->tag != AST_ID) continue;

        if (node->u.id.class_ == CLASS_label) {
            /* Look up in symbol table — the label must be declared somewhere */
            AstNode *entry = symboltable_lookup(cs->symbol_table, node->u.id.name);
            if (!entry || entry->u.id.class_ == CLASS_unknown) {
                /* "First reference wins" lineno (Python `check_labels`,
                 * symboltable.py:759-762, runs from p_start at :529 BEFORE
                 * check_pending_labels). check_labels iterates self.labels
                 * and reports via `check_is_declared(name, entry.lineno,
                 * "label")` — so the error site is entry.lineno (the
                 * lineno captured on FIRST creation), NOT the lineno of
                 * the AST_ID node currently being visited. The entry's
                 * lineno was stamped at first reference (bare-ID via
                 * access_func, forward `@label` via access_id, or the
                 * first access_label call itself when GOTO/GOSUB-only)
                 * and never overwritten by later references. Mirror via
                 * entry->lineno when an entry exists; fall back to
                 * node->lineno only when no symbol-table entry was ever
                 * created (Python's check_pending_labels-style path for a
                 * bare LABEL ref whose only mention was the GOTO/GOSUB
                 * — which itself doesn't symbol-table-touch in the C). */
                int err_lineno = (entry != NULL) ? entry->lineno : node->lineno;
                zxbc_error(cs, err_lineno, "Undeclared label \"%s\"", node->u.id.name);
                result = false;
            }
            continue;
        }

        if (node->u.id.class_ == CLASS_unknown) {
            /* A CALL/FUNCCALL callee is Python's token-"FUNCTION" — exempt
             * (its undeclared-ness is reported by check_pending_calls). */
            if (pl_is_marked(callees, callee_len, node))
                continue;
            /* Python token "ID": an unresolved bare identifier. get_entry
             * resolves by name; if still absent or unknown, it never became
             * a real symbol -> "Undeclared identifier". A name that DID get
             * declared elsewhere (e.g. forward-declared label/var sharing
             * the name) resolves to a non-unknown entry and passes. */
            AstNode *entry = symboltable_lookup(cs->symbol_table, node->u.id.name);
            if (!entry || entry->u.id.class_ == CLASS_unknown) {
                zxbc_error(cs, node->lineno, "Undeclared identifier \"%s\"",
                           node->u.id.name);
                result = false;
            }
            continue;
        }
    }

    return result;
}

/* ----------------------------------------------------------------
 * check_call_arguments — faithful port of src/api/check.py
 * check_call_arguments (api/check.py:91-183).
 *
 * Checks every argument of one pending function/sub call against the
 * callee's parameter signature. Returns true on success; on the first
 * reject it emits the Python-verbatim diagnostic and returns false.
 *
 * Data-model mapping (Python -> C):
 *   entry.ref.params      -> entry->parent->children[1] (AST_PARAMLIST),
 *                            its AST_ARGUMENT children
 *   param.name/byref      -> param->u.argument.name / .byref
 *   param.class_          -> param->u.argument.is_array ? array : var
 *   param.type_           -> param->type_
 *   param.default_value   -> param->children[0]  (NULL if child_count==0)
 *   args                  -> call->children[1] (AST_ARGLIST) children
 *   arg.name              -> arg->u.argument.name (NULL == positional)
 *   arg.value             -> arg->children[0]
 *   arg.class_            -> getattr(value,'class_',unknown):
 *                            value->u.id.class_ if value is AST_ID else unknown
 *   arg.typecast(t)       -> make_typecast() replacing arg->children[0]
 *
 * Diagnostics route through cs->current_file; Python passes
 * fname=filename (the call-site file, SymbolCALL.filename) for
 * R3/R4/R5/R6 and fname=arg.filename for R7/R9 — both equal the
 * call-site file for every owned single-scope fixture. We swap
 * cs->current_file to call->u.call.filename for the duration of the
 * argument checks and restore it after (the faithful analogue of the
 * fname= kwarg), then R11 keeps its existing call->lineno reporting.
 * ---------------------------------------------------------------- */
/* named_args dict lookup by key (api/check.py:104). Returns the slot
 * index of `name` in the parallel name array, or -1 if absent. */
static int na_find(const char **names, int n, const char *name) {
    for (int k = 0; k < n; k++)
        if (names[k] && name && strcmp(names[k], name) == 0)
            return k;
    return -1;
}

bool check_call_arguments(CompilerState *cs, AstNode *call,
                          AstNode *entry, const char *id_) {
    /* api/check.py:97-98 — the FIRST thing check_call_arguments does is
     * `if not SYMBOL_TABLE.check_is_declared(id_, lineno, "function"):
     *      return False`. check_is_declared (symboltable.py:163-174) does
     * `result = get_entry(id_)` and rejects when `result is None or not
     * result.declared`. In Python's FUNCTION_CALLS path id_ is
     * call.entry.original_name, so get_entry(id_) resolves to the SAME
     * entry already bound to the call (call.entry) — its `.declared` flag is
     * the authoritative answer. The C deferred loop already resolved that
     * entry (global lookup + callee fallback) and passes it here; a nested
     * SUB/FUNCTION moved to global scope is reachable via the call's
     * resolved callee even when a bare-name re-lookup at post-parse global
     * scope is not (the paramstr5 forward-nested-sub case — a re-lookup
     * here regressed it to a FALSE_POS). So consult the resolved entry's
     * `declared` flag directly, the faithful analogue of
     * get_entry(original_name).declared.
     *
     * Rejecting fires for an auto-declared (CLASS_unknown, declared=False)
     * callee — an undeclared `f$(s)` string-FUNCCALL or a bare `MYLABEL`
     * paren-less call resolving to no function/array/string-var — emitting
     *   `Undeclared function "<id_>"`  (rc 1),
     * exactly as the oracle does. Declared/forward-declared functions, subs
     * (including forward-nested), and builtins all pass (declared==True),
     * keeping FALSE_POS at 0. The message reports cs->current_file, already
     * swapped to the call site's file by the deferred loop (so an #include'd
     * call attributes to the included file, like Python's
     * fname=entry.filename). */
    if (entry == NULL || entry->tag != AST_ID || !entry->u.id.declared) {
        zxbc_error(cs, call->lineno, "Undeclared function \"%s\"", id_);
        return false;
    }

    /* entry.ref.params — the callee PARAMLIST (api/check.py:106),
     * stamped on the shared function ID at FUNCDECL-build time
     * (parser.c id_node->u.id.params). It is NULL for a builtin /
     * unresolved / non-function entry, where Python's
     * check_is_declared / check_is_callable would already have
     * returned False before any R3-R11 — so leave the arg checks
     * unfired (FALSE_POS-0). */
    if (entry->tag != AST_ID || entry->u.id.params == NULL)
        return true;
    AstNode *paramlist = entry->u.id.params;
    if (paramlist->tag != AST_PARAMLIST)
        return true;
    int nparams = paramlist->child_count;

    if (call->child_count < 2)
        return true;
    AstNode *arglist = call->children[1];
    if (!arglist || arglist->tag != AST_ARGLIST)
        return true;

    /* args is mutated in place by the default-fill (Python appends to
     * the SymbolARGLIST) and by typecast; iterate arglist->children
     * directly and grow it via ast_add_child, exactly as Python's
     * symbols.ARGLIST.make_node(args, arg) does. */

    /* named_args: dict[name -> arg]. param/arg counts are tiny; an
     * arena array of distinct names models len(named_args) and the
     * "param.name in named_args" / "named_args[param.name]" lookups
     * (api/check.py:104,125-135,149-150). */
    int cap = nparams + arglist->child_count + 1;
    const char **na_name = arena_alloc(&cs->arena,
                                       (size_t)cap * sizeof(char *));
    AstNode **na_arg = arena_alloc(&cs->arena,
                                   (size_t)cap * sizeof(AstNode *));
    int na_len = 0;
#define NA_FIND(nm) na_find(na_name, na_len, (nm))
#define NA_SET(nm, av) do { int _f = NA_FIND(nm); \
    if (_f >= 0) { na_arg[_f] = (av); } \
    else { na_name[na_len] = (nm); na_arg[na_len] = (av); na_len++; } } while (0)

    /* R3 — unexpected (mis-named) keyword argument (api/check.py:106-110).
     * param_names = {x.name for x in entry.ref.params}. */
    for (int a = 0; a < arglist->child_count; a++) {
        AstNode *arg = arglist->children[a];
        if (!arg || arg->tag != AST_ARGUMENT) continue;
        const char *an = arg->u.argument.name;
        if (an == NULL) continue;
        bool found = false;
        for (int pi = 0; pi < nparams; pi++) {
            AstNode *pm = paramlist->children[pi];
            if (pm && pm->u.argument.name &&
                strcmp(pm->u.argument.name, an) == 0) { found = true; break; }
        }
        if (!found) {
            zxbc_error(cs, call->lineno, "Unexpected argument '%s'", an);
            return false;
        }
    }

    /* zip(args, entry.ref.params): pair positionally, stopping at the
     * shorter (api/check.py:112-125). R4 — positional after keyword;
     * a positional arg's name is back-filled from its paired param. */
    const char *last_arg_name = NULL;
    int zipn = arglist->child_count < nparams ? arglist->child_count : nparams;
    for (int z = 0; z < zipn; z++) {
        AstNode *arg = arglist->children[z];
        AstNode *param = paramlist->children[z];
        if (!arg || arg->tag != AST_ARGUMENT) continue;
        if (last_arg_name != NULL && arg->u.argument.name == NULL) {
            zxbc_error(cs, call->lineno,
                       "Positional argument cannot go after keyword argument '%s'",
                       last_arg_name);
            return false;
        }
        if (arg->u.argument.name != NULL) {
            last_arg_name = arg->u.argument.name;
        } else {
            arg->u.argument.name = param ? param->u.argument.name : NULL;
        }
        NA_SET(arg->u.argument.name, arg);
    }

    /* Default-fill (api/check.py:127-135): if fewer named args than
     * params, walk params in order; skip those already supplied; stop
     * (break) at the first missing param with no default; otherwise
     * synthesise an ARGUMENT from param.default_value, append it to
     * args (ARGLIST.make_node) and to named_args. */
    if (na_len < nparams) {
        for (int pi = 0; pi < nparams; pi++) {
            AstNode *param = paramlist->children[pi];
            if (!param) continue;
            const char *pn = param->u.argument.name;
            if (NA_FIND(pn) >= 0) continue;
            AstNode *defv = (param->child_count >= 1) ? param->children[0]
                                                      : NULL;
            if (defv == NULL) break;
            AstNode *arg = ast_new(cs, AST_ARGUMENT, call->lineno);
            arg->u.argument.name = (char *)pn;
            arg->u.argument.byref = false;
            ast_add_child(cs, arg, defv);
            arg->type_ = defv->type_;
            ast_add_child(cs, arglist, arg);
            NA_SET(pn, arg);
        }
    }

    /* R5 — too many positional args: any arg still un-named after the
     * zip+back-fill is beyond the parameter list (api/check.py:137-140). */
    for (int a = 0; a < arglist->child_count; a++) {
        AstNode *arg = arglist->children[a];
        if (!arg || arg->tag != AST_ARGUMENT) continue;
        if (arg->u.argument.name == NULL) {
            zxbc_error(cs, call->lineno,
                       "Too many arguments for Function '%s'", id_);
            return false;
        }
    }

    /* R6 — argument/parameter count mismatch (api/check.py:142-147). */
    if (na_len != nparams) {
        const char *plural = (nparams != 1) ? "s" : "";
        zxbc_error(cs, call->lineno,
                   "Function '%s' takes %d parameter%s, not %d",
                   id_, nparams, plural, arglist->child_count);
        return false;
    }

    /* Per-parameter checks (api/check.py:149-173). */
    for (int pi = 0; pi < nparams; pi++) {
        AstNode *param = paramlist->children[pi];
        if (!param) continue;
        int ai = NA_FIND(param->u.argument.name);
        if (ai < 0) continue;
        AstNode *arg = na_arg[ai];
        if (!arg || arg->child_count < 1) continue;

        AstNode *aval = arg->children[0];
        SymbolClass arg_class = (aval && aval->tag == AST_ID)
                                    ? aval->u.id.class_ : CLASS_unknown;
        SymbolClass param_class =
            param->u.argument.is_array ? CLASS_array : CLASS_var;

        /* R7 — array/var class mismatch (api/check.py:152-154).
         * str(arg.value) is the ID name (_id.py:88). */
        if ((arg_class == CLASS_var || arg_class == CLASS_array) &&
            param_class != arg_class) {
            const char *vstr = (aval && aval->tag == AST_ID)
                                   ? aval->u.id.name : "";
            zxbc_error(cs, call->lineno, "Invalid argument '%s'", vstr);
            return false;
        }

        /* R8 — arg -> param typecast (api/check.py:156-157).
         * arg.typecast() == make_typecast replacing arg.value; on
         * failure make_typecast already emitted the Python-verbatim
         * message (Array/string/value conversion) and returned NULL. */
        AstNode *casted = make_typecast(cs, param->type_, aval,
                                        call->lineno);
        if (casted == NULL)
            return false;
        arg->children[0] = casted;
        if (casted->parent != arg) casted->parent = arg;

        /* R9 / R10 — ByRef parameter (api/check.py:159-170). Checked
         * AFTER the typecast: if the cast wrapped the value in a
         * TYPECAST (type mismatch on a ByRef param) arg.value is no
         * longer an ID/ARRAYLOAD and R9 fires (bad_fname_err0). */
        if (param->u.argument.byref) {
            AstNode *v = arg->children[0];
            bool is_var_ref =
                v && (v->tag == AST_ID || v->tag == AST_ARRAYLOAD ||
                      v->tag == AST_ARRAYACCESS);
            if (!is_var_ref) {
                zxbc_error(cs, call->lineno,
                           "Expected a variable name, not an expression (parameter By Reference)");
                return false;
            }
            /* arg.class_ recomputed from the (possibly cast) value;
             * not in (var, array, unknown) -> R10. Python omits fname
             * here, so it reports gl.FILENAME — equivalently our
             * (already-swapped) cs->current_file. */
            SymbolClass vc = (v->tag == AST_ID) ? v->u.id.class_
                                                : CLASS_unknown;
            if (vc != CLASS_var && vc != CLASS_array &&
                vc != CLASS_unknown) {
                zxbc_error(cs, call->lineno,
                           "Expected a variable or array name (parameter By Reference)");
                return false;
            }
            arg->u.argument.byref = true;
        }
    }

#undef NA_FIND
#undef NA_SET
    return true;
}

/* check_pending_calls: validate forward-referenced function calls.
 * Matches Python's src/api/check.py check_pending_calls() (:186-196):
 * for each pending call, run check_call_arguments. The R2 (SUB used as
 * FUNCTION) and R11 (declared-but-not-implemented) checks predate
 * S5.10a and are retained; S5.10a adds the R3-R10 argument checks via
 * check_call_arguments above. */
bool check_pending_calls(CompilerState *cs) {
    bool result = true;

    for (int i = 0; i < cs->function_calls.len; i++) {
        AstNode *call = cs->function_calls.data[i];
        if (!call) continue;

        /* The call node's first child is the callee ID */
        if (call->child_count < 1) continue;
        AstNode *callee = call->children[0];
        if (!callee || callee->tag != AST_ID) continue;

        /* Look up the callee in the global scope — the callee pointer may be
         * orphaned if it was created in a function scope that has since exited. */
        const char *name = callee->u.id.name;

        /* Strip deprecated suffix for lookup */
        size_t len = strlen(name);
        char stripped[256];
        const char *lookup_name = name;
        if (len > 0 && len < sizeof(stripped) && is_deprecated_suffix(name[len - 1])) {
            memcpy(stripped, name, len - 1);
            stripped[len - 1] = '\0';
            lookup_name = stripped;
        }

        /* Try global scope lookup first, then fall back to callee node */
        AstNode *entry = hashmap_get(&cs->symbol_table->global_scope->symbols, lookup_name);
        if (!entry) entry = callee;

        SymbolClass cls = entry->u.id.class_;

        /* Skip non-callable entries (arrays, variables) */
        if (cls == CLASS_array || cls == CLASS_var || cls == CLASS_const) {
            continue;
        }

        /* Check if a SUB is being used as a FUNCTION (in expression context) */
        if (call->tag == AST_FUNCCALL && cls == CLASS_sub) {
            zxbc_error(cs, call->lineno, "'%s' is a SUB, not a FUNCTION", name);
            result = false;
            continue;
        }

        /* R3-R10 argument checks (api/check.py check_call_arguments).
         * Run ONLY for calls Python would have deferred to
         * gl.FUNCTION_CALLS — i.e. callee NOT a finished definition at
         * call-parse time (Python call.py:104-110). Inline (def-first)
         * calls are checked at PARSE time instead, by
         * inline_check_call_arguments (parser.c), faithfully mirroring
         * Python's inline branch (call.py:103) which runs the same full
         * check_call_arguments at the call site — including its parse-time
         * firing order relative to the surrounding statement's [W100]
         * implicit-type warnings. Running the inline check here in the
         * post-parse loop instead would mis-order those warnings (the
         * call-arg error would follow rather than precede them). The
         * pre-existing R2/R11 checks below are NOT gated by this. */
        if (!call->u.call.callee_inline) {
            /* Python passes fname=filename (SymbolCALL.filename, the
             * call-site file) for R3-R6 and fname=arg.filename for
             * R7/R9 — both the call-site file for our single-scope
             * fixtures. Swap cs->current_file to the captured call
             * filename for the duration, then restore it (the faithful
             * analogue of the fname= kwarg). */
            char *saved_file = cs->current_file;
            if (call->u.call.filename)
                cs->current_file = call->u.call.filename;
            /* Python passes call.entry.original_name (api/check.py:194) —
             * the sigil-bearing name — to check_call_arguments, so the
             * `Undeclared function "<id>"` message prints `f$` not `f`. */
            const char *call_id = call->u.call.original_name
                                      ? call->u.call.original_name : name;
            bool ok = check_call_arguments(cs, call, entry, call_id);
            cs->current_file = saved_file;
            if (!ok) {
                result = false;
                continue;
            }
        }

        /* R11 — forward-declared but never implemented (api/check.py
         * :175-181). Python reports fname=entry.filename — the
         * #line-active filename when the SUB was DECLARED (stamped on
         * the ID node at first creation in symboltable_declare, mirroring
         * _id.py:58). Swap cs->current_file to entry.filename for the
         * duration of the emit (the faithful analogue of the fname=
         * kwarg), then restore — same shape as the R3-R10 fname= swap
         * above. lineno stays call->lineno (Python passes lineno through
         * check_call_arguments). bad_fname_err4 / probe
         * err_sub_decl_filename_via_line_pragma. */
        if (entry->u.id.forwarded) {
            const char *kind = (cls == CLASS_sub) ? "sub" : "function";
            char *saved_file = cs->current_file;
            if (entry->u.id.filename)
                cs->current_file = entry->u.id.filename;
            zxbc_error(cs, call->lineno, "%s '%s' declared but not implemented",
                       kind, name);
            cs->current_file = saved_file;
            result = false;
        }

        /* Note: CLASS_unknown entries that were never declared as function/sub
         * could be undeclared functions OR implicit string variables used with
         * subscripts. Without full type checking, we can't distinguish them,
         * so we defer this check to the semantic analysis phase. */
    }

    return result;
}

/* symboltable_check_classes: validate that all symbols in global scope
 * have been properly resolved (no CLASS_unknown left).
 * Matches Python's SYMBOL_TABLE.check_classes(). */
static bool check_classes_cb(const char *key, void *value, void *userdata) {
    (void)key;
    CompilerState *cs = (CompilerState *)userdata;
    AstNode *entry = (AstNode *)value;
    if (!entry) return true;
    if (entry->u.id.class_ == CLASS_unknown) {
        zxbc_error(cs, entry->lineno, "Undeclared identifier \"%s\"", entry->u.id.name);
    }
    return true;
}

void symboltable_check_classes(SymbolTable *st, CompilerState *cs) {
    hashmap_foreach(&st->global_scope->symbols, check_classes_cb, cs);
}

/* ----------------------------------------------------------------
 * Compiler state
 * ---------------------------------------------------------------- */

void compiler_init(CompilerState *cs) {
    memset(cs, 0, sizeof(*cs));
    arena_init(&cs->arena, 0);
    compiler_options_init(&cs->opts);

    hashmap_init(&cs->error_cache);
    hashmap_init(&cs->labels);
    hashmap_init(&cs->data_labels);
    hashmap_init(&cs->data_labels_required);

    vec_init(cs->function_level);
    vec_init(cs->function_calls);
    vec_init(cs->functions);
    vec_init(cs->loop_stack);
    vec_init(cs->datas);
    vec_init(cs->data_functions);
    vec_init(cs->inits);
    vec_init(cs->requires);
    vec_init(cs->sym_entries_ordered);   /* N1 */

    cs->symbol_table = symboltable_new(cs);
    cs->default_type = cs->symbol_table->basic_types[TYPE_float];

    cs->labels_allowed = true;
    cs->let_assignment = false;
    cs->print_is_used = false;
    /* function_translator.py:79-80 OPTIONS.__DEFINES
     * ["__ZXB_USE_LOCAL_ARRAY_WITH_BOUNDS__"] mirror — cleared per
     * compilation like print_is_used (memset already zeros it; explicit
     * for parity with the print_is_used reset). */
    cs->local_array_with_bounds_used = false;
    cs->last_brk_linenum = 0;
    cs->data_is_used = false;
    /* gl.DATA_PTR_CURRENT reset (zxbparser.py:149): current_data_label()
     * with len(gl.DATAS)==0 -> ".DATA.__DATA__0". */
    cs->data_ptr_current = current_data_label(cs);
    cs->temp_counter = 0;
}

void compiler_destroy(CompilerState *cs) {
    hashmap_free(&cs->error_cache);
    hashmap_free(&cs->labels);
    hashmap_free(&cs->data_labels);
    hashmap_free(&cs->data_labels_required);

    vec_free(cs->function_level);
    vec_free(cs->function_calls);
    vec_free(cs->functions);
    vec_free(cs->loop_stack);
    vec_free(cs->datas);
    vec_free(cs->data_functions);
    vec_free(cs->inits);
    vec_free(cs->requires);
    vec_free(cs->sym_entries_ordered);   /* N1 */

    arena_destroy(&cs->arena);
}

char *compiler_new_temp(CompilerState *cs) {
    char buf[32];
    snprintf(buf, sizeof(buf), "t%d", cs->temp_counter++);
    return arena_strdup(&cs->arena, buf);
}
