/*
 * parser.h — BASIC parser for ZX BASIC compiler
 *
 * Hand-written recursive descent parser with Pratt expression parsing.
 * Ported from src/zxbc/zxbparser.py (243 grammar rules).
 */
#ifndef ZXBC_PARSER_H
#define ZXBC_PARSER_H

#include "zxbc.h"
#include "lexer.h"

/* ----------------------------------------------------------------
 * Parser state
 * ---------------------------------------------------------------- */
typedef struct Parser {
    CompilerState *cs;
    BLexer lexer;
    BToken current;       /* current token (lookahead) */
    BToken previous;      /* previously consumed token */
    bool had_error;
    bool panic_mode;      /* suppress cascading errors */
} Parser;

/* Initialize parser */
void parser_init(Parser *p, CompilerState *cs, const char *input);

/* Parse a complete program. Returns the AST root, or NULL on error. */
AstNode *parser_parse(Parser *p);

/* ----------------------------------------------------------------
 * Phase D — PLY engine parallel parser (validation; not the production path)
 * ---------------------------------------------------------------- */
/* Init without priming the first token (engine pulls tokens itself). */
void parser_init_noprime(Parser *p, CompilerState *cs, const char *input);
/* Parse the program via the ported PLY LALR(1) engine + reduce-actions.
 * Returns the program AST (start-symbol value). *unwired_out is set true if
 * any not-yet-ported production (or p_error) fired; *unwired_prod_out gets the
 * first such production number (or -1 for p_error). */
AstNode *plyparse_program(Parser *p, bool *unwired_out, int *unwired_prod_out);

/* Phase C-full: parse via the engine in ERROR-EMIT mode — pd_error emits the
 * real p_error message+line (to cs's err stream) and bumps cs->error_count,
 * exactly as Python's p_error. *p_error_fired_out reports whether p_error
 * fired. Used by the engine-vs-Python error-output validation harness. */
AstNode *plyparse_program_emit_errors(Parser *p, bool *p_error_fired_out);

/* ----------------------------------------------------------------
 * Expression parsing (Pratt parser)
 * ---------------------------------------------------------------- */

/* Operator precedence levels matching Python's precedence table */
typedef enum {
    PREC_NONE = 0,
    PREC_LABEL,        /* LABEL, NEWLINE */
    PREC_COLON,        /* : */
    PREC_OR,           /* OR */
    PREC_AND,          /* AND */
    PREC_XOR,          /* XOR */
    PREC_NOT,          /* NOT (right) */
    PREC_COMPARISON,   /* < > = <= >= <> */
    PREC_BOR,          /* | (BOR) */
    PREC_BAND,         /* & ~ >> << (BAND, BXOR, SHR, SHL) */
    PREC_BNOT_ADD,     /* ! + - (BNOT, PLUS, MINUS) */
    PREC_MOD,          /* MOD */
    PREC_MULDIV,       /* * / */
    PREC_UNARY,        /* unary - (UMINUS) */
    PREC_POWER,        /* ^ (right-assoc) */
    PREC_PRIMARY,      /* literals, identifiers, () */
} Precedence;

/* Parse an expression with minimum precedence */
AstNode *parse_expression(Parser *p, Precedence min_prec);

/* Parse a primary expression (atom) */
AstNode *parse_primary(Parser *p);

#endif /* ZXBC_PARSER_H */
