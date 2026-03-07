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
