/*
 * test_ast.c — Tests for AST nodes and type system
 *
 * Matches all tests/symbols/ Python test files:
 *   test_symbolNOP.py, test_symbolNUMBER.py, test_symbolSTRING.py,
 *   test_symbolBINARY.py, test_symbolBLOCK.py, test_symbolSENTENCE.py,
 *   test_symbolBOUND.py, test_symbolBOUNDLIST.py, test_symbolARGLIST.py,
 *   test_symbolARRAYACCESS.py, test_symbolFUNCDECL.py, test_symbolFUNCTION.py,
 *   test_symbolLABEL.py, test_symbolSTRSLICE.py, test_symbolTYPE.py,
 *   test_symbolTYPEALIAS.py, test_symbolTYPECAST.py, test_symbolVAR.py,
 *   test_symbolVARARRAY.py
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

/* ================================================================
 * test_symbolNOP.py
 * ================================================================ */

TEST(test_nop_len_0) {
    CompilerState *cs = new_cs();
    AstNode *n = ast_new(cs, AST_NOP, 0);
    ASSERT_EQ_INT(n->child_count, 0);
    free_cs(cs);
}

TEST(test_nop_tag) {
    CompilerState *cs = new_cs();
    AstNode *n = ast_new(cs, AST_NOP, 0);
    ASSERT_EQ(n->tag, AST_NOP);
    ASSERT_STR_EQ(ast_tag_name(n->tag), "NOP");
    free_cs(cs);
}

/* ================================================================
 * test_symbolNUMBER.py
 * ================================================================ */

TEST(test_number_type_ubyte) {
    /* NUMBER(0) => ubyte */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, 0, 1);
    ASSERT_EQ(n->type_->basic_type, TYPE_ubyte);
    free_cs(cs);
}

TEST(test_number_type_byte) {
    /* NUMBER(-1) => byte */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, -1, 1);
    ASSERT_EQ(n->type_->basic_type, TYPE_byte);
    free_cs(cs);
}

TEST(test_number_type_uinteger) {
    /* NUMBER(256) => uinteger */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, 256, 1);
    ASSERT_EQ(n->type_->basic_type, TYPE_uinteger);
    free_cs(cs);
}

TEST(test_number_type_integer) {
    /* NUMBER(-256) => integer */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, -256, 1);
    ASSERT_EQ(n->type_->basic_type, TYPE_integer);
    free_cs(cs);
}

TEST(test_number_type_float) {
    /* NUMBER(3.14) => float */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, 3.14, 1);
    ASSERT_EQ(n->type_->basic_type, TYPE_float);
    free_cs(cs);
}

TEST(test_number_t) {
    /* n.t == "3.14" */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, 3.14, 1);
    ASSERT_STR_EQ(n->t, "3.14");
    free_cs(cs);
}

TEST(test_number_t_integer) {
    /* NUMBER(3).t == "3" */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, 3, 1);
    ASSERT_STR_EQ(n->t, "3");
    free_cs(cs);
}

TEST(test_number_cmp) {
    /* NUMBER(0) != NUMBER(1), NUMBER(0) == NUMBER(0) */
    CompilerState *cs = new_cs();
    AstNode *n = ast_number(cs, 0, 1);
    AstNode *m = ast_number(cs, 1, 2);
    ASSERT_TRUE(n->u.number.value != m->u.number.value);
    ASSERT_TRUE(n->u.number.value == 0);
    ASSERT_TRUE(m->u.number.value > n->u.number.value);
    free_cs(cs);
}

/* ================================================================
 * test_symbolSTRING.py
 * ================================================================ */

TEST(test_string_init) {
    CompilerState *cs = new_cs();
    AstNode *s = ast_new(cs, AST_STRING, 1);
    s->u.string.value = arena_strdup(&cs->arena, "zxbasic");
    s->u.string.length = 7;
    s->type_ = cs->symbol_table->basic_types[TYPE_string];

    ASSERT_STR_EQ(s->u.string.value, "zxbasic");
    ASSERT_EQ_INT(s->u.string.length, 7);
    ASSERT_EQ(s->type_->basic_type, TYPE_string);
    free_cs(cs);
}

TEST(test_string_cmp) {
    CompilerState *cs = new_cs();
    AstNode *s = ast_new(cs, AST_STRING, 1);
    s->u.string.value = "zxbasic";
    AstNode *t = ast_new(cs, AST_STRING, 2);
    t->u.string.value = "ZXBASIC";

    ASSERT_TRUE(strcmp(s->u.string.value, t->u.string.value) != 0);
    ASSERT_TRUE(strcmp(s->u.string.value, "zxbasic") == 0);
    free_cs(cs);
}

/* ================================================================
 * test_symbolBINARY.py
 * ================================================================ */

TEST(test_binary_left_right) {
    CompilerState *cs = new_cs();
    AstNode *l = ast_number(cs, 5, 1);
    AstNode *r = ast_number(cs, 3, 2);
    AstNode *b = ast_new(cs, AST_BINARY, 3);
    b->u.binary.operator = "PLUS";
    ast_add_child(cs, b, l);
    ast_add_child(cs, b, r);

    /* left = child[0], right = child[1] */
    ASSERT_EQ(b->children[0], l);
    ASSERT_EQ(b->children[1], r);
    ASSERT_EQ_INT(b->child_count, 2);
    ASSERT_STR_EQ(b->u.binary.operator, "PLUS");
    free_cs(cs);
}

/* ================================================================
 * test_symbolBOUND.py
 * ================================================================ */

TEST(test_bound_construction) {
    CompilerState *cs = new_cs();
    AstNode *bound = ast_new(cs, AST_BOUND, 1);
    AstNode *lo = ast_number(cs, 1, 1);
    AstNode *hi = ast_number(cs, 3, 1);
    ast_add_child(cs, bound, lo);
    ast_add_child(cs, bound, hi);

    ASSERT_EQ(bound->tag, AST_BOUND);
    ASSERT_EQ_INT(bound->child_count, 2);
    /* count = upper - lower + 1 = 3 - 1 + 1 = 3 */
    int count = (int)(bound->children[1]->u.number.value -
                      bound->children[0]->u.number.value + 1);
    ASSERT_EQ_INT(count, 3);
    free_cs(cs);
}

/* ================================================================
 * test_symbolBOUNDLIST.py
 * ================================================================ */

TEST(test_boundlist_construction) {
    CompilerState *cs = new_cs();
    AstNode *bl = ast_new(cs, AST_BOUNDLIST, 1);

    /* Bound (1 TO 2) */
    AstNode *b1 = ast_new(cs, AST_BOUND, 1);
    ast_add_child(cs, b1, ast_number(cs, 1, 1));
    ast_add_child(cs, b1, ast_number(cs, 2, 1));

    /* Bound (3 TO 4) */
    AstNode *b2 = ast_new(cs, AST_BOUND, 1);
    ast_add_child(cs, b2, ast_number(cs, 3, 1));
    ast_add_child(cs, b2, ast_number(cs, 4, 1));

    ast_add_child(cs, bl, b1);
    ast_add_child(cs, bl, b2);

    ASSERT_EQ(bl->tag, AST_BOUNDLIST);
    ASSERT_EQ_INT(bl->child_count, 2);
    free_cs(cs);
}

/* ================================================================
 * test_symbolBLOCK.py
 * ================================================================ */

TEST(test_block_empty) {
    CompilerState *cs = new_cs();
    AstNode *b = ast_new(cs, AST_BLOCK, 0);
    ASSERT_EQ_INT(b->child_count, 0);
    free_cs(cs);
}

TEST(test_block_with_child) {
    CompilerState *cs = new_cs();
    AstNode *b = ast_new(cs, AST_BLOCK, 1);
    AstNode *n = ast_number(cs, 1, 1);
    ast_add_child(cs, b, n);

    ASSERT_EQ_INT(b->child_count, 1);
    ASSERT_EQ(b->children[0], n);
    free_cs(cs);
}

TEST(test_block_add_child_null) {
    CompilerState *cs = new_cs();
    AstNode *b = ast_new(cs, AST_BLOCK, 1);
    ast_add_child(cs, b, NULL);
    ASSERT_EQ_INT(b->child_count, 0);
    free_cs(cs);
}

TEST(test_block_parent_set) {
    CompilerState *cs = new_cs();
    AstNode *b = ast_new(cs, AST_BLOCK, 1);
    AstNode *n = ast_number(cs, 1, 1);
    ast_add_child(cs, b, n);
    ASSERT_EQ(n->parent, b);
    free_cs(cs);
}

/* ================================================================
 * test_symbolSENTENCE.py
 * ================================================================ */

TEST(test_sentence_token) {
    CompilerState *cs = new_cs();
    AstNode *s = ast_new(cs, AST_SENTENCE, 1);
    s->u.sentence.kind = "TOKEN";
    ASSERT_STR_EQ(s->u.sentence.kind, "TOKEN");
    free_cs(cs);
}

TEST(test_sentence_children) {
    CompilerState *cs = new_cs();
    AstNode *s = ast_new(cs, AST_SENTENCE, 1);
    s->u.sentence.kind = "LET";

    /* Non-NULL args become children */
    AstNode *child = ast_number(cs, 42, 1);
    ast_add_child(cs, s, child);
    ASSERT_EQ_INT(s->child_count, 1);
    ASSERT_EQ(s->children[0], child);
    free_cs(cs);
}

/* ================================================================
 * test_symbolARGLIST.py
 * ================================================================ */

TEST(test_arglist_empty) {
    CompilerState *cs = new_cs();
    AstNode *al = ast_new(cs, AST_ARGLIST, 1);
    ASSERT_EQ_INT(al->child_count, 0);
    free_cs(cs);
}

TEST(test_arglist_with_arg) {
    CompilerState *cs = new_cs();
    AstNode *al = ast_new(cs, AST_ARGLIST, 1);
    AstNode *arg = ast_new(cs, AST_ARGUMENT, 1);
    AstNode *val = ast_number(cs, 5, 1);
    ast_add_child(cs, arg, val);
    ast_add_child(cs, al, arg);

    ASSERT_EQ_INT(al->child_count, 1);
    ASSERT_EQ(al->children[0], arg);
    free_cs(cs);
}

/* ================================================================
 * test_symbolARRAYACCESS.py
 * ================================================================ */

TEST(test_arrayaccess_construction) {
    CompilerState *cs = new_cs();
    AstNode *aa = ast_new(cs, AST_ARRAYACCESS, 2);

    /* child[0] = array ID */
    AstNode *arr_id = ast_new(cs, AST_ID, 1);
    arr_id->u.id.name = "test";
    arr_id->u.id.class_ = CLASS_array;
    ast_add_child(cs, aa, arr_id);

    /* child[1] = arglist with indices */
    AstNode *args = ast_new(cs, AST_ARGLIST, 2);
    AstNode *idx = ast_new(cs, AST_ARGUMENT, 2);
    ast_add_child(cs, idx, ast_number(cs, 2, 2));
    ast_add_child(cs, args, idx);
    ast_add_child(cs, aa, args);

    ASSERT_EQ(aa->tag, AST_ARRAYACCESS);
    ASSERT_EQ_INT(aa->child_count, 2);
    ASSERT_EQ(aa->children[0], arr_id);
    free_cs(cs);
}

/* ================================================================
 * test_symbolFUNCDECL.py
 * ================================================================ */

TEST(test_funcdecl_construction) {
    CompilerState *cs = new_cs();
    AstNode *fd = ast_new(cs, AST_FUNCDECL, 1);

    /* child[0] = function ID */
    AstNode *id = ast_new(cs, AST_ID, 1);
    id->u.id.name = "f";
    id->u.id.class_ = CLASS_function;
    id->type_ = cs->symbol_table->basic_types[TYPE_ubyte];
    ast_add_child(cs, fd, id);

    /* child[1] = PARAMLIST */
    AstNode *params = ast_new(cs, AST_PARAMLIST, 1);
    ast_add_child(cs, fd, params);

    /* child[2] = body (BLOCK) */
    AstNode *body = ast_new(cs, AST_BLOCK, 1);
    ast_add_child(cs, fd, body);

    ASSERT_EQ(fd->tag, AST_FUNCDECL);
    ASSERT_EQ_INT(fd->child_count, 3);
    ASSERT_STR_EQ(fd->children[0]->u.id.name, "f");
    ASSERT_EQ(fd->children[0]->u.id.class_, CLASS_function);
    ASSERT_EQ(fd->children[1]->tag, AST_PARAMLIST);
    ASSERT_EQ(fd->children[2]->tag, AST_BLOCK);
    free_cs(cs);
}

TEST(test_funcdecl_locals_size) {
    CompilerState *cs = new_cs();
    AstNode *id = ast_new(cs, AST_ID, 1);
    id->u.id.name = "f";
    id->u.id.class_ = CLASS_function;
    id->u.id.local_size = 0;
    ASSERT_EQ_INT(id->u.id.local_size, 0);
    free_cs(cs);
}

/* ================================================================
 * test_symbolFUNCTION.py
 * ================================================================ */

TEST(test_function_id_properties) {
    CompilerState *cs = new_cs();
    AstNode *f = ast_new(cs, AST_ID, 1);
    f->u.id.name = "test";
    f->u.id.class_ = CLASS_function;
    f->u.id.scope = SCOPE_global;

    ASSERT_EQ(f->u.id.class_, CLASS_function);
    ASSERT_STR_EQ(f->u.id.name, "test");
    free_cs(cs);
}

/* ================================================================
 * test_symbolLABEL.py
 * ================================================================ */

TEST(test_label_construction) {
    CompilerState *cs = new_cs();
    AstNode *l = ast_new(cs, AST_ID, 1);
    l->u.id.name = "test";
    l->u.id.class_ = CLASS_label;
    l->u.id.accessed = false;

    ASSERT_EQ(l->u.id.class_, CLASS_label);
    ASSERT_FALSE(l->u.id.accessed);
    free_cs(cs);
}

TEST(test_label_accessed) {
    CompilerState *cs = new_cs();
    AstNode *l = ast_new(cs, AST_ID, 1);
    l->u.id.name = "test";
    l->u.id.class_ = CLASS_label;
    l->u.id.accessed = false;

    l->u.id.accessed = true;
    ASSERT_TRUE(l->u.id.accessed);
    free_cs(cs);
}

/* ================================================================
 * test_symbolSTRSLICE.py
 * ================================================================ */

TEST(test_strslice_construction) {
    CompilerState *cs = new_cs();
    AstNode *ss = ast_new(cs, AST_STRSLICE, 1);

    AstNode *str = ast_new(cs, AST_STRING, 1);
    str->u.string.value = "ZXBASIC";
    str->u.string.length = 7;

    AstNode *lo = ast_number(cs, 1, 1);
    AstNode *hi = ast_number(cs, 2, 1);

    /* child[0] = string, child[1] = lower, child[2] = upper */
    ast_add_child(cs, ss, str);
    ast_add_child(cs, ss, lo);
    ast_add_child(cs, ss, hi);

    ASSERT_EQ(ss->tag, AST_STRSLICE);
    ASSERT_EQ_INT(ss->child_count, 3);
    ASSERT_EQ(ss->children[0], str);
    ASSERT_EQ(ss->children[1], lo);
    ASSERT_EQ(ss->children[2], hi);
    free_cs(cs);
}

/* ================================================================
 * test_symbolTYPE.py
 * ================================================================ */

TEST(test_type_basic) {
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

TEST(test_type_equal) {
    CompilerState *cs = new_cs();
    TypeInfo *t1 = type_new_basic(cs, TYPE_integer);
    TypeInfo *t2 = type_new_basic(cs, TYPE_integer);
    TypeInfo *t3 = type_new_basic(cs, TYPE_byte);

    ASSERT_TRUE(type_equal(t1, t2));
    ASSERT_TRUE(type_equal(t1, t1));
    ASSERT_FALSE(type_equal(t1, t3));
    ASSERT_FALSE(type_equal(t1, NULL));
    ASSERT_FALSE(type_equal(NULL, t1));
    free_cs(cs);
}

TEST(test_type_size_all) {
    CompilerState *cs = new_cs();
    /* Verify sizes match Python's type system */
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_byte)), 1);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_ubyte)), 1);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_integer)), 2);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_uinteger)), 2);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_long)), 4);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_ulong)), 4);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_fixed)), 4);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_float)), 5);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_string)), 2);
    ASSERT_EQ_INT(type_size(type_new_basic(cs, TYPE_boolean)), 1);
    free_cs(cs);
}

TEST(test_type_is_basic_false_for_composite) {
    /* Composite types (AST_TYPE) are not basic */
    CompilerState *cs = new_cs();
    TypeInfo *t = type_new(cs, "my_struct", 10);
    ASSERT_FALSE(type_is_basic(t));
    free_cs(cs);
}

/* ================================================================
 * test_symbolTYPEALIAS.py
 * ================================================================ */

TEST(test_typealias_equal) {
    CompilerState *cs = new_cs();
    TypeInfo *base = type_new_basic(cs, TYPE_integer);
    TypeInfo *alias = type_new_alias(cs, "alias", 0, base);

    ASSERT_EQ(alias->tag, AST_TYPEALIAS);
    ASSERT_EQ_INT(type_size(alias), type_size(base));
    ASSERT_TRUE(type_equal(alias, alias));
    ASSERT_TRUE(type_equal(base, alias));
    ASSERT_TRUE(type_equal(alias, base));
    free_cs(cs);
}

TEST(test_typealias_is_alias) {
    CompilerState *cs = new_cs();
    TypeInfo *base = type_new_basic(cs, TYPE_integer);
    TypeInfo *alias = type_new_alias(cs, "alias", 0, base);

    ASSERT_EQ(alias->tag, AST_TYPEALIAS);
    /* Alias resolves to basic type */
    ASSERT_TRUE(type_is_basic(alias));
    /* Base is not an alias */
    ASSERT_EQ(base->tag, AST_BASICTYPE);
    free_cs(cs);
}

/* ================================================================
 * test_symbolTYPECAST.py
 * ================================================================ */

TEST(test_typecast_construction) {
    CompilerState *cs = new_cs();
    AstNode *tc = ast_new(cs, AST_TYPECAST, 2);
    tc->type_ = cs->symbol_table->basic_types[TYPE_float];

    AstNode *operand = ast_number(cs, 3, 1);
    ast_add_child(cs, tc, operand);

    ASSERT_EQ(tc->tag, AST_TYPECAST);
    ASSERT_EQ(tc->type_->basic_type, TYPE_float);
    ASSERT_EQ_INT(tc->child_count, 1);
    ASSERT_EQ(tc->children[0], operand);
    /* operand type is ubyte (3 fits in ubyte) */
    ASSERT_EQ(operand->type_->basic_type, TYPE_ubyte);
    free_cs(cs);
}

/* ================================================================
 * test_symbolTYPEREF (test_symbolTYPE.py: test_cmp_types)
 * ================================================================ */

TEST(test_typeref_construction) {
    CompilerState *cs = new_cs();
    TypeInfo *base = type_new_basic(cs, TYPE_float);
    TypeInfo *ref = type_new_ref(cs, base, 5, true);

    ASSERT_EQ(ref->tag, AST_TYPEREF);
    ASSERT_TRUE(ref->implicit);
    ASSERT_EQ(ref->final_type, base);
    ASSERT_TRUE(type_equal(ref, base));
    free_cs(cs);
}

TEST(test_typeref_not_implicit) {
    CompilerState *cs = new_cs();
    TypeInfo *base = type_new_basic(cs, TYPE_integer);
    TypeInfo *ref = type_new_ref(cs, base, 0, false);

    ASSERT_FALSE(ref->implicit);
    ASSERT_TRUE(type_equal(ref, base));
    free_cs(cs);
}

/* ================================================================
 * test_symbolVAR.py
 * ================================================================ */

TEST(test_var_construction) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    AstNode *v = symboltable_declare_variable(st, cs, "v", 1,
        type_new_ref(cs, st->basic_types[TYPE_byte], 0, false));
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(v->u.id.class_, CLASS_var);
    ASSERT_EQ(v->u.id.scope, SCOPE_global);
    ASSERT_TRUE(type_equal(v->type_, st->basic_types[TYPE_byte]));
    free_cs(cs);
}

TEST(test_var_scope_local) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);
    AstNode *v = symboltable_declare_variable(st, cs, "v", 1,
        type_new_ref(cs, st->basic_types[TYPE_integer], 0, false));
    ASSERT_EQ(v->u.id.scope, SCOPE_local);
    free_cs(cs);
}

TEST(test_var_t_property) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    AstNode *v = symboltable_declare_variable(st, cs, "v", 1,
        type_new_ref(cs, st->basic_types[TYPE_integer], 0, false));
    /* Global vars get "_name" prefix in t */
    ASSERT_NOT_NULL(v->t);
    ASSERT_STR_EQ(v->t, "_v");
    free_cs(cs);
}

TEST(test_var_string_t) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    AstNode *v = symboltable_declare_variable(st, cs, "s", 1,
        type_new_ref(cs, st->basic_types[TYPE_string], 0, false));
    /* String vars get "$name" prefix in t */
    ASSERT_NOT_NULL(v->t);
    ASSERT_EQ(v->t[0], '$');
    free_cs(cs);
}

/* ================================================================
 * test_symbolVARARRAY.py
 * ================================================================ */

TEST(test_vararray_construction) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    /* Create bounds: (1 TO 2), (3 TO 4) */
    AstNode *bl = ast_new(cs, AST_BOUNDLIST, 1);
    AstNode *b1 = ast_new(cs, AST_BOUND, 1);
    ast_add_child(cs, b1, ast_number(cs, 1, 1));
    ast_add_child(cs, b1, ast_number(cs, 2, 1));
    AstNode *b2 = ast_new(cs, AST_BOUND, 1);
    ast_add_child(cs, b2, ast_number(cs, 3, 1));
    ast_add_child(cs, b2, ast_number(cs, 4, 1));
    ast_add_child(cs, bl, b1);
    ast_add_child(cs, bl, b2);

    TypeInfo *tref = type_new_ref(cs, st->basic_types[TYPE_ubyte], 0, false);
    AstNode *arr = symboltable_declare_array(st, cs, "test", 1, tref, bl);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ(arr->u.id.class_, CLASS_array);
    ASSERT_TRUE(type_equal(arr->type_, st->basic_types[TYPE_ubyte]));
    free_cs(cs);
}

TEST(test_vararray_scope_local) {
    CompilerState *cs = new_cs();
    SymbolTable *st = cs->symbol_table;

    symboltable_enter_scope(st, cs);

    AstNode *bl = ast_new(cs, AST_BOUNDLIST, 1);
    AstNode *b = ast_new(cs, AST_BOUND, 1);
    ast_add_child(cs, b, ast_number(cs, 0, 1));
    ast_add_child(cs, b, ast_number(cs, 2, 1));
    ast_add_child(cs, bl, b);

    TypeInfo *tref = type_new_ref(cs, st->basic_types[TYPE_float], 0, false);
    AstNode *arr = symboltable_declare_array(st, cs, "a", 12, tref, bl);
    ASSERT_NOT_NULL(arr);
    ASSERT_EQ(arr->u.id.scope, SCOPE_local);
    free_cs(cs);
}

/* ================================================================
 * test_symbolCONSTEXPR
 * ================================================================ */

TEST(test_constexpr_construction) {
    CompilerState *cs = new_cs();
    AstNode *ce = ast_new(cs, AST_CONSTEXPR, 1);
    AstNode *val = ast_number(cs, 42, 1);
    ast_add_child(cs, ce, val);

    ASSERT_EQ(ce->tag, AST_CONSTEXPR);
    ASSERT_EQ_INT(ce->child_count, 1);
    ASSERT_EQ(ce->children[0], val);
    free_cs(cs);
}

/* ================================================================
 * test_symbolASM
 * ================================================================ */

TEST(test_asm_construction) {
    CompilerState *cs = new_cs();
    AstNode *a = ast_new(cs, AST_ASM, 10);
    a->u.asm_block.code = "LD A, 42";

    ASSERT_EQ(a->tag, AST_ASM);
    ASSERT_STR_EQ(a->u.asm_block.code, "LD A, 42");
    free_cs(cs);
}

/* ================================================================
 * test_symbolVARDECL
 * ================================================================ */

TEST(test_vardecl_construction) {
    CompilerState *cs = new_cs();
    AstNode *vd = ast_new(cs, AST_VARDECL, 1);

    AstNode *id = ast_new(cs, AST_ID, 1);
    id->u.id.name = "x";
    id->u.id.class_ = CLASS_var;

    AstNode *init = ast_number(cs, 0, 1);

    ast_add_child(cs, vd, id);
    ast_add_child(cs, vd, init);

    ASSERT_EQ(vd->tag, AST_VARDECL);
    ASSERT_EQ_INT(vd->child_count, 2);
    ASSERT_STR_EQ(vd->children[0]->u.id.name, "x");
    free_cs(cs);
}

/* ================================================================
 * test_symbolARRAYDECL
 * ================================================================ */

TEST(test_arraydecl_construction) {
    CompilerState *cs = new_cs();
    AstNode *ad = ast_new(cs, AST_ARRAYDECL, 1);

    AstNode *id = ast_new(cs, AST_ID, 1);
    id->u.id.name = "arr";
    id->u.id.class_ = CLASS_array;

    AstNode *bl = ast_new(cs, AST_BOUNDLIST, 1);
    AstNode *b = ast_new(cs, AST_BOUND, 1);
    ast_add_child(cs, b, ast_number(cs, 0, 1));
    ast_add_child(cs, b, ast_number(cs, 9, 1));
    ast_add_child(cs, bl, b);

    ast_add_child(cs, ad, id);
    ast_add_child(cs, ad, bl);

    ASSERT_EQ(ad->tag, AST_ARRAYDECL);
    ASSERT_EQ_INT(ad->child_count, 2);
    ASSERT_EQ(ad->children[1]->tag, AST_BOUNDLIST);
    free_cs(cs);
}

/* ================================================================
 * Additional type property tests
 * ================================================================ */

TEST(test_type_is_string) {
    CompilerState *cs = new_cs();
    ASSERT_TRUE(type_is_string(type_new_basic(cs, TYPE_string)));
    ASSERT_FALSE(type_is_string(type_new_basic(cs, TYPE_integer)));
    free_cs(cs);
}

TEST(test_type_is_dynamic) {
    CompilerState *cs = new_cs();
    ASSERT_TRUE(type_is_dynamic(type_new_basic(cs, TYPE_string)));
    ASSERT_FALSE(type_is_dynamic(type_new_basic(cs, TYPE_integer)));
    free_cs(cs);
}

TEST(test_type_is_numeric) {
    CompilerState *cs = new_cs();
    ASSERT_TRUE(type_is_numeric(type_new_basic(cs, TYPE_integer)));
    ASSERT_TRUE(type_is_numeric(type_new_basic(cs, TYPE_float)));
    ASSERT_TRUE(type_is_numeric(type_new_basic(cs, TYPE_byte)));
    ASSERT_FALSE(type_is_numeric(type_new_basic(cs, TYPE_string)));
    free_cs(cs);
}

TEST(test_type_is_signed) {
    CompilerState *cs = new_cs();
    ASSERT_TRUE(type_is_signed(type_new_basic(cs, TYPE_integer)));
    ASSERT_TRUE(type_is_signed(type_new_basic(cs, TYPE_byte)));
    ASSERT_TRUE(type_is_signed(type_new_basic(cs, TYPE_float)));
    ASSERT_FALSE(type_is_signed(type_new_basic(cs, TYPE_ubyte)));
    ASSERT_FALSE(type_is_signed(type_new_basic(cs, TYPE_uinteger)));
    free_cs(cs);
}

/* ================================================================
 * PARAMLIST construction
 * ================================================================ */

TEST(test_paramlist_construction) {
    CompilerState *cs = new_cs();
    AstNode *pl = ast_new(cs, AST_PARAMLIST, 1);
    ASSERT_EQ(pl->tag, AST_PARAMLIST);
    ASSERT_EQ_INT(pl->child_count, 0);

    /* Add parameter */
    AstNode *arg = ast_new(cs, AST_ARGUMENT, 1);
    arg->u.argument.name = "x";
    arg->u.argument.byref = false;
    arg->u.argument.is_array = false;
    ast_add_child(cs, pl, arg);

    ASSERT_EQ_INT(pl->child_count, 1);
    ASSERT_STR_EQ(pl->children[0]->u.argument.name, "x");
    free_cs(cs);
}

/* ================================================================
 * ARGUMENT node
 * ================================================================ */

TEST(test_argument_properties) {
    CompilerState *cs = new_cs();
    AstNode *arg = ast_new(cs, AST_ARGUMENT, 1);
    arg->u.argument.name = "param1";
    arg->u.argument.byref = true;
    arg->u.argument.is_array = false;

    ASSERT_STR_EQ(arg->u.argument.name, "param1");
    ASSERT_TRUE(arg->u.argument.byref);
    ASSERT_FALSE(arg->u.argument.is_array);
    free_cs(cs);
}

/* ================================================================
 * UNARY node
 * ================================================================ */

TEST(test_unary_construction) {
    CompilerState *cs = new_cs();
    AstNode *u = ast_new(cs, AST_UNARY, 1);
    u->u.unary.operator = "MINUS";
    AstNode *operand = ast_number(cs, 5, 1);
    ast_add_child(cs, u, operand);

    ASSERT_EQ(u->tag, AST_UNARY);
    ASSERT_STR_EQ(u->u.unary.operator, "MINUS");
    ASSERT_EQ_INT(u->child_count, 1);
    ASSERT_EQ(u->children[0], operand);
    free_cs(cs);
}

/* ================================================================
 * BUILTIN node
 * ================================================================ */

TEST(test_builtin_construction) {
    CompilerState *cs = new_cs();
    AstNode *b = ast_new(cs, AST_BUILTIN, 1);
    b->u.builtin.fname = "ABS";
    b->u.builtin.func = "__ABS";
    AstNode *arg = ast_number(cs, -5, 1);
    ast_add_child(cs, b, arg);

    ASSERT_EQ(b->tag, AST_BUILTIN);
    ASSERT_STR_EQ(b->u.builtin.fname, "ABS");
    ASSERT_STR_EQ(b->u.builtin.func, "__ABS");
    ASSERT_EQ_INT(b->child_count, 1);
    free_cs(cs);
}

/* ================================================================
 * CALL / FUNCCALL nodes
 * ================================================================ */

TEST(test_call_construction) {
    CompilerState *cs = new_cs();
    AstNode *call = ast_new(cs, AST_CALL, 1);

    AstNode *callee = ast_new(cs, AST_ID, 1);
    callee->u.id.name = "myfunc";
    callee->u.id.class_ = CLASS_function;

    AstNode *args = ast_new(cs, AST_ARGLIST, 1);

    ast_add_child(cs, call, callee);
    ast_add_child(cs, call, args);

    ASSERT_EQ(call->tag, AST_CALL);
    ASSERT_EQ_INT(call->child_count, 2);
    ASSERT_STR_EQ(call->children[0]->u.id.name, "myfunc");
    ASSERT_EQ(call->children[1]->tag, AST_ARGLIST);
    free_cs(cs);
}

/* ================================================================
 * ARRAYINIT node
 * ================================================================ */

TEST(test_arrayinit_construction) {
    CompilerState *cs = new_cs();
    AstNode *ai = ast_new(cs, AST_ARRAYINIT, 1);
    ast_add_child(cs, ai, ast_number(cs, 1, 1));
    ast_add_child(cs, ai, ast_number(cs, 2, 1));
    ast_add_child(cs, ai, ast_number(cs, 3, 1));

    ASSERT_EQ(ai->tag, AST_ARRAYINIT);
    ASSERT_EQ_INT(ai->child_count, 3);
    free_cs(cs);
}

/* ================================================================
 * ID node properties
 * ================================================================ */

TEST(test_id_all_fields) {
    CompilerState *cs = new_cs();
    AstNode *id = ast_new(cs, AST_ID, 5);
    id->u.id.name = "myvar";
    id->u.id.class_ = CLASS_var;
    id->u.id.scope = SCOPE_global;
    id->u.id.convention = CONV_fastcall;
    id->u.id.byref = true;
    id->u.id.accessed = true;
    id->u.id.forwarded = false;
    id->u.id.declared = true;
    id->u.id.addr = 0x8000;
    id->u.id.addr_set = true;

    ASSERT_STR_EQ(id->u.id.name, "myvar");
    ASSERT_EQ(id->u.id.class_, CLASS_var);
    ASSERT_EQ(id->u.id.scope, SCOPE_global);
    ASSERT_EQ(id->u.id.convention, CONV_fastcall);
    ASSERT_TRUE(id->u.id.byref);
    ASSERT_TRUE(id->u.id.accessed);
    ASSERT_FALSE(id->u.id.forwarded);
    ASSERT_TRUE(id->u.id.declared);
    ASSERT_EQ_INT((int)id->u.id.addr, 0x8000);
    ASSERT_TRUE(id->u.id.addr_set);
    free_cs(cs);
}

/* ================================================================
 * Tag name coverage
 * ================================================================ */

TEST(test_tag_names) {
    ASSERT_STR_EQ(ast_tag_name(AST_NOP), "NOP");
    ASSERT_STR_EQ(ast_tag_name(AST_NUMBER), "NUMBER");
    ASSERT_STR_EQ(ast_tag_name(AST_STRING), "STRING");
    ASSERT_STR_EQ(ast_tag_name(AST_BINARY), "BINARY");
    ASSERT_STR_EQ(ast_tag_name(AST_UNARY), "UNARY");
    ASSERT_STR_EQ(ast_tag_name(AST_ID), "ID");
    ASSERT_STR_EQ(ast_tag_name(AST_BLOCK), "BLOCK");
    ASSERT_STR_EQ(ast_tag_name(AST_BOUND), "BOUND");
    ASSERT_STR_EQ(ast_tag_name(AST_BOUNDLIST), "BOUNDLIST");
    ASSERT_STR_EQ(ast_tag_name(AST_SENTENCE), "SENTENCE");
    ASSERT_STR_EQ(ast_tag_name(AST_ASM), "ASM");
    ASSERT_STR_EQ(ast_tag_name(AST_CONSTEXPR), "CONSTEXPR");
    ASSERT_STR_EQ(ast_tag_name(AST_TYPECAST), "TYPECAST");
    ASSERT_STR_EQ(ast_tag_name(AST_TYPEREF), "TYPEREF");
    ASSERT_STR_EQ(ast_tag_name(AST_BASICTYPE), "BASICTYPE");
    ASSERT_STR_EQ(ast_tag_name(AST_TYPEALIAS), "TYPEALIAS");
}

int main(void) {
    printf("test_ast (matching tests/symbols/ — all node types):\n");

    /* NOP */
    RUN_TEST(test_nop_len_0);
    RUN_TEST(test_nop_tag);

    /* NUMBER */
    RUN_TEST(test_number_type_ubyte);
    RUN_TEST(test_number_type_byte);
    RUN_TEST(test_number_type_uinteger);
    RUN_TEST(test_number_type_integer);
    RUN_TEST(test_number_type_float);
    RUN_TEST(test_number_t);
    RUN_TEST(test_number_t_integer);
    RUN_TEST(test_number_cmp);

    /* STRING */
    RUN_TEST(test_string_init);
    RUN_TEST(test_string_cmp);

    /* BINARY */
    RUN_TEST(test_binary_left_right);

    /* BOUND */
    RUN_TEST(test_bound_construction);

    /* BOUNDLIST */
    RUN_TEST(test_boundlist_construction);

    /* BLOCK */
    RUN_TEST(test_block_empty);
    RUN_TEST(test_block_with_child);
    RUN_TEST(test_block_add_child_null);
    RUN_TEST(test_block_parent_set);

    /* SENTENCE */
    RUN_TEST(test_sentence_token);
    RUN_TEST(test_sentence_children);

    /* ARGLIST */
    RUN_TEST(test_arglist_empty);
    RUN_TEST(test_arglist_with_arg);

    /* ARRAYACCESS */
    RUN_TEST(test_arrayaccess_construction);

    /* FUNCDECL */
    RUN_TEST(test_funcdecl_construction);
    RUN_TEST(test_funcdecl_locals_size);

    /* FUNCTION (ID) */
    RUN_TEST(test_function_id_properties);

    /* LABEL */
    RUN_TEST(test_label_construction);
    RUN_TEST(test_label_accessed);

    /* STRSLICE */
    RUN_TEST(test_strslice_construction);

    /* TYPE */
    RUN_TEST(test_type_basic);
    RUN_TEST(test_type_equal);
    RUN_TEST(test_type_size_all);
    RUN_TEST(test_type_is_basic_false_for_composite);

    /* TYPEALIAS */
    RUN_TEST(test_typealias_equal);
    RUN_TEST(test_typealias_is_alias);

    /* TYPECAST */
    RUN_TEST(test_typecast_construction);

    /* TYPEREF */
    RUN_TEST(test_typeref_construction);
    RUN_TEST(test_typeref_not_implicit);

    /* VAR */
    RUN_TEST(test_var_construction);
    RUN_TEST(test_var_scope_local);
    RUN_TEST(test_var_t_property);
    RUN_TEST(test_var_string_t);

    /* VARARRAY */
    RUN_TEST(test_vararray_construction);
    RUN_TEST(test_vararray_scope_local);

    /* CONSTEXPR */
    RUN_TEST(test_constexpr_construction);

    /* ASM */
    RUN_TEST(test_asm_construction);

    /* VARDECL */
    RUN_TEST(test_vardecl_construction);

    /* ARRAYDECL */
    RUN_TEST(test_arraydecl_construction);

    /* Additional type properties */
    RUN_TEST(test_type_is_string);
    RUN_TEST(test_type_is_dynamic);
    RUN_TEST(test_type_is_numeric);
    RUN_TEST(test_type_is_signed);

    /* PARAMLIST */
    RUN_TEST(test_paramlist_construction);

    /* ARGUMENT */
    RUN_TEST(test_argument_properties);

    /* UNARY */
    RUN_TEST(test_unary_construction);

    /* BUILTIN */
    RUN_TEST(test_builtin_construction);

    /* CALL */
    RUN_TEST(test_call_construction);

    /* ARRAYINIT */
    RUN_TEST(test_arrayinit_construction);

    /* ID full properties */
    RUN_TEST(test_id_all_fields);

    /* Tag names */
    RUN_TEST(test_tag_names);

    REPORT();
}
