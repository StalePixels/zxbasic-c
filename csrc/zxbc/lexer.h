/*
 * lexer.h — BASIC lexer for ZX BASIC compiler
 *
 * Ported from src/zxbc/zxblex.py. Hand-written lexer with multiple states
 * (INITIAL, string, asm, preproc, comment, bin) matching PLY's behavior.
 */
#ifndef ZXBC_LEXER_H
#define ZXBC_LEXER_H

#include "zxbc.h"
#include <stdbool.h>

/* ----------------------------------------------------------------
 * Token types
 *
 * From zxblex.py: _tokens + reserved keywords + preprocessor directives
 * ---------------------------------------------------------------- */
typedef enum {
    /* Special */
    BTOK_EOF = 0,
    BTOK_NEWLINE,
    BTOK_ERROR,

    /* Operators and punctuation */
    BTOK_PLUS,        /* + */
    BTOK_MINUS,       /* - */
    BTOK_MUL,         /* * */
    BTOK_DIV,         /* / */
    BTOK_POW,         /* ^ */
    BTOK_LP,          /* ( */
    BTOK_RP,          /* ) */
    BTOK_LBRACE,      /* { */
    BTOK_RBRACE,      /* } */
    BTOK_EQ,          /* = */
    BTOK_LT,          /* < */
    BTOK_GT,          /* > */
    BTOK_LE,          /* <= */
    BTOK_GE,          /* >= */
    BTOK_NE,          /* <> */
    BTOK_WEQ,         /* := */
    BTOK_CO,          /* : */
    BTOK_SC,          /* ; */
    BTOK_COMMA,       /* , */
    BTOK_RIGHTARROW,  /* => */
    BTOK_ADDRESSOF,   /* @ */
    BTOK_SHL,         /* << */
    BTOK_SHR,         /* >> */
    BTOK_BAND,        /* & */
    BTOK_BOR,         /* | */
    BTOK_BXOR,        /* ~ */
    BTOK_BNOT,        /* ! */

    /* Literals */
    BTOK_NUMBER,      /* numeric literal (integer or float) */
    BTOK_STRC,        /* string constant (after closing quote) */
    BTOK_ID,          /* identifier */
    BTOK_ARRAY_ID,    /* array identifier */
    BTOK_LABEL,       /* line label (number or id at start of line) */
    BTOK_ASM,         /* inline ASM block content */

    /* Keywords (from keywords.py) */
    BTOK_ABS, BTOK_ACS, BTOK_AND, BTOK_AS, BTOK_AT,
    BTOK_ASN, BTOK_ATN,
    BTOK_BEEP, BTOK_BIN, BTOK_BOLD, BTOK_BORDER, BTOK_BRIGHT,
    BTOK_BYREF, BTOK_BYVAL,
    BTOK_CAST, BTOK_CHR, BTOK_CIRCLE, BTOK_CLS, BTOK_CODE,
    BTOK_CONST, BTOK_CONTINUE, BTOK_COS,
    BTOK_DATA, BTOK_DECLARE, BTOK_DIM, BTOK_DO, BTOK_DRAW,
    BTOK_ELSE, BTOK_ELSEIF, BTOK_END, BTOK_ENDIF, BTOK_ERROR_KW,
    BTOK_EXIT, BTOK_EXP,
    BTOK_FASTCALL, BTOK_FLASH, BTOK_FOR, BTOK_FUNCTION,
    BTOK_GO, BTOK_GOTO, BTOK_GOSUB,
    BTOK_IF, BTOK_IN, BTOK_INK, BTOK_INKEY, BTOK_INT, BTOK_INVERSE,
    BTOK_ITALIC,
    BTOK_LBOUND, BTOK_LET, BTOK_LEN, BTOK_LN, BTOK_LOAD, BTOK_LOOP,
    BTOK_MOD,
    BTOK_NEXT, BTOK_NOT,
    BTOK_ON, BTOK_OR, BTOK_OUT, BTOK_OVER,
    BTOK_PAPER, BTOK_PAUSE, BTOK_PEEK, BTOK_PI, BTOK_PLOT, BTOK_POKE,
    BTOK_PRINT,
    BTOK_RANDOMIZE, BTOK_READ, BTOK_RESTORE, BTOK_RETURN, BTOK_RND,
    BTOK_SAVE, BTOK_SGN, BTOK_SIN, BTOK_SIZEOF, BTOK_SQR,
    BTOK_STDCALL, BTOK_STEP, BTOK_STOP, BTOK_STR, BTOK_SUB,
    BTOK_TAB, BTOK_TAN, BTOK_THEN, BTOK_TO,
    BTOK_UBOUND, BTOK_UNTIL, BTOK_USR,
    BTOK_VAL, BTOK_VERIFY,
    BTOK_WEND, BTOK_WHILE,
    BTOK_XOR,

    /* Type keywords */
    BTOK_BYTE, BTOK_UBYTE, BTOK_INTEGER, BTOK_UINTEGER,
    BTOK_LONG, BTOK_ULONG, BTOK_FIXED, BTOK_FLOAT, BTOK_STRING,

    /* Preprocessor directives */
    BTOK__LINE,
    BTOK__INIT,
    BTOK__REQUIRE,
    BTOK__PRAGMA,
    BTOK__PUSH,
    BTOK__POP,

    BTOK_COUNT,
} BTokenType;

/* Get token type name for debugging */
const char *btok_name(BTokenType t);

/* ----------------------------------------------------------------
 * Token
 * ---------------------------------------------------------------- */
typedef struct BToken {
    BTokenType type;
    int lineno;
    double numval;        /* for BTOK_NUMBER */
    char *sval;           /* for BTOK_ID, BTOK_STRC, BTOK_ASM, BTOK_LABEL (arena-allocated) */
    char *text;           /* original text of the token */
} BToken;

/* ----------------------------------------------------------------
 * Lexer state
 * ---------------------------------------------------------------- */
typedef enum {
    BLEXST_INITIAL = 0,
    BLEXST_STRING,
    BLEXST_ASM,
    BLEXST_PREPROC,
    BLEXST_COMMENT,
    BLEXST_BIN,
} BLexState;

#define BLEX_STATE_STACK_MAX 8

typedef struct BLexer {
    CompilerState *cs;
    const char *input;
    int pos;
    int len;
    int lineno;
    bool labels_allowed;

    /* State stack (for nested comments, etc.) */
    BLexState state;
    BLexState state_stack[BLEX_STATE_STACK_MAX];
    int state_depth;

    /* String accumulator */
    char *str_buf;
    int str_len;
    int str_cap;

    /* ASM block accumulator */
    char *asm_buf;
    int asm_len;
    int asm_cap;
    int asm_lineno;     /* line where ASM block started */

    /* Comment nesting level */
    int comment_level;
} BLexer;

/* Initialize/reset lexer */
void blexer_init(BLexer *lex, CompilerState *cs, const char *input);

/* Get next token */
BToken blexer_next(BLexer *lex);

/* Compute column of current position */
int blexer_find_column(BLexer *lex, int pos);

#endif /* ZXBC_LEXER_H */
