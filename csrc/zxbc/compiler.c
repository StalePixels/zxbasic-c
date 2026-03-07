/*
 * compiler.c — Compiler state initialization and management
 *
 * Ported from src/api/global_.py and src/zxbc/zxbparser.py init()
 */
#include "zxbc.h"
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
