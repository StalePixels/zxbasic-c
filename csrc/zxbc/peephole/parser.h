/*
 * parser.h — Port of src/arch/z80/peephole/parser.py
 *
 *   parse_str(spec) -> parsed dict | None
 *     - region `{{ ... }}` state machine (ST_INITIAL / ST_REGION)
 *     - COMMENT ";;" only in ST_INITIAL
 *     - RE_REGION / RE_DEF
 *     - NUMERIC coercion of OLEVEL / OFLAG (RE_INT, int())
 *     - REQUIRED = (REPLACE, WITH, OLEVEL, OFLAG)
 *     - REG_DEFINE lines -> (var, Evaluator, lineno)
 *     - IF region lines joined with " " then operator-precedence parse
 *       (parse_ifline: IF_OPERATORS precedence + tree rebalance)
 *     - simplify_expr
 *
 * On any error parse_str returns NULL (Python returns None; engine drops
 * the pattern). Warnings are emitted to stderr — none of the 52 shipped
 * .opt files trigger them, so their exact text is not byte-critical
 * (documented; the engine only observes the None/dict outcome).
 */
#ifndef ZXBC_PEEPHOLE_PARSER_H
#define ZXBC_PEEPHOLE_PARSER_H

#include <stdbool.h>
#include "arena.h"
#include "vec.h"
#include "evaluator.h"

typedef VEC(char *) ParStrVec;

typedef struct PeepDefine {
    char *var;       /* "$N" */
    Ev   *expr;      /* DefineLine.expr (Evaluator) */
    int   lineno;
} PeepDefine;
typedef VEC(PeepDefine) PeepDefVec;

typedef struct PeepholeParsed {
    bool       has_olevel, has_oflag;
    int        olevel;       /* O_LEVEL (int, NUMERIC-coerced) */
    int        oflag;        /* O_FLAG  (int, NUMERIC-coerced) */
    ParStrVec  replace;      /* REG_REPLACE — list of .line strings */
    ParStrVec  with_;        /* REG_WITH    — list of .line strings */
    PNode     *if_tree;      /* REG_IF parsed tree (NULL == empty -> [])  */
    bool       if_is_empty;  /* IF region had no lines (parse_ifline of "") */
    PeepDefVec defines;      /* REG_DEFINE  — (var, Evaluator, lineno) */
} PeepholeParsed;

/* Parse a whole .opt spec string. Returns NULL on any error. */
PeepholeParsed *peep_parse_str(Arena *a, const char *spec);

/* parse_ifline (exposed for reuse / tests). Returns the tree, or NULL on
 * error; *empty set when the (joined) input is whitespace-only -> []. */
PNode *peep_parse_ifline(Arena *a, const char *if_line, int lineno,
                         bool *error, bool *empty);

#endif /* ZXBC_PEEPHOLE_PARSER_H */
