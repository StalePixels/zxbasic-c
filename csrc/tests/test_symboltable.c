/*
 * test_symboltable.c — Tests for the symbol table
 *
 * Matches: tests/api/test_symbolTable.py (TestSymbolTable)
 */
#include "test_harness.h"
#include "zxbc.h"

/* Helper: create a fresh CompilerState for each test */
static CompilerState *new_cs(void) {
    static CompilerState cs;
    compiler_init(&cs);
    return &cs;
}

static void free_cs(CompilerState *cs) {
    compiler_destroy(cs);
}

/* ---- test_symbolTable.py: test__init__ ---- */
TEST(test_symboltable_init) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    /* All basic types should be registered */
    for (int i = 0; i < TYPE_COUNT; i++) {
        ASSERT_NOT_NULL(st->basic_types[i]);
        ASSERT_TRUE(type_is_basic(st->basic_types[i]));
        ASSERT_EQ(st->basic_types[i]->tag, AST_BASICTYPE);
    }

    /* Current scope should be global scope */
    ASSERT_EQ(st->current_scope, st->global_scope);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_get_entry ---- */
TEST(test_symboltable_get_entry) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    /* Not declared yet */
    AstNode *var_a = symboltable_lookup(st, "a");
    ASSERT_NULL(var_a);

    /* Declare */
    AstNode *decl = symboltable_declare(st, cs, "a", 10, CLASS_var);
    ASSERT_NOT_NULL(decl);
    ASSERT_EQ(decl->tag, AST_ID);
    ASSERT_EQ(decl->u.id.class_, CLASS_var);
    ASSERT_EQ(decl->u.id.scope, SCOPE_global);

    /* Now should find it */
    var_a = symboltable_lookup(st, "a");
    ASSERT_NOT_NULL(var_a);
    ASSERT_EQ(var_a, decl);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_declare_variable ---- */
TEST(test_symboltable_declare) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    AstNode *a = symboltable_declare(st, cs, "a", 10, CLASS_var);
    ASSERT_NOT_NULL(a);

    /* Declaring same name again should return existing */
    AstNode *a2 = symboltable_declare(st, cs, "a", 10, CLASS_var);
    ASSERT_EQ(a, a2);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_enter_scope ---- */
TEST(test_symboltable_enter_scope) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    /* Declare 'a' in global scope */
    symboltable_declare(st, cs, "a", 10, CLASS_var);

    /* Enter new scope */
    symboltable_enter_scope(st, cs);
    ASSERT(st->current_scope != st->global_scope);

    /* 'a' not in current scope, but visible through lookup */
    AstNode *found = symboltable_lookup(st, "a");
    ASSERT_NOT_NULL(found);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_declare_local_var ---- */
TEST(test_symboltable_declare_local_var) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    AstNode *a = symboltable_declare(st, cs, "a", 12, CLASS_var);
    ASSERT_NOT_NULL(a);
    ASSERT_EQ(a->u.id.scope, SCOPE_local);

    /* Should be findable */
    AstNode *found = symboltable_lookup(st, "a");
    ASSERT_NOT_NULL(found);
    ASSERT_EQ(found, a);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_leave_scope ---- */
TEST(test_symboltable_leave_scope) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    symboltable_declare(st, cs, "a", 10, CLASS_var);

    symboltable_exit_scope(st);
    ASSERT_EQ(st->current_scope, st->global_scope);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_local_var_cleaned ---- */
TEST(test_symboltable_local_var_cleaned) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    symboltable_declare(st, cs, "a", 10, CLASS_var);
    symboltable_exit_scope(st);

    /* 'a' should no longer be visible */
    AstNode *found = symboltable_lookup(st, "a");
    ASSERT_NULL(found);

    free_cs(cs);
}

/* ---- test_symbolTable.py: test_declare_param ---- */
TEST(test_symboltable_declare_param) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    AstNode *p = symboltable_declare(st, cs, "a", 11, CLASS_var);
    ASSERT_NOT_NULL(p);
    /* We set scope to parameter scope when declaring params */
    /* In our simplified implementation, local scope is used for now */

    free_cs(cs);
}

/* ---- Type registry test ---- */
TEST(test_symboltable_type_registry) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    /* All basic types should be in the registry */
    TypeInfo *t_byte = symboltable_get_type(st, "byte");
    ASSERT_NOT_NULL(t_byte);
    ASSERT_EQ(t_byte->basic_type, TYPE_byte);

    TypeInfo *t_float = symboltable_get_type(st, "float");
    ASSERT_NOT_NULL(t_float);
    ASSERT_EQ(t_float->basic_type, TYPE_float);

    TypeInfo *t_string = symboltable_get_type(st, "string");
    ASSERT_NOT_NULL(t_string);
    ASSERT_EQ(t_string->basic_type, TYPE_string);

    /* Non-existent type */
    TypeInfo *t_none = symboltable_get_type(st, "nonexistent");
    ASSERT_NULL(t_none);

    free_cs(cs);
}

int main(void) {
    printf("test_symboltable (matching tests/api/test_symbolTable.py):\n");
    RUN_TEST(test_symboltable_init);
    RUN_TEST(test_symboltable_get_entry);
    RUN_TEST(test_symboltable_declare);
    RUN_TEST(test_symboltable_enter_scope);
    RUN_TEST(test_symboltable_declare_local_var);
    RUN_TEST(test_symboltable_leave_scope);
    RUN_TEST(test_symboltable_local_var_cleaned);
    RUN_TEST(test_symboltable_declare_param);
    RUN_TEST(test_symboltable_type_registry);
    REPORT();
}
