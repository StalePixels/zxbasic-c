/*
 * test_symboltable.c — Tests for the symbol table
 *
 * Matches: tests/api/test_symbolTable.py (TestSymbolTable) — all 18 tests
 */
#include "test_harness.h"
#include "zxbc.h"
#include "errmsg.h"

#include <string.h>

/* ---- Test infrastructure ---- */

/* Helper: create a TYPEREF wrapping a basic type */
static TypeInfo *btyperef(CompilerState *cs, BasicType bt) {
    TypeInfo *basic = cs->symbol_table->basic_types[bt];
    return type_new_ref(cs, basic, 0, false);
}

/* ---- Captured-output test infrastructure ---- */

/* We use a pipe to capture error output */
static char captured_err[4096];

static CompilerState *new_cs_capture(void) {
    static CompilerState cs;
    compiler_init(&cs);
    cs.current_file = "(stdin)";

    memset(captured_err, 0, sizeof(captured_err));

    /* Use tmpfile for error capture */
    cs.opts.stderr_f = tmpfile();
    return &cs;
}

static const char *flush_capture(CompilerState *cs) {
    if (!cs->opts.stderr_f) return "";
    fflush(cs->opts.stderr_f);
    rewind(cs->opts.stderr_f);
    size_t n = fread(captured_err, 1, sizeof(captured_err) - 1, cs->opts.stderr_f);
    captured_err[n] = '\0';
    return captured_err;
}

static void free_cs_capture(CompilerState *cs) {
    if (cs->opts.stderr_f) {
        fclose(cs->opts.stderr_f);
        cs->opts.stderr_f = NULL;
    }
    compiler_destroy(cs);
}

/* ---- test__init__ ---- */
TEST(test_symboltable_init) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* All basic types should be registered */
    for (int i = 0; i < TYPE_COUNT; i++) {
        ASSERT_NOT_NULL(st->basic_types[i]);
        ASSERT_TRUE(type_is_basic(st->basic_types[i]));
        ASSERT_EQ(st->basic_types[i]->tag, AST_BASICTYPE);
    }

    /* Current scope should be global scope */
    ASSERT_EQ(st->current_scope, st->global_scope);

    free_cs_capture(cs);
}

/* ---- test_is_declared ---- */
TEST(test_is_declared) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* 'a' not declared yet */
    ASSERT_FALSE(symboltable_check_is_declared(st, "a", 0, "var", false, cs));

    /* Declare 'a' */
    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));

    /* 'a' is now declared */
    ASSERT_TRUE(symboltable_check_is_declared(st, "a", 1, "var", false, cs));

    free_cs_capture(cs);
}

/* ---- test_is_undeclared ---- */
TEST(test_is_undeclared) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* 'a' is undeclared */
    ASSERT_TRUE(symboltable_check_is_undeclared(st, "a", 10, false, cs));

    /* Declare 'a' */
    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));

    /* 'a' is not undeclared anymore */
    ASSERT_FALSE(symboltable_check_is_undeclared(st, "a", 10, false, cs));

    free_cs_capture(cs);
}

/* ---- test_declare_variable ---- */
TEST(test_declare_variable) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    AstNode *a = symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(symboltable_lookup(st, "a"));

    free_cs_capture(cs);
}

/* ---- test_declare_variable_dupl ---- */
TEST(test_declare_variable_dupl) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    /* Duplicate */
    AstNode *dup = symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    ASSERT_NULL(dup);

    const char *output = flush_capture(cs);
    ASSERT_NOT_NULL(strstr(output, "Variable 'a' already declared at (stdin):10"));

    free_cs_capture(cs);
}

/* ---- test_declare_variable_dupl_suffix ---- */
TEST(test_declare_variable_dupl_suffix) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    /* 'a%' should conflict with 'a' since suffix is stripped */
    AstNode *dup = symboltable_declare_variable(st, cs, "a%", 11, btyperef(cs, TYPE_integer));
    ASSERT_NULL(dup);

    const char *output = flush_capture(cs);
    ASSERT_NOT_NULL(strstr(output, "Variable 'a%' already declared at (stdin):10"));

    free_cs_capture(cs);
}

/* ---- test_declare_variable_wrong_suffix ---- */
TEST(test_declare_variable_wrong_suffix) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* b% expects integer type, but we declare as byte */
    AstNode *b = symboltable_declare_variable(st, cs, "b%", 12, btyperef(cs, TYPE_byte));
    ASSERT_NULL(b);

    const char *output = flush_capture(cs);
    ASSERT_NOT_NULL(strstr(output, "expected type integer for 'b%', got byte"));
    ASSERT_NOT_NULL(strstr(output, "'b%' suffix is for type 'integer' but it was declared as 'byte'"));

    free_cs_capture(cs);
}

/* ---- test_declare_variable_remove_suffix ---- */
TEST(test_declare_variable_remove_suffix) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* c% should be stored as 'c' */
    symboltable_declare_variable(st, cs, "c%", 12, btyperef(cs, TYPE_integer));
    AstNode *entry = symboltable_lookup(st, "c");
    ASSERT_NOT_NULL(entry);

    /* Name should not end with deprecated suffix */
    size_t len = strlen(entry->u.id.name);
    ASSERT_FALSE(is_deprecated_suffix(entry->u.id.name[len - 1]));

    free_cs_capture(cs);
}

/* ---- test_declare_param_dupl ---- */
TEST(test_declare_param_dupl) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    /* Duplicate param */
    AstNode *p = symboltable_declare_param(st, cs, "a", 11, btyperef(cs, TYPE_integer));
    ASSERT_NULL(p);

    const char *output = flush_capture(cs);
    ASSERT_NOT_NULL(strstr(output, "Duplicated parameter \"a\" (previous one at (stdin):10)"));

    free_cs_capture(cs);
}

/* ---- test_declare_param ---- */
TEST(test_declare_param) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    AstNode *p = symboltable_declare_param(st, cs, "a", 11, btyperef(cs, TYPE_integer));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->u.id.scope, SCOPE_parameter);
    ASSERT_EQ(p->u.id.class_, CLASS_var);
    /* t should not start with "$" for non-string types */
    ASSERT_TRUE(p->t[0] != '$');

    free_cs_capture(cs);
}

/* ---- test_declare_param_str ---- */
TEST(test_declare_param_str) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    AstNode *p = symboltable_declare_param(st, cs, "a", 11, btyperef(cs, TYPE_string));
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->u.id.scope, SCOPE_parameter);
    ASSERT_EQ(p->u.id.class_, CLASS_var);
    /* String param's t should start with "$" */
    ASSERT_EQ(p->t[0], '$');

    free_cs_capture(cs);
}

/* ---- test_get_entry ---- */
TEST(test_get_entry) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* Not found */
    AstNode *var_a = symboltable_get_entry(st, "a");
    ASSERT_NULL(var_a);

    /* Declare and find */
    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    var_a = symboltable_get_entry(st, "a");
    ASSERT_NOT_NULL(var_a);
    ASSERT_EQ(var_a->tag, AST_ID);
    ASSERT_EQ(var_a->u.id.class_, CLASS_var);
    ASSERT_EQ(var_a->u.id.scope, SCOPE_global);

    free_cs_capture(cs);
}

/* ---- test_enter_scope ---- */
TEST(test_enter_scope) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    symboltable_enter_scope(st, cs);
    ASSERT(st->current_scope != st->global_scope);

    /* 'a' undeclared in current scope (though visible via lookup) */
    ASSERT_TRUE(symboltable_check_is_undeclared(st, "a", 11, false, cs));

    free_cs_capture(cs);
}

/* ---- test_declare_local_var ---- */
TEST(test_declare_local_var) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    symboltable_declare_variable(st, cs, "a", 12, btyperef(cs, TYPE_float));
    ASSERT_TRUE(symboltable_check_is_declared(st, "a", 11, "var", false, cs));
    AstNode *entry = symboltable_get_entry(st, "a");
    ASSERT_EQ(entry->u.id.scope, SCOPE_local);

    free_cs_capture(cs);
}

/* ---- test_declare_array ---- */
TEST(test_declare_array) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    /* Create BOUNDLIST with one bound */
    AstNode *bounds = ast_new(cs, AST_BOUNDLIST, 1);
    AstNode *bound = ast_new(cs, AST_BOUND, 1);
    AstNode *lo = ast_new(cs, AST_NUMBER, 1); lo->u.number.value = 1;
    AstNode *hi = ast_new(cs, AST_NUMBER, 1); hi->u.number.value = 2;
    ast_add_child(cs, bound, lo);
    ast_add_child(cs, bound, hi);
    ast_add_child(cs, bounds, bound);

    /* Add second bound */
    AstNode *bound2 = ast_new(cs, AST_BOUND, 1);
    AstNode *lo2 = ast_new(cs, AST_NUMBER, 1); lo2->u.number.value = 3;
    AstNode *hi2 = ast_new(cs, AST_NUMBER, 1); hi2->u.number.value = 4;
    ast_add_child(cs, bound2, lo2);
    ast_add_child(cs, bound2, hi2);
    ast_add_child(cs, bounds, bound2);

    TypeInfo *tref = type_new_ref(cs, cs->symbol_table->basic_types[TYPE_byte], 0, false);
    AstNode *arr = symboltable_declare_array(st, cs, "test", 1, tref, bounds);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ(arr->u.id.class_, CLASS_array);

    free_cs_capture(cs);
}

/* ---- test_declare_array_fail (type must be TYPEREF) ---- */
TEST(test_declare_array_fail) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    AstNode *bounds = ast_new(cs, AST_BOUNDLIST, 1);
    /* Pass a raw basic type, not a TYPEREF — should fail */
    TypeInfo *raw = cs->symbol_table->basic_types[TYPE_byte];
    AstNode *arr = symboltable_declare_array(st, cs, "test", 1, raw, bounds);
    ASSERT_NULL(arr);

    free_cs_capture(cs);
}

/* ---- test_declare_array_fail2 (bounds must be BOUNDLIST) ---- */
TEST(test_declare_array_fail2) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    TypeInfo *tref = type_new_ref(cs, cs->symbol_table->basic_types[TYPE_byte], 0, false);
    /* Pass a non-BOUNDLIST node */
    AstNode *not_bounds = ast_new(cs, AST_NOP, 1);
    AstNode *arr = symboltable_declare_array(st, cs, "test", 1, tref, not_bounds);
    ASSERT_NULL(arr);

    free_cs_capture(cs);
}

/* ---- test_declare_local_array ---- */
TEST(test_declare_local_array) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);

    AstNode *bounds = ast_new(cs, AST_BOUNDLIST, 1);
    AstNode *bound = ast_new(cs, AST_BOUND, 1);
    AstNode *lo = ast_new(cs, AST_NUMBER, 1); lo->u.number.value = 0;
    AstNode *hi = ast_new(cs, AST_NUMBER, 1); hi->u.number.value = 2;
    ast_add_child(cs, bound, lo);
    ast_add_child(cs, bound, hi);
    ast_add_child(cs, bounds, bound);

    TypeInfo *tref = type_new_ref(cs, cs->symbol_table->basic_types[TYPE_float], 0, false);
    symboltable_declare_array(st, cs, "a", 12, tref, bounds);

    ASSERT_TRUE(symboltable_check_is_declared(st, "a", 11, "var", false, cs));
    AstNode *entry = symboltable_get_entry(st, "a");
    ASSERT_EQ(entry->u.id.scope, SCOPE_local);

    free_cs_capture(cs);
}

/* ---- test_declare_local_var_dup ---- */
TEST(test_declare_local_var_dup) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    symboltable_declare_variable(st, cs, "a", 12, btyperef(cs, TYPE_float));
    AstNode *dup = symboltable_declare_variable(st, cs, "a", 14, btyperef(cs, TYPE_float));
    ASSERT_NULL(dup);

    const char *output = flush_capture(cs);
    ASSERT_NOT_NULL(strstr(output, "Variable 'a' already declared at (stdin):12"));

    free_cs_capture(cs);
}

/* ---- test_leave_scope ---- */
TEST(test_leave_scope) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    symboltable_exit_scope(st);
    ASSERT_EQ(st->current_scope, st->global_scope);

    free_cs_capture(cs);
}

/* ---- test_local_var_cleaned ---- */
TEST(test_local_var_cleaned) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    symboltable_declare_variable(st, cs, "a", 10, btyperef(cs, TYPE_integer));
    symboltable_exit_scope(st);

    /* 'a' should no longer be visible */
    ASSERT_TRUE(symboltable_check_is_undeclared(st, "a", 10, false, cs));

    free_cs_capture(cs);
}

/* ---- Type registry test (bonus) ---- */
TEST(test_symboltable_type_registry) {
    CompilerState *cs = new_cs_capture();
    SymbolTable *st = cs->symbol_table;

    TypeInfo *t_byte = symboltable_get_type(st, "byte");
    ASSERT_NOT_NULL(t_byte);
    ASSERT_EQ(t_byte->basic_type, TYPE_byte);

    TypeInfo *t_float = symboltable_get_type(st, "float");
    ASSERT_NOT_NULL(t_float);
    ASSERT_EQ(t_float->basic_type, TYPE_float);

    TypeInfo *t_string = symboltable_get_type(st, "string");
    ASSERT_NOT_NULL(t_string);
    ASSERT_EQ(t_string->basic_type, TYPE_string);

    TypeInfo *t_none = symboltable_get_type(st, "nonexistent");
    ASSERT_NULL(t_none);

    free_cs_capture(cs);
}

int main(void) {
    printf("test_symboltable (matching tests/api/test_symbolTable.py):\n");
    RUN_TEST(test_symboltable_init);
    RUN_TEST(test_is_declared);
    RUN_TEST(test_is_undeclared);
    RUN_TEST(test_declare_variable);
    RUN_TEST(test_declare_variable_dupl);
    RUN_TEST(test_declare_variable_dupl_suffix);
    RUN_TEST(test_declare_variable_wrong_suffix);
    RUN_TEST(test_declare_variable_remove_suffix);
    RUN_TEST(test_declare_param_dupl);
    RUN_TEST(test_declare_param);
    RUN_TEST(test_declare_param_str);
    RUN_TEST(test_get_entry);
    RUN_TEST(test_enter_scope);
    RUN_TEST(test_declare_local_var);
    RUN_TEST(test_declare_array);
    RUN_TEST(test_declare_array_fail);
    RUN_TEST(test_declare_array_fail2);
    RUN_TEST(test_declare_local_array);
    RUN_TEST(test_declare_local_var_dup);
    RUN_TEST(test_leave_scope);
    RUN_TEST(test_local_var_cleaned);
    RUN_TEST(test_symboltable_type_registry);
    REPORT();
}
