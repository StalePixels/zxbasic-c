/*
 * compiler.c — Compiler state initialization and management
 *
 * Ported from src/api/global_.py and src/zxbc/zxbparser.py init()
 */
#include "zxbc.h"
#include "errmsg.h"
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Symbol table
 * ---------------------------------------------------------------- */

SymbolTable *symboltable_new(CompilerState *cs) {
    SymbolTable *st = arena_calloc(&cs->arena, 1, sizeof(SymbolTable));
    st->arena = &cs->arena;

    /* Create global scope */
    st->global_scope = arena_calloc(&cs->arena, 1, sizeof(Scope_));
    hashmap_init(&st->global_scope->symbols);
    st->global_scope->parent = NULL;
    st->global_scope->level = 0;
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

void symboltable_enter_scope(SymbolTable *st, CompilerState *cs) {
    Scope_ *scope = arena_calloc(st->arena, 1, sizeof(Scope_));
    hashmap_init(&scope->symbols);
    scope->parent = st->current_scope;
    scope->level = st->current_scope->level + 1;
    st->current_scope = scope;
}

void symboltable_exit_scope(SymbolTable *st) {
    if (st->current_scope->parent) {
        /* Note: we don't free the scope — arena handles that */
        st->current_scope = st->current_scope->parent;
    }
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
    node->u.id.class_ = class_;
    node->u.id.scope = (st->current_scope->level == 0) ? SCOPE_global : SCOPE_local;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;

    hashmap_set(&st->current_scope->symbols, name, node);
    return node;
}

AstNode *symboltable_lookup(SymbolTable *st, const char *name) {
    /* Search from current scope up to global */
    for (Scope_ *s = st->current_scope; s != NULL; s = s->parent) {
        AstNode *node = hashmap_get(&s->symbols, name);
        if (node) return node;
    }
    return NULL;
}

AstNode *symboltable_get_entry(SymbolTable *st, const char *name) {
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
    node->u.id.class_ = CLASS_var;
    node->u.id.scope = (st->current_scope->level == 0) ? SCOPE_global : SCOPE_local;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
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

    hashmap_set(&st->current_scope->symbols, base_name, node);
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
    node->u.id.class_ = CLASS_var;
    node->u.id.scope = SCOPE_parameter;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
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

    hashmap_set(&st->current_scope->symbols, name, node);
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
    node->u.id.class_ = CLASS_array;
    node->u.id.scope = (st->current_scope->level == 0) ? SCOPE_global : SCOPE_local;
    node->u.id.convention = CONV_unknown;
    node->u.id.byref = false;
    node->u.id.accessed = false;
    node->u.id.forwarded = false;
    node->u.id.declared = true;
    node->type_ = typeref;

    hashmap_set(&st->current_scope->symbols, name, node);
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
    vec_init(cs->inits);

    cs->symbol_table = symboltable_new(cs);
    cs->default_type = cs->symbol_table->basic_types[TYPE_float];

    cs->labels_allowed = true;
    cs->let_assignment = false;
    cs->print_is_used = false;
    cs->last_brk_linenum = 0;
    cs->data_is_used = false;
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
    vec_free(cs->inits);

    arena_destroy(&cs->arena);
}

char *compiler_new_temp(CompilerState *cs) {
    char buf[32];
    snprintf(buf, sizeof(buf), "t%d", cs->temp_counter++);
    return arena_strdup(&cs->arena, buf);
}
