/*
 * test_ast.c — Tests for AST node creation and type system operations
 *
 * Matches: tests/symbols/test_symbolNOP.py, test_symbolNUMBER.py (init part),
 *          test_symbolSTRING.py, test_symbolBLOCK.py, test_symbolSENTENCE.py,
 *          and type system tests from test_symbolTYPE.py
 */
#include "test_harness.h"
#include "zxbc.h"

static CompilerState *new_cs(void) {
    static CompilerState cs;
    compiler_init(&cs);
    return &cs;
}

static void free_cs(CompilerState *cs) {
    compiler_destroy(cs);
}

/* ---- test_symbolNOP.py ---- */
TEST(test_ast_nop) {
    CompilerState *cs = new_cs();
    AstNode *n = ast_new(cs, AST_NOP, 1);
    ASSERT_NOT_NULL(n);
    ASSERT_EQ(n->tag, AST_NOP);
    ASSERT_EQ_INT(n->lineno, 1);
    ASSERT_NULL(n->parent);
    ASSERT_EQ_INT(n->child_count, 0);
    ASSERT_STR_EQ(ast_tag_name(n->tag), "NOP");
    free_cs(cs);
}

/* ---- test_symbolNUMBER.py: test__init__ (basic construction) ---- */
TEST(test_ast_number) {
    CompilerState *cs = new_cs();
    AstNode *n = ast_new(cs, AST_NUMBER, 1);
    n->u.number.value = 42.0;
    ASSERT_EQ(n->tag, AST_NUMBER);
    ASSERT_EQ_INT(n->lineno, 1);
    ASSERT_EQ_INT((int)n->u.number.value, 42);
    free_cs(cs);
}

/* ---- test_symbolSTRING.py ---- */
TEST(test_ast_string) {
    CompilerState *cs = new_cs();
    AstNode *n = ast_new(cs, AST_STRING, 1);
    n->u.string.value = "Hello";
    n->u.string.length = 5;
    ASSERT_EQ(n->tag, AST_STRING);
    ASSERT_STR_EQ(n->u.string.value, "Hello");
    ASSERT_EQ_INT(n->u.string.length, 5);
    free_cs(cs);
}

/* ---- test_symbolBLOCK.py ---- */
TEST(test_ast_block) {
    CompilerState *cs = new_cs();
    AstNode *block = ast_new(cs, AST_BLOCK, 1);
    AstNode *s1 = ast_new(cs, AST_NOP, 1);
    AstNode *s2 = ast_new(cs, AST_NOP, 2);

    ast_add_child(cs, block, s1);
    ast_add_child(cs, block, s2);

    ASSERT_EQ_INT(block->child_count, 2);
    ASSERT_EQ(block->children[0], s1);
    ASSERT_EQ(block->children[1], s2);
    ASSERT_EQ(s1->parent, block);
    ASSERT_EQ(s2->parent, block);
    free_cs(cs);
}

/* ---- test_symbolSENTENCE.py ---- */
TEST(test_ast_sentence) {
    CompilerState *cs = new_cs();
    AstNode *s = ast_new(cs, AST_SENTENCE, 5);
    s->u.sentence.kind = "LET";
    ASSERT_EQ(s->tag, AST_SENTENCE);
    ASSERT_EQ_INT(s->lineno, 5);
    ASSERT_STR_EQ(s->u.sentence.kind, "LET");
    free_cs(cs);
}

/* ---- ast_add_child NULL safety ---- */
TEST(test_ast_add_child_null) {
    CompilerState *cs = new_cs();
    AstNode *block = ast_new(cs, AST_BLOCK, 1);
    ast_add_child(cs, block, NULL);
    ASSERT_EQ_INT(block->child_count, 0);
    free_cs(cs);
}

/* ---- Type system: type_new_basic ---- */
TEST(test_type_new_basic) {
    CompilerState *cs = new_cs();
    TypeInfo *t = type_new_basic(cs, TYPE_integer);
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(t->tag, AST_BASICTYPE);
    ASSERT_EQ(t->basic_type, TYPE_integer);
    ASSERT_STR_EQ(t->name, "integer");
    ASSERT_EQ_INT(t->size, 2);
    ASSERT_TRUE(type_is_basic(t));
    ASSERT_TRUE(type_is_numeric(t));
    ASSERT_TRUE(type_is_signed(t));
    ASSERT_FALSE(type_is_string(t));
    free_cs(cs);
}

/* ---- Type system: type_equal ---- */
TEST(test_type_equal) {
    CompilerState *cs = new_cs();
    TypeInfo *t1 = type_new_basic(cs, TYPE_integer);
    TypeInfo *t2 = type_new_basic(cs, TYPE_integer);
    TypeInfo *t3 = type_new_basic(cs, TYPE_byte);

    ASSERT_TRUE(type_equal(t1, t2));
    ASSERT_FALSE(type_equal(t1, t3));
    ASSERT_TRUE(type_equal(t1, t1));
    ASSERT_FALSE(type_equal(t1, NULL));
    ASSERT_FALSE(type_equal(NULL, t1));
    free_cs(cs);
}

/* ---- Type system: type_new_alias ---- */
TEST(test_type_alias) {
    CompilerState *cs = new_cs();
    TypeInfo *base = type_new_basic(cs, TYPE_integer);
    TypeInfo *alias = type_new_alias(cs, "myint", 10, base);

    ASSERT_EQ(alias->tag, AST_TYPEALIAS);
    ASSERT_STR_EQ(alias->name, "myint");
    ASSERT_EQ(alias->final_type, base);
    ASSERT_TRUE(type_equal(alias, base));
    free_cs(cs);
}

/* ---- Type system: type_new_ref ---- */
TEST(test_type_ref) {
    CompilerState *cs = new_cs();
    TypeInfo *base = type_new_basic(cs, TYPE_float);
    TypeInfo *ref = type_new_ref(cs, base, 5, true);

    ASSERT_EQ(ref->tag, AST_TYPEREF);
    ASSERT_TRUE(ref->implicit);
    ASSERT_EQ(ref->final_type, base);
    ASSERT_TRUE(type_equal(ref, base));
    free_cs(cs);
}

/* ---- Type system: type_is_string ---- */
TEST(test_type_is_string) {
    CompilerState *cs = new_cs();
    TypeInfo *ts = type_new_basic(cs, TYPE_string);
    TypeInfo *ti = type_new_basic(cs, TYPE_integer);

    ASSERT_TRUE(type_is_string(ts));
    ASSERT_FALSE(type_is_string(ti));
    free_cs(cs);
}

/* ---- Type system: type_is_dynamic ---- */
TEST(test_type_is_dynamic) {
    CompilerState *cs = new_cs();
    TypeInfo *ts = type_new_basic(cs, TYPE_string);
    TypeInfo *ti = type_new_basic(cs, TYPE_integer);

    ASSERT_TRUE(type_is_dynamic(ts));
    ASSERT_FALSE(type_is_dynamic(ti));
    free_cs(cs);
}

/* ---- Type system: type_size ---- */
TEST(test_type_size) {
    CompilerState *cs = new_cs();
    for (int i = 0; i < TYPE_COUNT; i++) {
        TypeInfo *t = type_new_basic(cs, (BasicType)i);
        ASSERT_EQ_INT(type_size(t), basictype_size((BasicType)i));
    }
    free_cs(cs);
}

int main(void) {
    printf("test_ast (matching tests/symbols/ node tests):\n");
    RUN_TEST(test_ast_nop);
    RUN_TEST(test_ast_number);
    RUN_TEST(test_ast_string);
    RUN_TEST(test_ast_block);
    RUN_TEST(test_ast_sentence);
    RUN_TEST(test_ast_add_child_null);
    RUN_TEST(test_type_new_basic);
    RUN_TEST(test_type_equal);
    RUN_TEST(test_type_alias);
    RUN_TEST(test_type_ref);
    RUN_TEST(test_type_is_string);
    RUN_TEST(test_type_is_dynamic);
    RUN_TEST(test_type_size);
    REPORT();
}
