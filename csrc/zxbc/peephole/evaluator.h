/*
 * evaluator.h — Port of src/arch/z80/peephole/evaluator.py
 *
 *   FN              : the StrEnum of operator/function names (verbatim).
 *   PNode           : the parser's TreeType (list[str | list]) — a string
 *                     atom or an ordered list of PNode. Produced by
 *                     parser.c, consumed by Evaluator.
 *   Number          : utils.parse_int-backed nullable integer arithmetic.
 *   Evaluator       : __init__ (recursive wrap) + eval (lazy binary
 *                     thunks, normalize).  UNARY (~18) + BINARY (11) ops.
 *
 * Value model (EvVal): a value is a string, a list, or a bool — exactly
 * Python's runtime values here. normalize(): Python-falsy -> "",
 * truthy -> str(value).
 */
#ifndef ZXBC_PEEPHOLE_EVALUATOR_H
#define ZXBC_PEEPHOLE_EVALUATOR_H

#include <stdbool.h>
#include "arena.h"
#include "vec.h"
#include "hashmap.h"

/* ---- FN (StrEnum) — string values verbatim from evaluator.py -------- */
typedef enum {
    FN_OP_NOT,   /* "!"  */
    FN_OP_PLUS,  /* "+"  */
    FN_OP_EQ,    /* "==" */
    FN_OP_NE,    /* "<>" */
    FN_OP_AND,   /* "&&" */
    FN_OP_OR,    /* "||" */
    FN_OP_IN,    /* "IN" */
    FN_OP_COMMA, /* ","  */
    FN_OP_NPLUS, /* ".+" */
    FN_OP_NSUB,  /* ".-" */
    FN_OP_NMUL,  /* ".*" */
    FN_OP_NDIV,  /* "./" */
    FN_IS_ASM, FN_IS_INDIR, FN_IS_REG16, FN_IS_REG8, FN_IS_LABEL,
    FN_IS_IMMED, FN_LEN, FN_INSTR, FN_HIREG, FN_LOREG, FN_HIVAL,
    FN_LOVAL, FN_GVAL, FN_IS_REQUIRED, FN_CTEST, FN_NEEDS, FN_FLAGVAL,
    FN_OP1, FN_OP2,
    FN__INVALID
} FN;

const char *fn_name(FN f);          /* enum -> string value */
bool        fn_lookup(const char *s, FN *out); /* FN(s); false == ValueError */
bool        fn_is_unary(FN f);      /* f in UNARY */
bool        fn_is_binary(FN f);     /* f in BINARY */
bool        fn_is_oper(const char *s); /* s in OPERS (BINARY|UNARY names) */
bool        fn_is_unary_name(const char *s);  /* `s in UNARY`  */
bool        fn_is_binary_name(const char *s); /* `s in BINARY` */

/* ---- PNode : parser TreeType --------------------------------------- */
typedef struct PNode PNode;
typedef VEC(PNode *) PNodeVec;

typedef enum { PN_STR, PN_LIST } PNodeKind;
struct PNode {
    PNodeKind kind;
    char     *str;   /* PN_STR */
    PNodeVec  list;  /* PN_LIST */
};

PNode *pnode_str(Arena *a, const char *s);
PNode *pnode_list(Arena *a);                 /* empty list */
void   pnode_push(PNode *l, PNode *child);
PNode *pnode_clone(Arena *a, const PNode *n);

/* ---- Evaluator ----------------------------------------------------- */
typedef struct Ev Ev;

/* Build Evaluator from a parser tree (PNode). NULL on ValueError
 * ("Invalid operator ...") — propagated by the engine like Python. */
Ev *ev_new(Arena *a, const PNode *expression, bool *value_error);

/* Evaluated value. */
typedef enum { EVV_STR, EVV_LIST, EVV_BOOL } EvValKind;
typedef struct EvVal EvVal;
typedef VEC(EvVal *) EvValVec;
struct EvVal {
    EvValKind kind;
    char     *s;     /* EVV_STR */
    bool      b;     /* EVV_BOOL */
    EvValVec  list;  /* EVV_LIST */
};

/* eval(vars_). vars_ : HashMap<"$N", char*>. On UnboundVarError sets
 * *unbound=true and returns NULL (engine propagates, matching Python). */
EvVal *ev_eval(Arena *a, Ev *e, const HashMap *vars_, bool *unbound);

/* Python-truthiness of an EvVal (for `if not cond.eval(match)`). */
bool evval_truthy(const EvVal *v);
/* Python str() of an EvVal (used by DEFINE results stored back as $vars). */
char *evval_str(Arena *a, const EvVal *v);

/* helpers.init() forwarder (GVAL/FLAGVAL use new_tmp_val's counter). */
void ev_helpers_init(void);

#endif /* ZXBC_PEEPHOLE_EVALUATOR_H */
