/*
 * ply_engine.h — C port of PLY's LALR(1) parse engine (src/ply/yacc.py
 * LRParser.parse). Drives the authoritative ply_action/ply_goto/ply_prod
 * tables (csrc/zxbc/plyparser/ply_tables.{h,c}) to make the SAME
 * shift/reduce/error decisions as Python's PLY parser.
 *
 * This file is the MECHANISM only (the shift/reduce/error-recovery loop and
 * the parse stack). The grammar's per-production semantic actions (AST
 * building) and the lexer are injected via callbacks, exactly as PLY injects
 * p.callable and lexer.token. Nothing here is invented: each branch mirrors a
 * specific branch of yacc.py's parse() loop (line refs in ply_engine.c).
 */
#ifndef ZXBC_PLY_ENGINE_H
#define ZXBC_PLY_ENGINE_H

#include <stdbool.h>
#include "ply_tables.h"

/* A grammar symbol value carried on the parse stack. Opaque to the engine —
 * the action callback interprets it. We carry the bookkeeping PLY's
 * YaccSymbol carries that the engine itself touches: the symbol type id and
 * a line number (for error reporting + the error token's lineno). The actual
 * semantic payload (AST node, etc.) is the void* `value`. */
typedef struct PlySym {
    int type;        /* symbol id (terminal/$end/error/nonterminal) */
    int lineno;      /* source line (LexToken.lineno) */
    void *value;     /* semantic value (token value or built AST node) */
} PlySym;

struct PlyParser;

/* Token source: fill *out with the next token; return false at EOF ($end).
 * Mirrors lexer.token() returning None at EOF. `ud` is user data. */
typedef bool (*PlyLexFn)(void *ud, PlySym *out);

/* Reduce action for production `prodno`. `rhs` points at the RHS slice of
 * length `len` (rhs[0..len-1] are the popped symbols); the action writes the
 * resulting LHS value into *result_value (PLY's pslice[0]). Returns true on
 * success; false signals a SyntaxError (PLY's `except SyntaxError` → error
 * recovery). Mirrors p.callable(pslice). `ud` is user data. */
typedef bool (*PlyActionFn)(void *ud, int prodno, PlySym *rhs, int len,
                            void **result_value, int *result_lineno);

/* Error callback: PLY's p_error(errtoken). errtoken==NULL means EOF/None.
 * Mirrors self.errorfunc(errtoken). `ud` is user data. The callback may call
 * ply_errok() to signal panic-mode recovery (PLY's errok()). */
typedef void (*PlyErrorFn)(void *ud, const PlySym *errtoken);

typedef struct PlyParser {
    PlyLexFn lex;
    PlyActionFn action;
    PlyErrorFn error;
    void *ud;

    /* errok mechanism (PLY self.errorok). Set true by the error callback via
     * ply_errok() to indicate it recovered and `errok_token` is the next
     * lookahead to use. */
    bool errorok;
    bool errok_token_set;
    PlySym errok_token;

    int cur_lineno;  /* lexer.lineno equivalent at end-of-input (for p_error) */

    /* Optional decision-trace hook (validation only; NULL in production).
     * kind: 'S' shift(arg=state), 'R' reduce(arg=prodno), 'A' accept,
     * 'E' error(arg=lookahead type id). Lets a harness compare the C loop's
     * shift/reduce decisions against PLY's reference trace. */
    void (*trace)(void *ud, char kind, int arg);
} PlyParser;

void ply_parser_init(PlyParser *p, PlyLexFn lex, PlyActionFn action,
                     PlyErrorFn error, void *ud);

/* Called from within the PlyErrorFn to perform PLY's errok(): the returned
 * token replaces the lookahead and parsing resumes. */
void ply_errok(PlyParser *p, const PlySym *next_lookahead);

/* Run the parse. Returns the start-symbol's value (PLY's parse() return), or
 * NULL on a fatal/aborted parse. */
void *ply_parse(PlyParser *p);

#endif /* ZXBC_PLY_ENGINE_H */
