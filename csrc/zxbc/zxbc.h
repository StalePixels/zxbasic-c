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
            char *mangled;       /* Python ID.mangled = MANGLE_CHR + name ("_a");
                                  * S5.3: used by VarTranslator for the data
                                  * label. Equals .t for globals. NULL until
                                  * symboltable_declare* sets it. */
            /* Python ID.filename (_id.py:58) — gl.FILENAME captured at
             * FIRST USE (the lineno/filename pair stamped on creation,
             * never overwritten). Used by api/check.py:179 for the
             * R11 "sub/function declared but not implemented" diagnostic
             * (fname=entry.filename), so the error attributes to the
             * #line-active filename when the SUB was DECLARED — not the
             * filename active at error-emit time. NULL == not stamped
             * (legacy callers pre-S5.x); zxbc_error falls back to
             * cs->current_file in that case. */
            char *filename;
            SymbolClass class_;
            Scope scope;
            Convention convention;
            bool byref;
            bool accessed;
            bool forwarded;      /* forward-declared function */
            bool declared;       /* has been defined (not just referenced) */
            bool default_value;  /* parameter has default value */
            /* Python ID.caseins (_id.py:37,63 / type_.py:27).
             * Stamped at declare time from OPTIONS.case_insensitive
             * (symboltable.py:115). When TRUE, this entry is ALSO
             * indexed in its scope's caseins HashMap keyed by
             * lower(name) so lookup_lower can resolve a mixed-case
             * reference to a mixed-case-declared symbol when the
             * pragma was in force. Default FALSE preserves the
             * default case-sensitive behaviour bit-for-bit. */
            bool caseins;
            /* S5.3: the initializer expr for `DIM v AS t = expr` (Python
             * VarRef.default_value). NULL == no initializer. Faithful to
             * zxbparser.py:711 declare_variable(default_value=defval). */
            AstNode *default_value_expr;
            /* S5.3: the AT-address expr for `DIM v AS t AT expr` (Python
             * entry.addr, a typecast Symbol; zxbparser.py:679). NULL == no
             * AT clause. ic_deflabel uses this. */
            AstNode *addr_expr;
            int64_t addr;        /* for absolute address variables */
            bool addr_set;
            /* Function-specific fields */
            AstNode *body;       /* function body (for FUNCDECL) */
            /* S5.10a — the callee's PARAMLIST, the faithful analogue of
             * Python entry.ref.params (id_/ref/funcref.py:50). Stamped
             * on the shared function ID node when its FUNCDECL is built
             * (the DECLARE forward node and the real definition both set
             * it; the definition's value wins, mirroring Python where
             * the definition's PARAMLIST replaces the declare's on the
             * same funcref). check_call_arguments reads this rather than
             * chasing id->parent, which the post-parse call-node
             * ast_add_child re-parents away from the FUNCDECL for any
             * call that is textually AFTER the definition. NULL until a
             * FUNCDECL is built for this entry. */
            AstNode *params;     /* AST_PARAMLIST (Python entry.ref.params) */
            int local_size;      /* bytes for local vars */
            int param_size;      /* bytes for parameters */

            /* S5.7d — inside-function stack-frame offset model.
             *
             * `offset` is the SymbolRef.offset (symbolref.py:28 / varref.py
             * :22 / arrayref.py:27) — the +/- IX-relative stack slot of a
             * parameter or local. For a PARAMETER it is the cumulative
             * PARAMLIST offset (parser_assign_param_offsets, copied onto
             * the body-scope symbol at scope entry). For a LOCAL var/array
             * it is written by symboltable_compute_offsets at scope-exit
             * (port of symboltable.py:235-266). `offset_set` distinguishes
             * a real 0 offset from "never assigned". */
            int offset;
            bool offset_set;
            /* The local-array geometry the FunctionTranslator :58-116 walk
             * and arrayref.py size/memsize need. Populated for a CLASS_array
             * local at its DIM site (parser) so the translator can run the
             * :68-100 array-init branch after the parse-time scope pop.
             * NULL boundlist == not an array / no bounds. */
            AstNode *arr_boundlist;   /* AST_BOUNDLIST (the DIM bounds) */
            AstNode *arr_init;        /* AST_ARRAYINIT / expr init (or NULL) */
            bool is_zero_based;       /* arrayref.py:89-91 (all lower==0) */
            bool lbound_used;         /* arrayref.py:23 (LBOUND ref'd) */
            bool ubound_used;         /* arrayref.py:24 (UBOUND ref'd) */
            bool is_dynamically_accessed; /* arrayref.py:29 */

            /* FUNCDECL-only: the function body's local Scope insertion-
             * ordered entry list, captured BEFORE the parse-time scope pop
             * (Python func.ref.local_symbol_table, zxbparser.py:2910). The
             * FunctionTranslator :58-116 walk and compute_offsets iterate
             * this. NULL until parse_sub_or_func_decl populates it. */
            struct AstNode **local_entries;
            int local_entries_count;

            /* p_addr_of_id (zxbparser.py:2682): `@id` sets
             * entry.has_address = True. Used by traverse_const's final
             * ID/VARARRAY branch (translator_visitor.py:244-245). */
            bool has_address;
            /* LabelRef.scope_owner (labelref.py:22,38-45): the list of
             * nested functions textually containing this label, captured
             * at access_label time as list(gl.FUNCTION_LEVEL)
             * (symboltable.py:622-623). When the label entry is marked
             * accessed, every scope_owner function entry is marked
             * accessed too (labelref.py:51-55) — this is what keeps a
             * SUB/FUNCTION whose only "use" is a `@label` from being
             * O>1-pruned. NULL/0 until a label def/access captures it. */
            struct AstNode **scope_owner;
            int scope_owner_count;
        } id;

        /* AST_BUILTIN */
        struct {
            char *fname;      /* builtin function name (e.g. "ABS", "LEN") */
            char *func;       /* backend function name */
            /* SymbolBUILTIN.discard_result (builtin.py:25). Set True by
             * the optimizer's visit_LET side-effect extraction for
             * IN/RND/USR whose result is unused (optimize.py:335);
             * visit_BUILTIN then ic_fparam's the discarded result
             * (translator.py:154-155). */
            bool discard_result;
        } builtin;

        /* AST_FUNCDECL: child[0]=ID, child[1]=PARAMLIST, child[2]=body.
         * S5.7b — `is_forward` marks the C-port-only FUNCDECL the parser
         * synthesises for a `DECLARE` (parser.c is_declare branch). Python
         * has no such node (p_funcdeclforward returns None,
         * zxbparser.py:2918-2930) — only the real definition's FUNCDECL
         * reaches gl.FUNCTIONS. The deferred-function enqueue
         * (tr_visit_funcdecl) skips is_forward nodes so the C queue holds
         * exactly the same set Python's gl.FUNCTIONS does. Not serialised
         * by zxbc-ast-dump (parse-isolated). FUNCDECL nodes never use any
         * other `u.*` member, so this dedicated struct cannot alias. */
        struct {
            bool is_forward;
        } funcdecl;

        /* AST_CALL/FUNCCALL: child[0] = callee ID, child[1] = ARGLIST.
         * S5.10a — `filename` is the call-site source filename captured
         * at parse time, the faithful analogue of Python
         * SymbolCALL.filename (symbols/call.py:42, set from the
         * make_node `filename` arg which is gl.FILENAME at the call's
         * parse point). check_pending_calls / check_call_arguments
         * (api/check.py:91-196) pass it as `fname=` to the R3-R10
         * argument-error messages so a #line-directive'd call reports
         * the directive's filename (bad_fname_err*), not the post-parse
         * cs->current_file. NULL == use cs->current_file. CALL/FUNCCALL
         * tag nodes never use any other `u.*` member (children-only;
         * tr_visit_call_common reads no `u.*`), so this dedicated
         * struct cannot alias. */
        struct {
            char *filename;
            /* S5.10a — Python SymbolCALL.make_node (symbols/call.py
             * :102-110) dispatches the argument check INLINE at the
             * call site when `entry.declared and not entry.forwarded`
             * (the callee is already a fully-parsed definition);
             * otherwise the call is appended to gl.FUNCTION_CALLS and
             * checked LATER by check_pending_calls. The C deferred loop
             * (check_pending_calls) is the faithful analogue of ONLY
             * that gl.FUNCTION_CALLS path, so the ported R3-R10
             * argument checks must run there only for calls Python
             * would also have deferred — i.e. callee NOT a finished
             * definition at call-parse time. `callee_inline` is true
             * when the resolved callee already had a real PARAMLIST and
             * was not forwarded when this call node was built (Python's
             * inline branch); R3-R10 skip such calls (the C has no
             * inline path — those are out of the deferred loop's
             * faithful scope), which keeps FALSE_POS at 0 for the
             * pre-existing parenless-call parser-shape divergence
             * (funcnoparm). The pre-existing R2/R11 firing is
             * unaffected by this flag. */
            bool callee_inline;
            /* The callee's ORIGINAL name as written at the call site,
             * preserving the deprecated sigil ($, %, etc.) — the faithful
             * analogue of Python ID.original_name (symbols/id_/_id.py:57),
             * which check_pending_calls passes to check_call_arguments
             * (api/check.py:194). The resolved callee ID node stores the
             * sigil-STRIPPED name; the undeclared-function diagnostic must
             * print the sigil-bearing original (e.g. `f$`, not `f`).
             * NULL == fall back to the callee ID's stripped name. */
            char *original_name;
        } call;
        /* AST_VARDECL: child[0] = ID, child[1] = initializer (or NULL) */
        /* AST_ARRAYDECL: child[0] = ID, child[1] = BOUNDLIST */
        /* AST_ARRAYACCESS/ARRAYLOAD: child[0] = array ID, child[1..n] = indices.
         * `offset` is the faithful analogue of SymbolARRAYACCESS.offset
         * (symbols/arrayaccess.py:68-91): the constant byte offset from the
         * start of the array DATA region when EVERY subscript is a compile-
         * time constant, else "not constant" (Python returns None). Computed
         * once at ARRAYACCESS-node construction in the parser (the C analogue
         * of the cached_property + make_call's `arr.offset is not None ->
         * append the folded NUMBER child`, zxbparser.py:388-392). `is_const`
         * distinguishes a real 0-byte offset from "dynamic / not foldable"
         * (Python's `offset is None`). ARRAYACCESS/ARRAYLOAD tag nodes are
         * children-only otherwise (no other `u.*` member is read by their
         * visitors), so this dedicated struct cannot alias. */
        struct {
            long offset;       /* byte offset (valid iff is_const) */
            bool is_const;     /* all subscripts constant (offset valid) */
            bool is_load;      /* read context => Python sym.ARRAYLOAD
                                * (visit_ARRAYLOAD: aload). false => the
                                * LETARRAY lvalue, Python sym.ARRAYACCESS
                                * (visit_ARRAYACCESS: just push indices).
                                * Set from expr_context at construction. */
        } arrayaccess;

        /* AST_ARGUMENT */
        struct {
            char *name;       /* parameter name */
            bool byref;
            bool is_array;
            /* For a PARAMLIST array ARGUMENT only: snapshot of the body-
             * scope param symbol's ref.is_dynamically_accessed, copied at
             * function-def completion (before the body scope is popped).
             * Python's param symbol IS the PARAMLIST child, so call.py:49-55
             * reads `param.ref.is_dynamically_accessed` directly off the
             * body-set flag; the C separates the ARGUMENT wrapper from the
             * body-scope ID, so the flag is mirrored here for the CALL-site
             * array-arg propagation (var_translator.py:58 __LBOUND__). */
            bool is_dynamically_accessed;
            /* S5.7b — cumulative parameter offset, assigned left-to-right
             * by parser_assign_param_offsets (paramlist.py:53-58
             * PARAMLIST.append_child: param.ref.offset = self.size;
             * self.size += param.size). Only meaningful on a PARAMLIST's
             * ARGUMENT children. -1 == not assigned. */
            int offset;
        } argument;

        /* AST_BOUND: child[0] = lower, child[1] = upper */

        /* AST_SENTENCE */
        struct {
            char *kind;       /* "LET", "IF", "FOR", "PRINT", etc. */
            bool sentinel;    /* sentinel marker for program end, etc. */
            /* PRINT trailing-newline flag, mirrors Python node.eol
             * (zxbparser.py:2039,2045,2054). Default false (zero-init for
             * every sentence); set true/false explicitly only by the PRINT
             * production in parse_print_statement. No other sentence reads
             * it. node.eol True -> emit a trailing PRINT_EOL. */
            bool eol;
        } sentence;

        /* AST_ASM */
        struct {
            char *code;       /* inline assembly text */
            char *filename;   /* node.filename (zxbparser:255, gl.FILENAME at
                               * construction). Used by visit_ASM to emit
                               * the surrounding `#line N "filename"`
                               * directives — must reflect the file that
                               * actually contained the ASM block, including
                               * #include'd files (tap_include_asm_error). */
        } asm_block;

        /* AST_CONSTEXPR: child[0] = the constant expression */
        /* AST_STRSLICE: child[0] = string, child[1] = lower, child[2] = upper */
        /* AST_BLOCK: children are the statements */
    } u;
};

/* ----------------------------------------------------------------
 * S5.8d — DataRef (src/api/dataref.py:14-24).
 *
 * Python's gl.DATAS is a list[DataRef(label, datas)] — NOT a list of
 * raw DATA sentence nodes. Each DataRef pairs the per-DATA-line label
 * SYMBOL (the make_label entry, id == gl.DATA_PTR_CURRENT at that
 * point) with the ordered list of that line's items. Each item is
 * EITHER a static value expr (kept verbatim — emit_data_blocks reads
 * d.value.type_ etc.) OR a synthesised __DATA__FUNCPTR__N FUNCDECL
 * node (the non-static thunk). `is_funcdecl` discriminates; the
 * faithful analogue of emit_data_blocks:133 `isinstance(d,
 * symbols.FUNCDECL)`. `label_name` is DataRef.label.name (the
 * .DATA.__DATA__N string) used both by ic_label and by the
 * emit_data_blocks:149 `[x.label.name for x in gl.DATAS]` set. */
typedef struct DataItem {
    bool is_funcdecl;     /* non-static -> synthesised FUNCPTR FUNCDECL */
    AstNode *node;        /* static: the value expr;  funcptr: the FUNCDECL */
} DataItem;

typedef struct DataRef {
    char *label_name;          /* DataRef.label.name (".DATA.__DATA__N") */
    AstNode *label_entry;      /* the make_label symbol-table entry */
    DataItem *items;           /* arena-allocated, ordered */
    int item_count;
} DataRef;

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
    /* Python Scope.caseins (scope.py:44). Mirror of `symbols` keyed
     * by name.lower(), populated ONLY when an entry's .caseins flag
     * is true at insertion time (scope.py:56-57). Lookup falls back
     * to this map when the exact-case lookup misses (scope.py:51).
     * Empty by default — pure passthrough under the default
     * case-sensitive option. */
    HashMap caseins;
    struct Scope_ *parent;
    int level;               /* 0 = global */
    /* Python Scope.namespace (scope.py:45) — the mangling prefix. The
     * global scope's is "" (symboltable.py:54). enter_scope sets a child
     * scope's to make_child_namespace(parent_namespace, fn_name)
     * (symboltable.py:228); declare() mangles every entry as
     * make_child_namespace(current_namespace, name) (symboltable.py:122).
     * A nested SUB/FUNCTION thus mangles "_procedure.subprocedure" not
     * "_subprocedure" — the opt2_labelinfunc4/paramstr5 divergence. */
    const char *namespace_;
    /* S5.7d — per-scope insertion-ordered entry list, mirroring Python's
     * Scope.symbols OrderedDict (scope.py:43-44). The HashMap above is
     * unordered; compute_offsets needs the OrderedDict iteration order
     * BEFORE its stable entry_size sort (symboltable.py:250). Every
     * symboltable_declare* pushes here at the same single insert point it
     * pushes cs->sym_entries_ordered (symboltable.py:116). Arena-backed
     * growable; never freed (arena owns it). */
    struct AstNode **ordered;
    int ordered_count;
    int ordered_cap;
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

/* Scope management. `namespace_name` is the Python enter_scope(namespace)
 * arg (symboltable.py:228) — the function name whose body scope is being
 * entered; NULL/"" is tolerated (treated as ""). */
void symboltable_enter_scope(SymbolTable *st, CompilerState *cs,
                             const char *namespace_name);
void symboltable_exit_scope(SymbolTable *st);
/* Python SymbolTable.make_child_namespace (symboltable.py:140-149):
 * empty parent -> MANGLE_CHR + child ("_x"); else parent +
 * NAMESPACE_SEPARATOR + child ("p.x"). MANGLE_CHR="_", SEP="."
 * (global_.py:167-168). */
char *make_child_namespace(CompilerState *cs, const char *parent,
                           const char *child);
/* S5.7d — compute_offsets (symboltable.py:235-266): assigns per-local
 * +/- IX offsets over the scope's insertion-ordered entries (stable
 * entry_size sort) and returns the total local frame size (locals_size,
 * the ic_enter operand). Called at function scope-exit before the pop. */
/* opt_level mirrors OPTIONS.optimization_level so compute_offsets can
 * apply Python Scope.values(filter_by_opt=True) — at O>1, un-accessed
 * locals are dropped from the offset list (symboltable.py:283 /
 * scope.py:63-66). Pass cs->opts.optimization_level. */
int symboltable_compute_offsets(SymbolTable *st, Scope_ *scope, int opt_level);

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
bool check_is_dynamic(const AstNode *entry);  /* check.is_dynamic single-entry */
void mark_label_accessed(AstNode *label);     /* LabelRef.accessed cascade */
void label_capture_scope_owner(CompilerState *cs, AstNode *label); /* access_label */
bool check_is_numeric(const AstNode *a, const AstNode *b); /* both have numeric type */
bool check_is_string_node(const AstNode *a, const AstNode *b); /* both are STRING constants */
const AstNode *const_string_value_node(const AstNode *n); /* ConstRef.value -> AST_STRING */
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
AstNode *symboltable_access_id_noexplicit(SymbolTable *st, CompilerState *cs,
                                          const char *name, int lineno,
                                          TypeInfo *default_type,
                                          SymbolClass default_class);
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

    /* S5.3 (N1): every ID symbol-table entry, in first-textual-appearance
     * (insertion) order. The C scope HashMap is unordered, so this mirrors
     * Python's OrderedDict global scope (scope.py:43-44) and the single
     * insert point symboltable.py:116 (symboltable_declare). data_ast is
     * built by filtering this to global CLASS_var entries — the faithful
     * analogue of SYMBOL_TABLE.vars_ (symboltable.py:776-782, filter_by_opt
     * = False → ALL declared vars regardless of O-level). */
    VEC(AstNode *) sym_entries_ordered;

    /* Parser state */
    bool labels_allowed;     /* for line-start label detection */
    bool let_assignment;     /* inside a LET statement */
    bool print_is_used;      /* PRINT has been referenced */
    /* function_translator.py:79-80 — set when a local bounded array's
     * bound_ptrs != ["0","0"]; mirrors OPTIONS.__DEFINES
     * ["__ZXB_USE_LOCAL_ARRAY_WITH_BOUNDS__"]="" so the ASM filter pass
     * (set_option_defines+reset_id_table, zxbc.py:183-187) #includes the
     * _WITH_BOUNDS runtime variants gated in array/arrayalloc.asm:75. */
    bool local_array_with_bounds_used;
    int last_brk_linenum;    /* last line for BREAK check */

    /* Function tracking */
    VEC(AstNode *) function_level;   /* scope stack of function IDs */
    VEC(AstNode *) function_calls;   /* pending function call checks */
    VEC(AstNode *) functions;        /* all declared functions */
    VEC(LoopInfo) loop_stack;        /* nested loop tracking */

    /* Labels */
    HashMap labels;          /* user-defined labels */

    /* DATA statement tracking — the Python gl.DATA* model
     * (src/api/global_.py:183-188). S5.8d makes these LIVE:
     *  - datas              = gl.DATAS (list[DataRef], one per DATA line)
     *  - data_labels        = gl.DATA_LABELS (declared-label -> the data
     *                         ptr current when that label was declared;
     *                         a str->str map; make_label:457)
     *  - data_labels_required = gl.DATA_LABELS_REQUIRED (the set of
     *                         labels a RESTORE asked for; visit_RESTORE
     *                         :494; emit_data_blocks:149)
     *  - data_functions     = gl.DATA_FUNCTIONS (the synthesised
     *                         __DATA__FUNCPTR__N FUNCDECL nodes,
     *                         p_data:1764; merged into the pending-fn
     *                         queue by codegen before FunctionTranslator)
     *  - data_ptr_current   = gl.DATA_PTR_CURRENT (the current
     *                         current_data_label() string; reset value
     *                         is ".DATA.__DATA__0", advanced per DATA) */
    VEC(DataRef *) datas;
    HashMap data_labels;
    HashMap data_labels_required;
    VEC(AstNode *) data_functions;
    char *data_ptr_current;
    bool data_is_used;

    /* Init routines */
    VEC(char *) inits;       /* #init labels */

    /* #require'd asm modules collected at parse time — the faithful
     * analogue of Python's parse-time `arch.target.backend.REQUIRES.add`
     * (zxbparser.py:3234 p_preproc_line_require). Python's
     * `common.REQUIRES` is a module global cleared by backend.init()
     * BEFORE the parse (zxbc.py:93) and populated DURING the parse; the C
     * Backend is constructed after the parse, so the parse-time names are
     * staged here and seeded into b->requires_ in codegen_emit right after
     * backend_init (matching the Python clear→parse→translate→emit order).*/
    VEC(char *) requires;    /* parse-time #require module names */

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

/* S5.8d — current_data_label() (src/api/utils.py:110-114):
 *   f"{global_.DATAS_NAMESPACE}.__DATA__{len(global_.DATAS)}"
 * DATAS_NAMESPACE == ".DATA" (global_.py:134). Arena-owned result;
 * the index is cs->datas.len AT THE TIME OF THE CALL. */
char *current_data_label(CompilerState *cs);

/* (make_label's DATA_LABELS write is applied inline at its three
 * parser sites — see compiler.c near symboltable_access_label and the
 * parser.c BTOK_DATA / label-declaration paths. No standalone helper.) */

/* Post-parse validation (from p_start in zxbparser.py, check.py) */
bool check_pending_labels(CompilerState *cs, AstNode *ast);
bool check_pending_calls(CompilerState *cs);
/* check_call_arguments — faithful port of api/check.py:91-183. Python
 * runs it INLINE at the call site (symbols/call.py:103) when the callee
 * is a finished definition (entry.declared and not entry.forwarded),
 * and via check_pending_calls for deferred (forward-declared) calls.
 * Exposed so the parser can invoke it at parse time for the inline
 * branch — matching Python's parse-time firing order relative to the
 * later-emitted implicit-type [W100] warnings. */
bool check_call_arguments(CompilerState *cs, AstNode *call,
                          AstNode *entry, const char *id_);
void symboltable_check_classes(SymbolTable *st, CompilerState *cs);

#endif /* ZXBC_H */
