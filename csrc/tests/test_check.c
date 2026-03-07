/*
 * test_check.c — Tests for api/check functions
 *
 * Matches: tests/api/test_check.py (TestCheck) — 4 tests
 */
#include "test_harness.h"
#include "zxbc.h"

#include <string.h>

static CompilerState *new_cs(void) {
    static CompilerState cs;
    compiler_init(&cs);
    cs.current_file = "(stdin)";
    return &cs;
}

static void free_cs(CompilerState *cs) {
    compiler_destroy(cs);
}

/* ---- test_is_temporary_value_const_string ---- */
TEST(test_is_temporary_value_const_string) {
    CompilerState *cs = new_cs();
    AstNode *node = ast_new(cs, AST_STRING, 1);
    node->u.string.value = "Hello world";
    node->u.string.length = 11;
    ASSERT_FALSE(is_temporary_value(node));
    free_cs(cs);
}

/* ---- test_is_temporary_value_var ---- */
TEST(test_is_temporary_value_var) {
    CompilerState *cs = new_cs();
    AstNode *node = ast_new(cs, AST_ID, 1);
    node->u.id.name = "a";
    node->u.id.class_ = CLASS_var;
    node->t = "_a";  /* Variables get "_name" prefix */
    ASSERT_FALSE(is_temporary_value(node));
    free_cs(cs);
}

/* ---- test_is_temporary_value_param ---- */
TEST(test_is_temporary_value_param) {
    CompilerState *cs = new_cs();
    AstNode *node = ast_new(cs, AST_ID, 1);
    node->u.id.name = "a";
    node->u.id.class_ = CLASS_var;
    node->u.id.scope = SCOPE_parameter;
    node->t = "_a";
    ASSERT_FALSE(is_temporary_value(node));
    free_cs(cs);
}

/* ---- test_is_temporary_value_expr ---- */
TEST(test_is_temporary_value_expr) {
    CompilerState *cs = new_cs();

    /* Create a BINARY node — expressions are temporary */
    AstNode *child = ast_new(cs, AST_ID, 1);
    child->u.id.name = "a";
    child->u.id.class_ = CLASS_var;
    child->t = "_a";

    AstNode *node = ast_new(cs, AST_BINARY, 1);
    node->u.binary.operator = "PLUS";
    node->t = NULL;  /* expression temporaries have no t set initially */
    ast_add_child(cs, node, child);
    ast_add_child(cs, node, child);

    ASSERT_TRUE(is_temporary_value(node));
    free_cs(cs);
}

int main(void) {
    printf("test_check (matching tests/api/test_check.py):\n");
    RUN_TEST(test_is_temporary_value_const_string);
    RUN_TEST(test_is_temporary_value_var);
    RUN_TEST(test_is_temporary_value_param);
    RUN_TEST(test_is_temporary_value_expr);
    REPORT();
}
