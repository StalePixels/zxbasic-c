/*
 * compiler.c — Compiler state initialization and management
 *
 * Ported from src/api/global_.py and src/zxbc/zxbparser.py init()
 */
#include "zxbc.h"
#include "errmsg.h"
#include <math.h>
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

    /* Same type — no cast needed */
    if (type_equal(new_type, node->type_))
        return node;

    /* Target is unknown — skip the cast (type not yet resolved) */
    if (new_type->basic_type == TYPE_unknown ||
        (new_type->final_type && new_type->final_type->basic_type == TYPE_unknown))
        return node;

    /* Source type not yet resolved — skip */
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

    /* If it's a CONSTEXPR, cast the inner expression */
    if (check_is_CONST(node)) {
        if (node->child_count > 0) {
            node->children[0] = make_typecast(cs, new_type, node->children[0], lineno);
        }
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
            /* Integer: mask to target size */
            int sz = basictype_size(nf->basic_type);
            if (node->tag == AST_NUMBER && sz > 0) {
                int64_t ival = (int64_t)node->u.number.value;
                int64_t mask = ((int64_t)1 << (8 * sz)) - 1;
                int64_t new_val = ival & mask;

                if (ival >= 0 && ival != new_val) {
                    warn_conversion_lose_digits(cs, lineno);
                    node->u.number.value = (double)new_val;
                } else if (ival < 0 && ((1LL << (sz * 8)) + ival) != new_val) {
                    warn_conversion_lose_digits(cs, lineno);
                    node->u.number.value = (double)(new_val - (1LL << (sz * 8)));
                }
            }
        }
    }

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
            /* Both are compile-time numbers — fold */
            AstNode *cl = make_typecast(cs, c_type, left, lineno);
            AstNode *cr = make_typecast(cs, c_type, right, lineno);
            if (cl && cr && cl->tag == AST_NUMBER && cr->tag == AST_NUMBER) {
                double result;
                if (fold_numeric(operator, cl->u.number.value, cr->u.number.value, &result)) {
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

    /* String constant folding (concatenation) */
    if (check_is_string_node(left, right) && strcmp(operator, "PLUS") == 0) {
        if (left->tag == AST_STRING && right->tag == AST_STRING) {
            size_t len = strlen(left->u.string.value) + strlen(right->u.string.value);
            char *concat = arena_alloc(&cs->arena, len + 1);
            strcpy(concat, left->u.string.value);
            strcat(concat, right->u.string.value);
            AstNode *s = ast_new(cs, AST_STRING, lineno);
            s->u.string.value = concat;
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

    /* Constant folding for MINUS */
    if (strcmp(operator, "MINUS") == 0 && operand->tag == AST_NUMBER) {
        operand->u.number.value = -operand->u.number.value;
        return operand;
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

    AstNode *n = ast_new(cs, AST_UNARY, lineno);
    n->u.unary.operator = arena_strdup(&cs->arena, operator);
    ast_add_child(cs, n, operand);
    n->type_ = operand->type_;
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

    /* Check --explicit mode */
    if (cs->opts.explicit_ && !default_type) {
        if (!symboltable_check_is_declared(st, lookup_name, lineno, "identifier", true, cs))
            return NULL;
    }

    AstNode *result = symboltable_lookup(st, lookup_name);
    if (!result) {
        /* Implicit declaration */
        if (suffix_type) {
            default_type = suffix_type;
        } else if (!default_type) {
            default_type = type_new_ref(cs, cs->default_type, lineno, true);
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
        err_unexpected_class(cs, lineno, name,
                             symbolclass_to_string(result->u.id.class_),
                             symbolclass_to_string(CLASS_var));
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
        err_unexpected_class(cs, lineno, name,
                             symbolclass_to_string(result->u.id.class_),
                             symbolclass_to_string(CLASS_function));
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

    /* Check if callable: function/sub/array/string */
    SymbolClass cls = entry->u.id.class_;
    if (cls == CLASS_function || cls == CLASS_sub || cls == CLASS_array ||
        cls == CLASS_unknown) {
        return entry;
    }

    /* Variables/constants are callable if they're strings (string slicing) */
    if ((cls == CLASS_var || cls == CLASS_const) && type_is_string(entry->type_)) {
        return entry;
    }

    err_not_array_nor_func(cs, lineno, name);
    return NULL;
}

AstNode *symboltable_access_array(SymbolTable *st, CompilerState *cs,
                                   const char *name, int lineno, TypeInfo *default_type) {
    if (!symboltable_check_is_declared(st, name, lineno, "array", true, cs))
        return NULL;

    AstNode *result = symboltable_lookup(st, name);
    if (!result) return NULL;

    if (result->u.id.class_ != CLASS_array && result->u.id.class_ != CLASS_unknown) {
        err_unexpected_class(cs, lineno, name,
                             symbolclass_to_string(result->u.id.class_),
                             symbolclass_to_string(CLASS_array));
        return NULL;
    }

    return result;
}

AstNode *symboltable_access_label(SymbolTable *st, CompilerState *cs,
                                   const char *name, int lineno) {
    AstNode *result = symboltable_lookup(st, name);
    if (!result) {
        result = symboltable_declare(st, cs, name, lineno, CLASS_label);
        result->u.id.declared = false;
        return result;
    }

    if (result->u.id.class_ != CLASS_label && result->u.id.class_ != CLASS_unknown) {
        err_unexpected_class(cs, lineno, name,
                             symbolclass_to_string(result->u.id.class_),
                             symbolclass_to_string(CLASS_label));
        return NULL;
    }

    if (result->u.id.class_ == CLASS_unknown)
        result->u.id.class_ = CLASS_label;

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
bool check_pending_labels(CompilerState *cs, AstNode *ast) {
    if (!ast) return true;

    bool result = true;

    /* Iterative traversal to avoid stack overflow on deeply nested ASTs */
    int stack_cap = 256;
    int stack_len = 0;
    AstNode **stack = arena_alloc(&cs->arena, stack_cap * sizeof(AstNode *));
    stack[stack_len++] = ast;

    while (stack_len > 0) {
        AstNode *node = stack[--stack_len];
        if (!node) continue;

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

        /* Only check raw ID nodes (not already classified as var/func/array etc.) */
        if (node->tag != AST_ID) continue;
        /* Skip nodes already resolved to a concrete class */
        if (node->u.id.class_ != CLASS_unknown && node->u.id.class_ != CLASS_label) continue;

        /* Look up in symbol table (matching Python's SYMBOL_TABLE.get_entry) */
        AstNode *entry = symboltable_lookup(cs->symbol_table, node->u.id.name);
        if (!entry || entry->u.id.class_ == CLASS_unknown) {
            zxbc_error(cs, node->lineno, "Undeclared identifier \"%s\"", node->u.id.name);
            result = false;
        }
    }

    return result;
}

/* check_pending_calls: validate forward-referenced function calls.
 * Matches Python's src/api/check.py check_pending_calls(). */
bool check_pending_calls(CompilerState *cs) {
    bool result = true;

    for (int i = 0; i < cs->function_calls.len; i++) {
        AstNode *call = cs->function_calls.data[i];
        if (!call) continue;

        /* The call node's first child is the callee ID */
        if (call->child_count < 1) continue;
        AstNode *callee = call->children[0];
        if (!callee || callee->tag != AST_ID) continue;

        const char *name = callee->u.id.name;
        AstNode *entry = symboltable_lookup(cs->symbol_table, name);

        if (!entry) {
            zxbc_error(cs, call->lineno, "Undeclared function \"%s\"", name);
            result = false;
            continue;
        }

        /* Check if forward-declared but never implemented */
        if (entry->u.id.forwarded) {
            const char *kind = (entry->u.id.class_ == CLASS_sub) ? "sub" : "function";
            zxbc_error(cs, call->lineno, "%s '%s' declared but not implemented", kind, name);
            result = false;
        }
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
