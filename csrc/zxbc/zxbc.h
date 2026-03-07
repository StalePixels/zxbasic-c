/*
 * zxbc.h — ZX BASIC Compiler (C port)
 *
 * Main header file. Defines the compiler state and all core types.
 * Ported from src/zxbc/, src/symbols/, src/api/.
 */
#ifndef ZXBC_H
#define ZXBC_H

#include "arena.h"
#include "strbuf.h"
#include "vec.h"
#include "hashmap.h"
#include "types.h"
#include "options.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */
typedef struct AstNode AstNode;
typedef struct SymbolTable SymbolTable;
typedef struct CompilerState CompilerState;

/* ----------------------------------------------------------------
 * AST node tags (one per Symbol subclass in Python)
 *
 * From src/symbols/sym.py exports:
 *   ARGLIST, ARGUMENT, ARRAYACCESS, ARRAYDECL, ARRAYLOAD,
 *   ASM, BINARY, BLOCK, BOUND, BOUNDLIST, BUILTIN, CALL,
 *   CONSTEXPR, FUNCCALL, FUNCDECL, ID, NOP, NUMBER,
 *   PARAMLIST, SENTENCE, STRING, STRSLICE, SYMBOL,
 *   TYPE, TYPECAST, TYPEREF, UNARY, VARDECL
 * ---------------------------------------------------------------- */
typedef enum {
    AST_NOP = 0,
    AST_NUMBER,
    AST_STRING,
    AST_BINARY,
    AST_UNARY,
    AST_ID,
    AST_TYPECAST,
    AST_BUILTIN,
    AST_CALL,
    AST_FUNCCALL,
    AST_FUNCDECL,
    AST_VARDECL,
    AST_ARRAYDECL,
    AST_ARRAYACCESS,
    AST_ARRAYLOAD,
    AST_ARGUMENT,
    AST_ARGLIST,
    AST_PARAMLIST,
    AST_BLOCK,
    AST_SENTENCE,
    AST_BOUND,
    AST_BOUNDLIST,
    AST_ASM,
    AST_CONSTEXPR,
    AST_STRSLICE,
    AST_ARRAYINIT,
    AST_TYPE,
    AST_BASICTYPE,
    AST_TYPEALIAS,
    AST_TYPEREF,
    AST_COUNT,
} AstTag;

static inline const char *ast_tag_name(AstTag tag) {
    static const char *names[] = {
        "NOP", "NUMBER", "STRING", "BINARY", "UNARY", "ID",
        "TYPECAST", "BUILTIN", "CALL", "FUNCCALL", "FUNCDECL",
        "VARDECL", "ARRAYDECL", "ARRAYACCESS", "ARRAYLOAD",
        "ARGUMENT", "ARGLIST", "PARAMLIST", "BLOCK", "SENTENCE",
        "BOUND", "BOUNDLIST", "ASM", "CONSTEXPR", "STRSLICE",
        "ARRAYINIT", "TYPE", "BASICTYPE", "TYPEALIAS", "TYPEREF",
    };
    return (tag >= 0 && tag < AST_COUNT) ? names[tag] : "UNKNOWN";
}

/* ----------------------------------------------------------------
 * AST type node — represents a type in the type system
 *
 * Ported from src/symbols/type_.py (SymbolTYPE hierarchy).
 * This is separate from AstNode because types are registered in
 * the symbol table, not part of the expression AST per se.
 * ---------------------------------------------------------------- */
typedef struct TypeInfo {
    AstTag tag;              /* AST_TYPE, AST_BASICTYPE, AST_TYPEALIAS, AST_TYPEREF */
    char *name;              /* type name */
    int lineno;              /* line where defined (0 = builtin) */
    bool case_insensitive;
    bool accessed;           /* has this type been used */
    SymbolClass class_;      /* always CLASS_type */

    /* For AST_BASICTYPE: the primitive type enum */
    BasicType basic_type;    /* TYPE_unknown if not a basic type */

    /* For alias/ref: points to the final (non-alias) type */
    struct TypeInfo *final_type;  /* self if not an alias */

    /* For compound types: children (struct fields, etc.) */
    struct TypeInfo **children;
    int child_count;

    /* For AST_TYPEREF: whether implicitly inferred */
    bool implicit;

    /* Size cache (computed from basic_type or children) */
    int size;
} TypeInfo;

/* ----------------------------------------------------------------
 * AST node — tagged union for all expression/statement nodes
 *
 * Common header: tag, type_, lineno, parent, children.
 * ---------------------------------------------------------------- */
struct AstNode {
    AstTag tag;
    TypeInfo *type_;         /* result type of this node (NULL if untyped) */
    int lineno;
    AstNode *parent;

    /* Children list (growable) */
    AstNode **children;
    int child_count;
    int child_cap;

    /* Temporary variable name for IR generation (Phase 4) */
    char *t;

    /* Per-tag data */
    union {
        /* AST_NUMBER */
        struct {
            double value;
        } number;

        /* AST_STRING */
        struct {
            char *value;
            int length;
        } string;

        /* AST_BINARY */
        struct {
            char *operator;   /* "+", "-", "*", "/", "AND", "OR", etc. */
            char *func;       /* optional function name for operator overload */
        } binary;

        /* AST_UNARY */
        struct {
            char *operator;
            char *func;
        } unary;

        /* AST_ID */
        struct {
            char *name;
            SymbolClass class_;
            Scope scope;
            Convention convention;
            bool byref;
            bool accessed;
            bool forwarded;      /* forward-declared function */
            bool declared;       /* has been defined (not just referenced) */
            bool default_value;  /* parameter has default value */
            int64_t addr;        /* for absolute address variables */
            bool addr_set;
            /* Function-specific fields */
            AstNode *body;       /* function body (for FUNCDECL) */
            int local_size;      /* bytes for local vars */
            int param_size;      /* bytes for parameters */
        } id;

        /* AST_BUILTIN */
        struct {
            char *fname;      /* builtin function name (e.g. "ABS", "LEN") */
            char *func;       /* backend function name */
        } builtin;

        /* AST_CALL/FUNCCALL: child[0] = callee ID, child[1] = ARGLIST */
        /* AST_FUNCDECL: child[0] = ID, child[1] = PARAMLIST, child[2] = body */
        /* AST_VARDECL: child[0] = ID, child[1] = initializer (or NULL) */
        /* AST_ARRAYDECL: child[0] = ID, child[1] = BOUNDLIST */
        /* AST_ARRAYACCESS/ARRAYLOAD: child[0] = array ID, child[1..n] = indices */

        /* AST_ARGUMENT */
        struct {
            char *name;       /* parameter name */
            bool byref;
            bool is_array;
        } argument;

        /* AST_BOUND: child[0] = lower, child[1] = upper */

        /* AST_SENTENCE */
        struct {
            char *kind;       /* "LET", "IF", "FOR", "PRINT", etc. */
            bool sentinel;    /* sentinel marker for program end, etc. */
        } sentence;

        /* AST_ASM */
        struct {
            char *code;       /* inline assembly text */
        } asm_block;

        /* AST_CONSTEXPR: child[0] = the constant expression */
        /* AST_STRSLICE: child[0] = string, child[1] = lower, child[2] = upper */
        /* AST_BLOCK: children are the statements */
    } u;
};

/* ----------------------------------------------------------------
 * AST node operations
 * ---------------------------------------------------------------- */

/* Create a new AST node (arena-allocated) */
AstNode *ast_new(CompilerState *cs, AstTag tag, int lineno);

/* Append a child to a node */
void ast_add_child(CompilerState *cs, AstNode *parent, AstNode *child);

/* Create a NUMBER node with auto type inference from value */
AstNode *ast_number(CompilerState *cs, double value, int lineno);

/* ----------------------------------------------------------------
 * Type system operations
 * ---------------------------------------------------------------- */

/* Create type info nodes */
TypeInfo *type_new(CompilerState *cs, const char *name, int lineno);
TypeInfo *type_new_basic(CompilerState *cs, BasicType bt);
TypeInfo *type_new_alias(CompilerState *cs, const char *name, int lineno, TypeInfo *alias);
TypeInfo *type_new_ref(CompilerState *cs, TypeInfo *type, int lineno, bool implicit);

/* Type queries */
bool type_equal(const TypeInfo *a, const TypeInfo *b);
bool type_is_basic(const TypeInfo *t);
bool type_is_signed(const TypeInfo *t);
bool type_is_numeric(const TypeInfo *t);
bool type_is_string(const TypeInfo *t);
bool type_is_dynamic(const TypeInfo *t);
int type_size(const TypeInfo *t);

/* ----------------------------------------------------------------
 * Symbol table
 * ---------------------------------------------------------------- */
typedef struct Scope_ {
    HashMap symbols;         /* name -> AstNode* (ID nodes) */
    struct Scope_ *parent;
    int level;               /* 0 = global */
} Scope_;

struct SymbolTable {
    Arena *arena;
    Scope_ *current_scope;
    Scope_ *global_scope;

    /* Registered basic types (TYPE_unknown through TYPE_boolean) */
    TypeInfo *basic_types[TYPE_COUNT];

    /* All registered user types */
    HashMap type_registry;   /* name -> TypeInfo* */
};

/* Create/destroy symbol table */
SymbolTable *symboltable_new(CompilerState *cs);

/* Scope management */
void symboltable_enter_scope(SymbolTable *st, CompilerState *cs);
void symboltable_exit_scope(SymbolTable *st);

/* Symbol operations */
AstNode *symboltable_declare(SymbolTable *st, CompilerState *cs,
                              const char *name, int lineno, SymbolClass class_);
AstNode *symboltable_lookup(SymbolTable *st, const char *name);
AstNode *symboltable_get_entry(SymbolTable *st, const char *name);

/* Higher-level declaration functions (matching Python's SymbolTable API) */
AstNode *symboltable_declare_variable(SymbolTable *st, CompilerState *cs,
                                       const char *name, int lineno, TypeInfo *typeref);
AstNode *symboltable_declare_param(SymbolTable *st, CompilerState *cs,
                                    const char *name, int lineno, TypeInfo *typeref);
AstNode *symboltable_declare_array(SymbolTable *st, CompilerState *cs,
                                    const char *name, int lineno,
                                    TypeInfo *typeref, AstNode *bounds);

/* Check functions (matching Python's check_is_declared/check_is_undeclared) */
bool symboltable_check_is_declared(SymbolTable *st, const char *name, int lineno,
                                    const char *classname, bool show_error,
                                    CompilerState *cs);
bool symboltable_check_is_undeclared(SymbolTable *st, const char *name, int lineno,
                                      bool show_error, CompilerState *cs);

/* Type operations */
TypeInfo *symboltable_get_type(SymbolTable *st, const char *name);

/* Check module (matching Python's api/check.py) */
bool is_temporary_value(const AstNode *node);

/* AST node predicates — matching check.is_number(), is_const(), etc. */
bool check_is_number(const AstNode *node);    /* NUMBER or CONST with numeric type */
bool check_is_const(const AstNode *node);     /* CONST (declared constant) */
bool check_is_CONST(const AstNode *node);     /* CONSTEXPR (compile-time expression) */
bool check_is_static(const AstNode *node);    /* CONSTEXPR, NUMBER, or CONST */
bool check_is_numeric(const AstNode *a, const AstNode *b); /* both have numeric type */
bool check_is_string_node(const AstNode *a, const AstNode *b); /* both are STRING constants */
bool check_is_null(const AstNode *node);      /* NULL, NOP, or empty BLOCK */

/* Type promotion — matching check.common_type() */
TypeInfo *check_common_type(CompilerState *cs, const AstNode *a, const AstNode *b);

/* Semantic node creation — matching Python's SymbolTYPECAST/BINARY/UNARY.make_node() */
AstNode *make_typecast(CompilerState *cs, TypeInfo *new_type, AstNode *node, int lineno);
AstNode *make_binary_node(CompilerState *cs, const char *operator, AstNode *left,
                          AstNode *right, int lineno, TypeInfo *type_);
AstNode *make_unary_node(CompilerState *cs, const char *operator, AstNode *operand,
                         int lineno);

/* Symbol table access methods — matching Python's symboltable.access_*() */
AstNode *symboltable_access_id(SymbolTable *st, CompilerState *cs,
                                const char *name, int lineno,
                                TypeInfo *default_type, SymbolClass default_class);
AstNode *symboltable_access_var(SymbolTable *st, CompilerState *cs,
                                 const char *name, int lineno, TypeInfo *default_type);
AstNode *symboltable_access_call(SymbolTable *st, CompilerState *cs,
                                  const char *name, int lineno, TypeInfo *type_);
AstNode *symboltable_access_func(SymbolTable *st, CompilerState *cs,
                                  const char *name, int lineno, TypeInfo *default_type);
AstNode *symboltable_access_array(SymbolTable *st, CompilerState *cs,
                                   const char *name, int lineno, TypeInfo *default_type);
AstNode *symboltable_access_label(SymbolTable *st, CompilerState *cs,
                                   const char *name, int lineno);

/* ----------------------------------------------------------------
 * Compiler state — the main context struct
 *
 * Replaces Python's module-level globals (api/global_.py, zxbparser.py)
 * ---------------------------------------------------------------- */

/* Loop info for FOR/WHILE/DO tracking */
typedef struct LoopInfo {
    LoopType type;
    int lineno;
    char *var_name;          /* FOR variable name, or NULL */
} LoopInfo;

struct CompilerState {
    Arena arena;
    CompilerOptions opts;

    /* Error tracking */
    int error_count;
    int warning_count;
    HashMap error_cache;     /* dedup error messages */
    char *current_file;      /* current source filename */

    /* Symbol table */
    SymbolTable *symbol_table;

    /* AST roots */
    AstNode *ast;            /* main program AST */
    AstNode *data_ast;       /* global variable declarations */

    /* Parser state */
    bool labels_allowed;     /* for line-start label detection */
    bool let_assignment;     /* inside a LET statement */
    bool print_is_used;      /* PRINT has been referenced */
    int last_brk_linenum;    /* last line for BREAK check */

    /* Function tracking */
    VEC(AstNode *) function_level;   /* scope stack of function IDs */
    VEC(AstNode *) function_calls;   /* pending function call checks */
    VEC(AstNode *) functions;        /* all declared functions */
    VEC(LoopInfo) loop_stack;        /* nested loop tracking */

    /* Labels */
    HashMap labels;          /* user-defined labels */

    /* DATA statement tracking */
    VEC(AstNode *) datas;
    HashMap data_labels;
    HashMap data_labels_required;
    bool data_is_used;

    /* Init routines */
    VEC(char *) inits;       /* #init labels */

    /* Default type for undeclared variables */
    TypeInfo *default_type;

    /* Temp variable counter */
    int temp_counter;
};

/* Initialize/destroy compiler state */
void compiler_init(CompilerState *cs);
void compiler_destroy(CompilerState *cs);

/* Generate a new temporary variable name */
char *compiler_new_temp(CompilerState *cs);

/* Post-parse validation (from p_start in zxbparser.py, check.py) */
bool check_pending_labels(CompilerState *cs, AstNode *ast);
bool check_pending_calls(CompilerState *cs);
void symboltable_check_classes(SymbolTable *st, CompilerState *cs);

#endif /* ZXBC_H */
