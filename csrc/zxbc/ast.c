/*
 * ast.c — AST node creation and manipulation
 *
 * Ported from src/ast/tree.py, src/symbols/symbol_.py
 */
#include "zxbc.h"
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * AST node creation
 * ---------------------------------------------------------------- */

AstNode *ast_new(CompilerState *cs, AstTag tag, int lineno) {
    AstNode *n = arena_calloc(&cs->arena, 1, sizeof(AstNode));
    n->tag = tag;
    n->lineno = lineno;
    n->parent = NULL;
    n->children = NULL;
    n->child_count = 0;
    n->child_cap = 0;
    n->type_ = NULL;
    n->t = NULL;
    return n;
}

void ast_add_child(CompilerState *cs, AstNode *parent, AstNode *child) {
    if (child == NULL) return;

    if (parent->child_count >= parent->child_cap) {
        int new_cap = parent->child_cap < 4 ? 4 : parent->child_cap * 2;
        AstNode **new_children = arena_alloc(&cs->arena, new_cap * sizeof(AstNode *));
        if (parent->children) {
            memcpy(new_children, parent->children, parent->child_count * sizeof(AstNode *));
        }
        parent->children = new_children;
        parent->child_cap = new_cap;
    }

    child->parent = parent;
    parent->children[parent->child_count++] = child;
}

/* Create a NUMBER node with auto type inference from value
 * Matches Python's SymbolNUMBER auto-typing logic */
AstNode *ast_number(CompilerState *cs, double value, int lineno) {
    AstNode *n = ast_new(cs, AST_NUMBER, lineno);
    n->u.number.value = value;

    SymbolTable *st = cs->symbol_table;
    if (value == (int64_t)value) {
        int64_t iv = (int64_t)value;
        if (iv >= 0 && iv <= 255)
            n->type_ = st->basic_types[TYPE_ubyte];
        else if (iv >= -128 && iv <= 127)
            n->type_ = st->basic_types[TYPE_byte];
        else if (iv >= 0 && iv <= 65535)
            n->type_ = st->basic_types[TYPE_uinteger];
        else if (iv >= -32768 && iv <= 32767)
            n->type_ = st->basic_types[TYPE_integer];
        else if (iv >= 0 && iv <= 4294967295LL)
            n->type_ = st->basic_types[TYPE_ulong];
        else if (iv >= -2147483648LL && iv <= 2147483647LL)
            n->type_ = st->basic_types[TYPE_long];
        else
            n->type_ = st->basic_types[TYPE_float];
    } else {
        n->type_ = st->basic_types[TYPE_float];
    }

    /* Set t to string representation of value */
    char buf[64];
    if (value == (int64_t)value)
        snprintf(buf, sizeof(buf), "%lld", (long long)(int64_t)value);
    else
        snprintf(buf, sizeof(buf), "%g", value);
    n->t = arena_strdup(&cs->arena, buf);

    return n;
}

/* ----------------------------------------------------------------
 * Type system
 * ---------------------------------------------------------------- */

TypeInfo *type_new(CompilerState *cs, const char *name, int lineno) {
    TypeInfo *t = arena_calloc(&cs->arena, 1, sizeof(TypeInfo));
    t->tag = AST_TYPE;
    t->name = arena_strdup(&cs->arena, name);
    t->lineno = lineno;
    t->class_ = CLASS_type;
    t->basic_type = TYPE_unknown;
    t->final_type = t;  /* self-referential by default */
    t->accessed = false;
    return t;
}

TypeInfo *type_new_basic(CompilerState *cs, BasicType bt) {
    TypeInfo *t = arena_calloc(&cs->arena, 1, sizeof(TypeInfo));
    t->tag = AST_BASICTYPE;
    t->name = arena_strdup(&cs->arena, basictype_to_string(bt));
    t->lineno = 0;  /* builtins defined at line 0 */
    t->class_ = CLASS_type;
    t->basic_type = bt;
    t->final_type = t;
    t->size = basictype_size(bt);
    return t;
}

TypeInfo *type_new_alias(CompilerState *cs, const char *name, int lineno, TypeInfo *alias) {
    TypeInfo *t = arena_calloc(&cs->arena, 1, sizeof(TypeInfo));
    t->tag = AST_TYPEALIAS;
    t->name = arena_strdup(&cs->arena, name);
    t->lineno = lineno;
    t->class_ = CLASS_type;
    t->basic_type = TYPE_unknown;
    t->final_type = alias->final_type;  /* resolve through chain */
    return t;
}

TypeInfo *type_new_ref(CompilerState *cs, TypeInfo *type, int lineno, bool implicit) {
    TypeInfo *t = arena_calloc(&cs->arena, 1, sizeof(TypeInfo));
    t->tag = AST_TYPEREF;
    t->name = arena_strdup(&cs->arena, type->name);
    t->lineno = lineno;
    t->class_ = CLASS_type;
    t->basic_type = TYPE_unknown;
    t->final_type = type->final_type;
    t->implicit = implicit;
    return t;
}

bool type_is_basic(const TypeInfo *t) {
    if (!t) return false;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    return f->tag == AST_BASICTYPE;
}

bool type_is_signed(const TypeInfo *t) {
    if (!t) return false;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    if (f->tag == AST_BASICTYPE)
        return basictype_is_signed(f->basic_type);
    return false;
}

bool type_is_numeric(const TypeInfo *t) {
    if (!t) return false;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    if (f->tag == AST_BASICTYPE)
        return basictype_is_numeric(f->basic_type);
    return false;
}

bool type_is_string(const TypeInfo *t) {
    if (!t) return false;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    return f->tag == AST_BASICTYPE && f->basic_type == TYPE_string;
}

bool type_is_dynamic(const TypeInfo *t) {
    if (!t) return false;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    if (f->tag == AST_BASICTYPE)
        return f->basic_type == TYPE_string;
    /* Compound type: check children */
    for (int i = 0; i < f->child_count; i++) {
        if (type_is_dynamic(f->children[i]))
            return true;
    }
    return false;
}

int type_size(const TypeInfo *t) {
    if (!t) return 0;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    if (f->tag == AST_BASICTYPE)
        return basictype_size(f->basic_type);
    /* Compound type: sum of children sizes */
    int total = 0;
    for (int i = 0; i < f->child_count; i++) {
        total += type_size(f->children[i]);
    }
    return total;
}

bool type_equal(const TypeInfo *a, const TypeInfo *b) {
    if (!a || !b) return false;

    /* Resolve aliases */
    a = a->final_type ? a->final_type : a;
    b = b->final_type ? b->final_type : b;

    if (a == b) return true;

    /* Both basic: compare basic_type */
    if (a->tag == AST_BASICTYPE && b->tag == AST_BASICTYPE)
        return a->basic_type == b->basic_type;

    /* One basic, one compound with 1 child */
    if (a->tag == AST_BASICTYPE && b->child_count == 1)
        return type_equal(a, b->children[0]);
    if (b->tag == AST_BASICTYPE && a->child_count == 1)
        return type_equal(a->children[0], b);

    /* Both compound: compare children */
    if (a->child_count != b->child_count)
        return false;
    for (int i = 0; i < a->child_count; i++) {
        if (!type_equal(a->children[i], b->children[i]))
            return false;
    }
    return true;
}
