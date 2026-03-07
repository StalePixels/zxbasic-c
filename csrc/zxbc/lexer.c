/*
 * lexer.c — BASIC lexer for ZX BASIC compiler
 *
 * Ported from src/zxbc/zxblex.py. Hand-written lexer with multiple states.
 */
#include "lexer.h"
#include "errmsg.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Keywords table (from keywords.py)
 *
 * Sorted for binary search. All keys are lowercase.
 * ---------------------------------------------------------------- */
typedef struct {
    const char *name;
    BTokenType token;
} KeywordEntry;

static const KeywordEntry keywords[] = {
    { "abs",       BTOK_ABS },
    { "acs",       BTOK_ACS },
    { "and",       BTOK_AND },
    { "as",        BTOK_AS },
    { "asm",       BTOK_ASM },
    { "asn",       BTOK_ASN },
    { "at",        BTOK_AT },
    { "atn",       BTOK_ATN },
    { "band",      BTOK_BAND },
    { "beep",      BTOK_BEEP },
    { "bin",       BTOK_BIN },
    { "bnot",      BTOK_BNOT },
    { "bold",      BTOK_BOLD },
    { "bor",       BTOK_BOR },
    { "border",    BTOK_BORDER },
    { "bright",    BTOK_BRIGHT },
    { "byte",      BTOK_BYTE },
    { "bxor",      BTOK_BXOR },
    { "byref",     BTOK_BYREF },
    { "byval",     BTOK_BYVAL },
    { "cast",      BTOK_CAST },
    { "chr",       BTOK_CHR },
    { "chr$",      BTOK_CHR },
    { "circle",    BTOK_CIRCLE },
    { "cls",       BTOK_CLS },
    { "code",      BTOK_CODE },
    { "const",     BTOK_CONST },
    { "continue",  BTOK_CONTINUE },
    { "cos",       BTOK_COS },
    { "data",      BTOK_DATA },
    { "declare",   BTOK_DECLARE },
    { "dim",       BTOK_DIM },
    { "do",        BTOK_DO },
    { "draw",      BTOK_DRAW },
    { "else",      BTOK_ELSE },
    { "elseif",    BTOK_ELSEIF },
    { "end",       BTOK_END },
    { "endif",     BTOK_ENDIF },
    { "error",     BTOK_ERROR_KW },
    { "exit",      BTOK_EXIT },
    { "exp",       BTOK_EXP },
    { "fastcall",  BTOK_FASTCALL },
    { "fixed",     BTOK_FIXED },
    { "flash",     BTOK_FLASH },
    { "float",     BTOK_FLOAT },
    { "for",       BTOK_FOR },
    { "function",  BTOK_FUNCTION },
    { "go",        BTOK_GO },
    { "gosub",     BTOK_GOSUB },
    { "goto",      BTOK_GOTO },
    { "if",        BTOK_IF },
    { "in",        BTOK_IN },
    { "ink",       BTOK_INK },
    { "inkey",     BTOK_INKEY },
    { "inkey$",    BTOK_INKEY },
    { "int",       BTOK_INT },
    { "integer",   BTOK_INTEGER },
    { "inverse",   BTOK_INVERSE },
    { "italic",    BTOK_ITALIC },
    { "lbound",    BTOK_LBOUND },
    { "len",       BTOK_LEN },
    { "let",       BTOK_LET },
    { "ln",        BTOK_LN },
    { "load",      BTOK_LOAD },
    { "long",      BTOK_LONG },
    { "loop",      BTOK_LOOP },
    { "mod",       BTOK_MOD },
    { "next",      BTOK_NEXT },
    { "not",       BTOK_NOT },
    { "on",        BTOK_ON },
    { "or",        BTOK_OR },
    { "out",       BTOK_OUT },
    { "over",      BTOK_OVER },
    { "paper",     BTOK_PAPER },
    { "pause",     BTOK_PAUSE },
    { "peek",      BTOK_PEEK },
    { "pi",        BTOK_PI },
    { "plot",      BTOK_PLOT },
    { "poke",      BTOK_POKE },
    { "print",     BTOK_PRINT },
    { "randomize", BTOK_RANDOMIZE },
    { "read",      BTOK_READ },
    { "restore",   BTOK_RESTORE },
    { "return",    BTOK_RETURN },
    { "rnd",       BTOK_RND },
    { "save",      BTOK_SAVE },
    { "sgn",       BTOK_SGN },
    { "shl",       BTOK_SHL },
    { "shr",       BTOK_SHR },
    { "sin",       BTOK_SIN },
    { "sizeof",    BTOK_SIZEOF },
    { "sqr",       BTOK_SQR },
    { "stdcall",   BTOK_STDCALL },
    { "step",      BTOK_STEP },
    { "stop",      BTOK_STOP },
    { "str",       BTOK_STR },
    { "str$",      BTOK_STR },
    { "string",    BTOK_STRING },
    { "sub",       BTOK_SUB },
    { "tab",       BTOK_TAB },
    { "tan",       BTOK_TAN },
    { "then",      BTOK_THEN },
    { "to",        BTOK_TO },
    { "ubyte",     BTOK_UBYTE },
    { "ubound",    BTOK_UBOUND },
    { "uinteger",  BTOK_UINTEGER },
    { "ulong",     BTOK_ULONG },
    { "until",     BTOK_UNTIL },
    { "usr",       BTOK_USR },
    { "val",       BTOK_VAL },
    { "verify",    BTOK_VERIFY },
    { "wend",      BTOK_WEND },
    { "while",     BTOK_WHILE },
    { "xor",       BTOK_XOR },
};

#define NUM_KEYWORDS (sizeof(keywords) / sizeof(keywords[0]))

/* Preprocessor directives */
typedef struct {
    const char *name;
    BTokenType token;
} PreprocEntry;

static const PreprocEntry preproc_directives[] = {
    { "init",    BTOK__INIT },
    { "line",    BTOK__LINE },
    { "pop",     BTOK__POP },
    { "pragma",  BTOK__PRAGMA },
    { "push",    BTOK__PUSH },
    { "require", BTOK__REQUIRE },
};

#define NUM_PREPROC (sizeof(preproc_directives) / sizeof(preproc_directives[0]))

/* ----------------------------------------------------------------
 * Helper: case-insensitive keyword lookup
 * ---------------------------------------------------------------- */
static BTokenType lookup_keyword(const char *id) {
    /* Linear scan — keyword table is small enough */
    char lower[64];
    int i;
    for (i = 0; id[i] && i < 63; i++)
        lower[i] = (char)tolower((unsigned char)id[i]);
    lower[i] = '\0';

    for (size_t k = 0; k < NUM_KEYWORDS; k++) {
        if (strcmp(lower, keywords[k].name) == 0)
            return keywords[k].token;
    }
    return BTOK_ID;
}

static BTokenType lookup_preproc(const char *id) {
    char lower[64];
    int i;
    for (i = 0; id[i] && i < 63; i++)
        lower[i] = (char)tolower((unsigned char)id[i]);
    lower[i] = '\0';

    for (size_t k = 0; k < NUM_PREPROC; k++) {
        if (strcmp(lower, preproc_directives[k].name) == 0)
            return preproc_directives[k].token;
    }
    return BTOK_ID;
}

/* ----------------------------------------------------------------
 * Token name for debugging
 * ---------------------------------------------------------------- */
const char *btok_name(BTokenType t) {
    static const char *names[] = {
        [BTOK_EOF] = "EOF", [BTOK_NEWLINE] = "NEWLINE", [BTOK_ERROR] = "ERROR",
        [BTOK_PLUS] = "PLUS", [BTOK_MINUS] = "MINUS", [BTOK_MUL] = "MUL",
        [BTOK_DIV] = "DIV", [BTOK_POW] = "POW", [BTOK_LP] = "LP", [BTOK_RP] = "RP",
        [BTOK_LBRACE] = "LBRACE", [BTOK_RBRACE] = "RBRACE",
        [BTOK_EQ] = "EQ", [BTOK_LT] = "LT", [BTOK_GT] = "GT",
        [BTOK_LE] = "LE", [BTOK_GE] = "GE", [BTOK_NE] = "NE",
        [BTOK_WEQ] = "WEQ", [BTOK_CO] = "CO", [BTOK_SC] = "SC",
        [BTOK_COMMA] = "COMMA", [BTOK_RIGHTARROW] = "RIGHTARROW",
        [BTOK_ADDRESSOF] = "ADDRESSOF",
        [BTOK_SHL] = "SHL", [BTOK_SHR] = "SHR",
        [BTOK_BAND] = "BAND", [BTOK_BOR] = "BOR", [BTOK_BXOR] = "BXOR",
        [BTOK_BNOT] = "BNOT",
        [BTOK_NUMBER] = "NUMBER", [BTOK_STRC] = "STRC",
        [BTOK_ID] = "ID", [BTOK_ARRAY_ID] = "ARRAY_ID", [BTOK_LABEL] = "LABEL",
        [BTOK_ASM] = "ASM",
        [BTOK_ABS] = "ABS", [BTOK_ACS] = "ACS", [BTOK_AND] = "AND",
        [BTOK_AS] = "AS", [BTOK_AT] = "AT", [BTOK_ASN] = "ASN", [BTOK_ATN] = "ATN",
        [BTOK_BEEP] = "BEEP", [BTOK_BIN] = "BIN",
        [BTOK_BOLD] = "BOLD", [BTOK_BORDER] = "BORDER", [BTOK_BRIGHT] = "BRIGHT",
        [BTOK_BYREF] = "BYREF", [BTOK_BYVAL] = "BYVAL",
        [BTOK_CAST] = "CAST", [BTOK_CHR] = "CHR", [BTOK_CIRCLE] = "CIRCLE",
        [BTOK_CLS] = "CLS", [BTOK_CODE] = "CODE",
        [BTOK_CONST] = "CONST", [BTOK_CONTINUE] = "CONTINUE", [BTOK_COS] = "COS",
        [BTOK_DATA] = "DATA", [BTOK_DECLARE] = "DECLARE",
        [BTOK_DIM] = "DIM", [BTOK_DO] = "DO", [BTOK_DRAW] = "DRAW",
        [BTOK_ELSE] = "ELSE", [BTOK_ELSEIF] = "ELSEIF",
        [BTOK_END] = "END", [BTOK_ENDIF] = "ENDIF",
        [BTOK_ERROR_KW] = "ERROR", [BTOK_EXIT] = "EXIT", [BTOK_EXP] = "EXP",
        [BTOK_FASTCALL] = "FASTCALL", [BTOK_FLASH] = "FLASH",
        [BTOK_FOR] = "FOR", [BTOK_FUNCTION] = "FUNCTION",
        [BTOK_GO] = "GO", [BTOK_GOTO] = "GOTO", [BTOK_GOSUB] = "GOSUB",
        [BTOK_IF] = "IF", [BTOK_IN] = "IN", [BTOK_INK] = "INK",
        [BTOK_INKEY] = "INKEY", [BTOK_INT] = "INT", [BTOK_INVERSE] = "INVERSE",
        [BTOK_ITALIC] = "ITALIC",
        [BTOK_LBOUND] = "LBOUND", [BTOK_LET] = "LET", [BTOK_LEN] = "LEN",
        [BTOK_LN] = "LN", [BTOK_LOAD] = "LOAD", [BTOK_LOOP] = "LOOP",
        [BTOK_MOD] = "MOD",
        [BTOK_NEXT] = "NEXT", [BTOK_NOT] = "NOT",
        [BTOK_ON] = "ON", [BTOK_OR] = "OR", [BTOK_OUT] = "OUT", [BTOK_OVER] = "OVER",
        [BTOK_PAPER] = "PAPER", [BTOK_PAUSE] = "PAUSE",
        [BTOK_PEEK] = "PEEK", [BTOK_PI] = "PI", [BTOK_PLOT] = "PLOT",
        [BTOK_POKE] = "POKE", [BTOK_PRINT] = "PRINT",
        [BTOK_RANDOMIZE] = "RANDOMIZE", [BTOK_READ] = "READ",
        [BTOK_RESTORE] = "RESTORE", [BTOK_RETURN] = "RETURN", [BTOK_RND] = "RND",
        [BTOK_SAVE] = "SAVE", [BTOK_SGN] = "SGN",
        [BTOK_SIN] = "SIN", [BTOK_SIZEOF] = "SIZEOF", [BTOK_SQR] = "SQR",
        [BTOK_STDCALL] = "STDCALL", [BTOK_STEP] = "STEP",
        [BTOK_STOP] = "STOP", [BTOK_STR] = "STR", [BTOK_SUB] = "SUB",
        [BTOK_TAB] = "TAB", [BTOK_TAN] = "TAN", [BTOK_THEN] = "THEN",
        [BTOK_TO] = "TO",
        [BTOK_UBOUND] = "UBOUND", [BTOK_UNTIL] = "UNTIL", [BTOK_USR] = "USR",
        [BTOK_VAL] = "VAL", [BTOK_VERIFY] = "VERIFY",
        [BTOK_WEND] = "WEND", [BTOK_WHILE] = "WHILE",
        [BTOK_XOR] = "XOR",
        [BTOK_BYTE] = "BYTE", [BTOK_UBYTE] = "UBYTE",
        [BTOK_INTEGER] = "INTEGER", [BTOK_UINTEGER] = "UINTEGER",
        [BTOK_LONG] = "LONG", [BTOK_ULONG] = "ULONG",
        [BTOK_FIXED] = "FIXED", [BTOK_FLOAT] = "FLOAT", [BTOK_STRING] = "STRING",
        [BTOK__LINE] = "_LINE", [BTOK__INIT] = "_INIT",
        [BTOK__REQUIRE] = "_REQUIRE", [BTOK__PRAGMA] = "_PRAGMA",
        [BTOK__PUSH] = "_PUSH", [BTOK__POP] = "_POP",
    };
    if (t >= 0 && t < BTOK_COUNT)
        return names[t] ? names[t] : "?";
    return "?";
}

/* ----------------------------------------------------------------
 * Lexer helpers
 * ---------------------------------------------------------------- */

static inline char peek(BLexer *lex) {
    return (lex->pos < lex->len) ? lex->input[lex->pos] : '\0';
}

static inline char peek2(BLexer *lex) {
    return (lex->pos + 1 < lex->len) ? lex->input[lex->pos + 1] : '\0';
}

static inline char advance(BLexer *lex) {
    return (lex->pos < lex->len) ? lex->input[lex->pos++] : '\0';
}

static inline bool at_end(BLexer *lex) {
    return lex->pos >= lex->len;
}

static void push_state(BLexer *lex, BLexState state) {
    if (lex->state_depth < BLEX_STATE_STACK_MAX) {
        lex->state_stack[lex->state_depth++] = lex->state;
    }
    lex->state = state;
}

static void pop_state(BLexer *lex) {
    if (lex->state_depth > 0) {
        lex->state = lex->state_stack[--lex->state_depth];
    } else {
        lex->state = BLEXST_INITIAL;
    }
}

/* Append char to string buffer */
static void str_append_char(BLexer *lex, char c) {
    if (lex->str_len >= lex->str_cap) {
        int new_cap = lex->str_cap < 64 ? 64 : lex->str_cap * 2;
        char *new_buf = arena_alloc(&lex->cs->arena, new_cap);
        if (lex->str_buf)
            memcpy(new_buf, lex->str_buf, lex->str_len);
        lex->str_buf = new_buf;
        lex->str_cap = new_cap;
    }
    lex->str_buf[lex->str_len++] = c;
}

/* Append char to ASM buffer */
static void asm_append_char(BLexer *lex, char c) {
    if (lex->asm_len >= lex->asm_cap) {
        int new_cap = lex->asm_cap < 256 ? 256 : lex->asm_cap * 2;
        char *new_buf = arena_alloc(&lex->cs->arena, new_cap);
        if (lex->asm_buf)
            memcpy(new_buf, lex->asm_buf, lex->asm_len);
        lex->asm_buf = new_buf;
        lex->asm_cap = new_cap;
    }
    lex->asm_buf[lex->asm_len++] = c;
}

static void asm_append_str(BLexer *lex, const char *s, int n) {
    for (int i = 0; i < n; i++)
        asm_append_char(lex, s[i]);
}

int blexer_find_column(BLexer *lex, int pos) {
    int i = pos;
    while (i > 0 && lex->input[i - 1] != '\n')
        i--;
    return pos - i + 1;
}

/* Check if current position is a label (at beginning of line) */
static bool is_label(BLexer *lex, int tok_pos, BTokenType tok_type, const char *tok_value) {
    if (!lex->labels_allowed)
        return false;

    /* Check if there's only whitespace before this token on the line */
    int c = tok_pos - 1;
    while (c > 0 && (lex->input[c] == ' ' || lex->input[c] == '\t'))
        c--;

    int i = tok_pos;
    while (i >= 0 && lex->input[i] != '\n')
        i--;

    int column = c - i;
    if (column != 0)
        return false;

    /* Numbers at start of line are always labels */
    if (tok_type == BTOK_NUMBER)
        return true;

    /* IDs are labels if followed by ':' (after optional whitespace) */
    int end = tok_pos;
    if (tok_value) end += (int)strlen(tok_value);
    while (end < lex->len && (lex->input[end] == ' ' || lex->input[end] == '\t'))
        end++;
    if (end < lex->len && lex->input[end] == ':')
        return true;

    return false;
}

/* Match a case-insensitive word at current position */
static bool match_word_ci(BLexer *lex, const char *word) {
    int start = lex->pos;
    for (int i = 0; word[i]; i++) {
        if (start + i >= lex->len)
            return false;
        if (tolower((unsigned char)lex->input[start + i]) != tolower((unsigned char)word[i]))
            return false;
    }
    int end = start + (int)strlen(word);
    /* Must not be followed by an alphanumeric or underscore */
    if (end < lex->len && (isalnum((unsigned char)lex->input[end]) || lex->input[end] == '_'))
        return false;
    return true;
}

/* ----------------------------------------------------------------
 * Lexer initialization
 * ---------------------------------------------------------------- */

void blexer_init(BLexer *lex, CompilerState *cs, const char *input) {
    memset(lex, 0, sizeof(*lex));
    lex->cs = cs;
    lex->input = input;
    lex->len = (int)strlen(input);
    lex->pos = 0;
    lex->lineno = 1;
    lex->labels_allowed = true;
    lex->state = BLEXST_INITIAL;
    lex->state_depth = 0;
    lex->comment_level = 0;
}

/* ----------------------------------------------------------------
 * Make token helpers
 * ---------------------------------------------------------------- */

static BToken make_tok(BLexer *lex, BTokenType type) {
    BToken t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.lineno = lex->lineno;
    return t;
}

static BToken make_num_tok(BLexer *lex, double val) {
    BToken t = make_tok(lex, BTOK_NUMBER);
    t.numval = val;
    return t;
}

static BToken make_str_tok(BLexer *lex, BTokenType type, const char *s) {
    BToken t = make_tok(lex, type);
    t.sval = arena_strdup(&lex->cs->arena, s);
    return t;
}

/* ----------------------------------------------------------------
 * #line directive handling
 * ---------------------------------------------------------------- */
static bool try_preproc_line(BLexer *lex) {
    /* Match: #[ \t]*[Ll][Ii][Nn][Ee][ \t]+DIGITS([ \t]+"FILENAME")?[ \t]*\n */
    int save = lex->pos;
    int save_lineno = lex->lineno;

    /* Skip # */
    if (peek(lex) != '#') return false;
    advance(lex);

    /* Skip whitespace */
    while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);

    /* Match "line" case-insensitive */
    if (!match_word_ci(lex, "line")) { lex->pos = save; return false; }
    lex->pos += 4;

    /* Require at least one space/tab */
    if (peek(lex) != ' ' && peek(lex) != '\t') { lex->pos = save; return false; }
    while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);

    /* Read line number */
    if (!isdigit((unsigned char)peek(lex))) { lex->pos = save; lex->lineno = save_lineno; return false; }
    int line_num = 0;
    while (isdigit((unsigned char)peek(lex))) {
        line_num = line_num * 10 + (advance(lex) - '0');
    }

    /* Optional filename in quotes */
    while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
    if (peek(lex) == '"') {
        advance(lex); /* skip opening quote */
        int fname_start = lex->pos;
        while (lex->pos < lex->len && peek(lex) != '"' && peek(lex) != '\n') {
            if (peek(lex) == '"' && peek2(lex) == '"') {
                advance(lex); advance(lex); /* escaped quote */
                continue;
            }
            advance(lex);
        }
        int fname_len = lex->pos - fname_start;
        if (peek(lex) == '"') advance(lex);

        /* Set filename */
        lex->cs->current_file = arena_strndup(&lex->cs->arena,
            lex->input + fname_start, fname_len);
    }

    /* Skip to end of line */
    while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
    if (peek(lex) == '\r') advance(lex);
    if (peek(lex) == '\n') advance(lex);

    lex->lineno = line_num;
    return true;
}

/* ----------------------------------------------------------------
 * State: COMMENT (block comments /' ... '/)
 * ---------------------------------------------------------------- */
static BToken lex_comment(BLexer *lex) {
    while (!at_end(lex)) {
        if (peek(lex) == '/' && peek2(lex) == '\'') {
            /* Begin nested comment */
            advance(lex); advance(lex);
            lex->comment_level++;
            push_state(lex, BLEXST_COMMENT);
        } else if (peek(lex) == '\'' && peek2(lex) == '/') {
            /* End block comment */
            advance(lex); advance(lex);
            lex->comment_level--;
            pop_state(lex);
            return blexer_next(lex); /* continue in restored state */
        } else if (peek(lex) == '\n' || (peek(lex) == '\r' && peek2(lex) == '\n')) {
            if (peek(lex) == '\r') advance(lex);
            advance(lex);
            lex->lineno++;
        } else {
            advance(lex);
        }
    }
    return make_tok(lex, BTOK_EOF);
}

/* ----------------------------------------------------------------
 * State: STRING
 * ---------------------------------------------------------------- */
static BToken lex_string(BLexer *lex) {
    lex->str_len = 0;

    while (!at_end(lex)) {
        char c = peek(lex);

        if (c == '\\' && lex->pos + 1 < lex->len) {
            char c2 = lex->input[lex->pos + 1];

            /* \\ -> backslash */
            if (c2 == '\\') {
                advance(lex); advance(lex);
                str_append_char(lex, '\\');
                continue;
            }
            /* \* -> copyright symbol (chr 127) */
            if (c2 == '*') {
                advance(lex); advance(lex);
                str_append_char(lex, 127);
                continue;
            }
            /* \<space/'/./: pair> -> UDG graphic */
            if (lex->pos + 2 < lex->len &&
                (c2 == ' ' || c2 == '\'' || c2 == '.' || c2 == ':')) {
                char c3 = lex->input[lex->pos + 2];
                if (c3 == ' ' || c3 == '\'' || c3 == '.' || c3 == ':') {
                    int P_val = 0, N_val = 0;
                    switch (c2) {
                        case ' ': P_val = 0; break;
                        case '\'': P_val = 2; break;
                        case '.': P_val = 8; break;
                        case ':': P_val = 10; break;
                    }
                    switch (c3) {
                        case ' ': N_val = 0; break;
                        case '\'': N_val = 1; break;
                        case '.': N_val = 4; break;
                        case ':': N_val = 5; break;
                    }
                    advance(lex); advance(lex); advance(lex);
                    str_append_char(lex, (char)(128 + P_val + N_val));
                    continue;
                }
            }
            /* \A-\U or \a-\u -> UDG characters */
            if ((c2 >= 'A' && c2 <= 'U') || (c2 >= 'a' && c2 <= 'u')) {
                advance(lex); advance(lex);
                str_append_char(lex, (char)(79 + toupper((unsigned char)c2)));
                continue;
            }
            /* \#NNN -> ASCII code */
            if (c2 == '#' && lex->pos + 4 < lex->len &&
                isdigit((unsigned char)lex->input[lex->pos + 2]) &&
                isdigit((unsigned char)lex->input[lex->pos + 3]) &&
                isdigit((unsigned char)lex->input[lex->pos + 4])) {
                int code = (lex->input[lex->pos + 2] - '0') * 100 +
                           (lex->input[lex->pos + 3] - '0') * 10 +
                           (lex->input[lex->pos + 4] - '0');
                advance(lex); advance(lex); advance(lex); advance(lex); advance(lex);
                str_append_char(lex, (char)code);
                continue;
            }
            /* \{pN} -> paper code */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'p' &&
                isdigit((unsigned char)lex->input[lex->pos + 3]) &&
                lex->input[lex->pos + 4] == '}') {
                str_append_char(lex, 17);
                str_append_char(lex, (char)(lex->input[lex->pos + 3] - '0'));
                lex->pos += 5;
                continue;
            }
            /* \{iN} -> ink code */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'i' &&
                isdigit((unsigned char)lex->input[lex->pos + 3]) &&
                lex->input[lex->pos + 4] == '}') {
                str_append_char(lex, 16);
                str_append_char(lex, (char)(lex->input[lex->pos + 3] - '0'));
                lex->pos += 5;
                continue;
            }
            /* \{f0} \{f1} -> flash */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'f' &&
                (lex->input[lex->pos + 3] == '0' || lex->input[lex->pos + 3] == '1') &&
                lex->input[lex->pos + 4] == '}') {
                str_append_char(lex, 18);
                str_append_char(lex, (char)(lex->input[lex->pos + 3] - '0'));
                lex->pos += 5;
                continue;
            }
            /* \{b0} \{b1} -> bright */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'b' &&
                (lex->input[lex->pos + 3] == '0' || lex->input[lex->pos + 3] == '1') &&
                lex->input[lex->pos + 4] == '}') {
                str_append_char(lex, 19);
                str_append_char(lex, (char)(lex->input[lex->pos + 3] - '0'));
                lex->pos += 5;
                continue;
            }
            /* \{v[in01]} -> inverse */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'v' &&
                lex->input[lex->pos + 4] == '}') {
                char vc = lex->input[lex->pos + 3];
                if (vc == 'n' || vc == 'i' || vc == '0' || vc == '1') {
                    str_append_char(lex, 20);
                    str_append_char(lex, (char)((vc == 'i' || vc == '1') ? 1 : 0));
                    lex->pos += 5;
                    continue;
                }
            }
            /* \{I0} \{I1} -> italic */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'I' &&
                (lex->input[lex->pos + 3] == '0' || lex->input[lex->pos + 3] == '1') &&
                lex->input[lex->pos + 4] == '}') {
                str_append_char(lex, 15);
                str_append_char(lex, (char)(lex->input[lex->pos + 3] - '0'));
                lex->pos += 5;
                continue;
            }
            /* \{B0} \{B1} -> bold */
            if (c2 == '{' && lex->pos + 4 < lex->len &&
                lex->input[lex->pos + 2] == 'B' &&
                (lex->input[lex->pos + 3] == '0' || lex->input[lex->pos + 3] == '1') &&
                lex->input[lex->pos + 4] == '}') {
                str_append_char(lex, 14);
                str_append_char(lex, (char)(lex->input[lex->pos + 3] - '0'));
                lex->pos += 5;
                continue;
            }
        }

        /* "" -> escaped double quote inside string */
        if (c == '"' && peek2(lex) == '"') {
            advance(lex); advance(lex);
            str_append_char(lex, '"');
            continue;
        }

        /* " -> end of string */
        if (c == '"') {
            advance(lex);
            lex->state = BLEXST_INITIAL;

            /* Null-terminate and create token */
            str_append_char(lex, '\0');
            BToken t = make_tok(lex, BTOK_STRC);
            t.sval = arena_strdup(&lex->cs->arena, lex->str_buf);
            return t;
        }

        /* Regular character */
        advance(lex);
        str_append_char(lex, c);
    }

    /* Unterminated string */
    return make_tok(lex, BTOK_EOF);
}

/* ----------------------------------------------------------------
 * State: ASM (inline assembly)
 * ---------------------------------------------------------------- */
static BToken lex_asm(BLexer *lex) {
    while (!at_end(lex)) {
        /* Check for END ASM */
        if (match_word_ci(lex, "end")) {
            int save = lex->pos;
            lex->pos += 3;
            /* Skip whitespace */
            while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
            if (match_word_ci(lex, "asm")) {
                lex->pos += 3;
                lex->state = BLEXST_INITIAL;

                /* Null-terminate ASM buffer */
                asm_append_char(lex, '\0');
                BToken t = make_tok(lex, BTOK_ASM);
                t.sval = arena_strdup(&lex->cs->arena, lex->asm_buf);
                t.lineno = lex->asm_lineno;
                return t;
            }
            lex->pos = save;
        }

        /* Check for #line directive inside ASM */
        if (peek(lex) == '#') {
            int save = lex->pos;
            int save_lineno = lex->lineno;
            /* Copy the #line text into ASM buffer too */
            int line_start = lex->pos;
            if (try_preproc_line(lex)) {
                asm_append_str(lex, lex->input + line_start, lex->pos - line_start);
                continue;
            }
            lex->pos = save;
            lex->lineno = save_lineno;
        }

        /* String literal in ASM (pass through) */
        if (peek(lex) == '"') {
            asm_append_char(lex, advance(lex));
            while (!at_end(lex) && peek(lex) != '"') {
                asm_append_char(lex, advance(lex));
            }
            if (peek(lex) == '"')
                asm_append_char(lex, advance(lex));
            continue;
        }

        /* Comment in ASM (skip to end of line) */
        if (peek(lex) == ';') {
            while (!at_end(lex) && peek(lex) != '\n')
                advance(lex);
            continue;
        }

        /* Newline */
        if (peek(lex) == '\n' || (peek(lex) == '\r' && peek2(lex) == '\n')) {
            if (peek(lex) == '\r') asm_append_char(lex, advance(lex));
            asm_append_char(lex, advance(lex));
            lex->lineno++;
            continue;
        }

        /* Regular character */
        asm_append_char(lex, advance(lex));
    }

    return make_tok(lex, BTOK_EOF);
}

/* ----------------------------------------------------------------
 * State: PREPROC (after # at column 1)
 * ---------------------------------------------------------------- */
static BToken lex_preproc(BLexer *lex) {
    /* Skip whitespace */
    while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);

    char c = peek(lex);

    /* Newline ends preproc state */
    if (c == '\n' || (c == '\r' && peek2(lex) == '\n')) {
        if (c == '\r') advance(lex);
        advance(lex);
        lex->lineno++;
        lex->state = BLEXST_INITIAL;
        return make_tok(lex, BTOK_NEWLINE);
    }

    /* Identifier */
    if (isalpha((unsigned char)c) || c == '_') {
        int start = lex->pos;
        while (isalpha((unsigned char)peek(lex)) || peek(lex) == '_')
            advance(lex);
        char *id = arena_strndup(&lex->cs->arena, lex->input + start, lex->pos - start);
        BTokenType tt = lookup_preproc(id);
        BToken t = make_tok(lex, tt);
        t.sval = id;
        return t;
    }

    /* Integer */
    if (isdigit((unsigned char)c)) {
        int start = lex->pos;
        while (isdigit((unsigned char)peek(lex))) advance(lex);
        char *num_str = arena_strndup(&lex->cs->arena, lex->input + start, lex->pos - start);
        BToken t = make_tok(lex, BTOK_NUMBER);
        t.numval = atof(num_str);
        t.sval = num_str;
        return t;
    }

    /* String in preproc */
    if (c == '"') {
        advance(lex);
        int start = lex->pos;
        while (!at_end(lex) && peek(lex) != '"' && peek(lex) != '\n')
            advance(lex);
        char *s = arena_strndup(&lex->cs->arena, lex->input + start, lex->pos - start);
        if (peek(lex) == '"') advance(lex);
        return make_str_tok(lex, BTOK_STRC, s);
    }

    /* Punctuation in preproc */
    if (c == '(') { advance(lex); return make_tok(lex, BTOK_LP); }
    if (c == ')') { advance(lex); return make_tok(lex, BTOK_RP); }
    if (c == '=') { advance(lex); return make_tok(lex, BTOK_EQ); }

    /* Skip other whitespace */
    if (c == ' ' || c == '\t') {
        while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
        return lex_preproc(lex);
    }

    /* Unknown char */
    advance(lex);
    zxbc_error(lex->cs, lex->lineno, "illegal character '%c'", c);
    return make_tok(lex, BTOK_ERROR);
}

/* ----------------------------------------------------------------
 * State: BIN (after BIN keyword, reading binary digits)
 * ---------------------------------------------------------------- */
static BToken lex_bin_state(BLexer *lex) {
    /* Skip whitespace */
    while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);

    char c = peek(lex);

    /* Binary digits */
    if (c == '0' || c == '1') {
        int64_t val = 0;
        while (peek(lex) == '0' || peek(lex) == '1') {
            val = val * 2 + (advance(lex) - '0');
        }
        lex->state = BLEXST_INITIAL;
        return make_num_tok(lex, (double)val);
    }

    /* Newline */
    if (c == '\n' || (c == '\r' && peek2(lex) == '\n')) {
        if (c == '\r') advance(lex);
        advance(lex);
        lex->lineno++;
        lex->labels_allowed = true;
        lex->state = BLEXST_INITIAL;
        BToken t = make_tok(lex, BTOK_NEWLINE);
        return t;
    }

    /* Block comment start */
    if (c == '/' && peek2(lex) == '\'') {
        advance(lex); advance(lex);
        lex->comment_level++;
        push_state(lex, BLEXST_COMMENT);
        return lex_comment(lex);
    }

    /* Line continuation */
    if ((c == '_' || c == '\\') && lex->pos + 1 < lex->len) {
        /* Check if rest of line is whitespace/comment then newline */
        int save = lex->pos;
        advance(lex);
        while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
        /* Optional REM or ' comment */
        if (peek(lex) == '\'') {
            while (!at_end(lex) && peek(lex) != '\n') advance(lex);
        } else if (match_word_ci(lex, "rem")) {
            while (!at_end(lex) && peek(lex) != '\n') advance(lex);
        }
        if (peek(lex) == '\r') advance(lex);
        if (peek(lex) == '\n') {
            advance(lex);
            lex->lineno++;
            lex->labels_allowed = false;
            return blexer_next(lex);
        }
        lex->pos = save;
    }

    /* Any non-binary char: return 0 and go back to INITIAL */
    lex->state = BLEXST_INITIAL;
    return make_num_tok(lex, 0);
}

/* ----------------------------------------------------------------
 * State: INITIAL
 * ---------------------------------------------------------------- */
static BToken lex_initial(BLexer *lex) {
    while (!at_end(lex)) {
        char c = peek(lex);

        /* Skip whitespace */
        if (c == ' ' || c == '\t') {
            advance(lex);
            continue;
        }

        /* Block comment /' */
        if (c == '/' && peek2(lex) == '\'') {
            advance(lex); advance(lex);
            lex->comment_level++;
            push_state(lex, BLEXST_COMMENT);
            return lex_comment(lex);
        }

        /* Newline */
        if (c == '\n' || (c == '\r' && peek2(lex) == '\n')) {
            if (c == '\r') advance(lex);
            advance(lex);
            lex->lineno++;
            lex->labels_allowed = true;
            return make_tok(lex, BTOK_NEWLINE);
        }
        if (c == '\r') {
            advance(lex);
            continue;
        }

        /* Line continuation: _ or \ at end of line */
        if (c == '_' || c == '\\') {
            int save = lex->pos;
            advance(lex);
            while (peek(lex) == ' ' || peek(lex) == '\t') advance(lex);
            /* Optional trailing comment */
            if (peek(lex) == '\'') {
                while (!at_end(lex) && peek(lex) != '\n') advance(lex);
            } else if (match_word_ci(lex, "rem")) {
                while (!at_end(lex) && peek(lex) != '\n') advance(lex);
            }
            if (peek(lex) == '\r') advance(lex);
            if (peek(lex) == '\n') {
                advance(lex);
                lex->lineno++;
                lex->labels_allowed = false;
                continue;
            }
            /* Not a line continuation — restore and treat as normal */
            lex->pos = save;
            /* Fall through to ID handling below for _ */
            if (c == '\\') {
                advance(lex);
                zxbc_error(lex->cs, lex->lineno, "illegal character '\\'");
                return make_tok(lex, BTOK_ERROR);
            }
        }

        /* #line directive (must be at column 1) */
        if (c == '#') {
            if (blexer_find_column(lex, lex->pos) == 1) {
                int save = lex->pos;
                int save_lineno = lex->lineno;
                if (try_preproc_line(lex)) {
                    continue; /* #line handled, get next token */
                }
                lex->pos = save;
                lex->lineno = save_lineno;
                /* Enter preproc state */
                advance(lex);
                lex->state = BLEXST_PREPROC;
                return lex_preproc(lex);
            }
            advance(lex);
            zxbc_error(lex->cs, lex->lineno, "illegal character '#'");
            return make_tok(lex, BTOK_ERROR);
        }

        /* String literal */
        if (c == '"') {
            advance(lex);
            lex->state = BLEXST_STRING;
            return lex_string(lex);
        }

        /* REM comment */
        if (match_word_ci(lex, "rem") && lex->pos + 3 < lex->len) {
            char after = lex->input[lex->pos + 3];
            if (after == ' ' || after == '\t') {
                /* Skip rest of line */
                while (!at_end(lex) && peek(lex) != '\n') advance(lex);
                continue;
            }
            if (after == '\r' || after == '\n') {
                /* Empty REM — emit as newline */
                lex->pos += 3;
                if (peek(lex) == '\r') advance(lex);
                advance(lex);
                lex->lineno++;
                lex->labels_allowed = true;
                return make_tok(lex, BTOK_NEWLINE);
            }
        }

        /* Single-line comment ' */
        if (c == '\'') {
            while (!at_end(lex) && peek(lex) != '\n') advance(lex);
            continue;
        }

        /* Two-char operators */
        if (c == ':' && peek2(lex) == '=') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_WEQ);
        }
        if (c == '>' && peek2(lex) == '=') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_GE);
        }
        if (c == '<' && peek2(lex) == '=') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_LE);
        }
        if (c == '<' && peek2(lex) == '>') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_NE);
        }
        if (c == '=' && peek2(lex) == '>') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_RIGHTARROW);
        }
        if (c == '<' && peek2(lex) == '<') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_SHL);
        }
        if (c == '>' && peek2(lex) == '>') {
            advance(lex); advance(lex);
            return make_tok(lex, BTOK_SHR);
        }

        /* Single-char operators */
        if (c == '+') { advance(lex); return make_tok(lex, BTOK_PLUS); }
        if (c == '-') { advance(lex); return make_tok(lex, BTOK_MINUS); }
        if (c == '*') { advance(lex); return make_tok(lex, BTOK_MUL); }
        if (c == '/') { advance(lex); return make_tok(lex, BTOK_DIV); }
        if (c == '(') { advance(lex); return make_tok(lex, BTOK_LP); }
        if (c == ')') { advance(lex); return make_tok(lex, BTOK_RP); }
        if (c == '{') { advance(lex); return make_tok(lex, BTOK_LBRACE); }
        if (c == '}') { advance(lex); return make_tok(lex, BTOK_RBRACE); }
        if (c == '=') { advance(lex); return make_tok(lex, BTOK_EQ); }
        if (c == '<') { advance(lex); return make_tok(lex, BTOK_LT); }
        if (c == '>') { advance(lex); return make_tok(lex, BTOK_GT); }
        if (c == '^') { advance(lex); return make_tok(lex, BTOK_POW); }
        if (c == ':') { advance(lex); return make_tok(lex, BTOK_CO); }
        if (c == ';') { advance(lex); return make_tok(lex, BTOK_SC); }
        if (c == ',') { advance(lex); return make_tok(lex, BTOK_COMMA); }
        if (c == '@') { advance(lex); return make_tok(lex, BTOK_ADDRESSOF); }
        if (c == '~') { advance(lex); return make_tok(lex, BTOK_BXOR); }
        if (c == '&') { advance(lex); return make_tok(lex, BTOK_BAND); }
        if (c == '|') { advance(lex); return make_tok(lex, BTOK_BOR); }
        if (c == '!') { advance(lex); return make_tok(lex, BTOK_BNOT); }

        /* Hex number: $XXXX, 0xXXXX, or NNNNh */
        if (c == '$' && lex->pos + 1 < lex->len &&
            isxdigit((unsigned char)lex->input[lex->pos + 1])) {
            advance(lex); /* skip $ */
            int64_t val = 0;
            while (isxdigit((unsigned char)peek(lex))) {
                char d = advance(lex);
                val = val * 16 + (isdigit((unsigned char)d) ? d - '0'
                                   : tolower((unsigned char)d) - 'a' + 10);
            }
            return make_num_tok(lex, (double)val);
        }
        if (c == '0' && peek2(lex) == 'x') {
            advance(lex); advance(lex);
            int64_t val = 0;
            while (isxdigit((unsigned char)peek(lex))) {
                char d = advance(lex);
                val = val * 16 + (isdigit((unsigned char)d) ? d - '0'
                                   : tolower((unsigned char)d) - 'a' + 10);
            }
            return make_num_tok(lex, (double)val);
        }

        /* Number: decimal, hex (NNh), octal (NNo), binary (NNb or %NN) */
        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peek2(lex)))) {
            int start = lex->pos;
            int tok_pos = lex->pos;

            /* Try to read hex: digits followed by 'h' */
            if (isdigit((unsigned char)c)) {
                int hstart = lex->pos;
                bool all_hex = true;
                while (isxdigit((unsigned char)peek(lex)))
                    advance(lex);
                char suffix = tolower((unsigned char)peek(lex));

                if (suffix == 'h') {
                    /* Hex number NNNh */
                    int64_t val = 0;
                    for (int i = hstart; i < lex->pos; i++) {
                        char d = lex->input[i];
                        val = val * 16 + (isdigit((unsigned char)d) ? d - '0'
                                           : tolower((unsigned char)d) - 'a' + 10);
                    }
                    advance(lex); /* skip 'h' */
                    return make_num_tok(lex, (double)val);
                }

                if (suffix == 'o') {
                    /* Octal number NNNo */
                    int64_t val = 0;
                    all_hex = true;
                    for (int i = hstart; i < lex->pos; i++) {
                        char d = lex->input[i];
                        if (d >= '0' && d <= '7')
                            val = val * 8 + (d - '0');
                        else
                            all_hex = false;
                    }
                    if (all_hex) {
                        advance(lex); /* skip 'o' */
                        return make_num_tok(lex, (double)val);
                    }
                }

                if (suffix == 'b') {
                    /* Binary NNNb — but only if all digits are 0/1 */
                    bool all_bin = true;
                    for (int i = hstart; i < lex->pos; i++) {
                        if (lex->input[i] != '0' && lex->input[i] != '1') {
                            all_bin = false;
                            break;
                        }
                    }
                    if (all_bin) {
                        int64_t val = 0;
                        for (int i = hstart; i < lex->pos; i++)
                            val = val * 2 + (lex->input[i] - '0');
                        advance(lex); /* skip 'b' */
                        return make_num_tok(lex, (double)val);
                    }
                }

                /* Not hex/octal/binary suffix — restore and parse as decimal */
                lex->pos = start;
            }

            /* Decimal number (with optional fractional and exponent) */
            while (isdigit((unsigned char)peek(lex))) advance(lex);
            if (peek(lex) == '.' && isdigit((unsigned char)lex->input[lex->pos + 1])) {
                advance(lex); /* skip . */
                while (isdigit((unsigned char)peek(lex))) advance(lex);
            } else if (peek(lex) == '.' && lex->pos > start) {
                /* Trailing dot: 123. — consume it */
                advance(lex);
                while (isdigit((unsigned char)peek(lex))) advance(lex);
            }
            if (tolower((unsigned char)peek(lex)) == 'e') {
                advance(lex);
                if (peek(lex) == '+' || peek(lex) == '-') advance(lex);
                while (isdigit((unsigned char)peek(lex))) advance(lex);
            }

            char *num_str = arena_strndup(&lex->cs->arena, lex->input + start, lex->pos - start);
            double val = atof(num_str);

            /* Check if this is a label (integer at start of line) */
            if (val == (int)val && is_label(lex, tok_pos, BTOK_NUMBER, num_str)) {
                BToken t = make_tok(lex, BTOK_LABEL);
                t.numval = val;
                t.sval = num_str;
                return t;
            }

            BToken t = make_num_tok(lex, val);
            t.text = num_str;
            return t;
        }

        /* Binary with % prefix */
        if (c == '%' && lex->pos + 1 < lex->len &&
            (lex->input[lex->pos + 1] == '0' || lex->input[lex->pos + 1] == '1')) {
            advance(lex); /* skip % */
            int64_t val = 0;
            while (peek(lex) == '0' || peek(lex) == '1')
                val = val * 2 + (advance(lex) - '0');
            return make_num_tok(lex, (double)val);
        }

        /* Identifier or keyword */
        if (isalpha((unsigned char)c) || c == '_') {
            int tok_pos = lex->pos;
            int id_start = lex->pos;
            advance(lex);
            while (isalnum((unsigned char)peek(lex)) || peek(lex) == '_')
                advance(lex);
            /* Check for deprecated suffix ($, %, &) */
            if (peek(lex) == '$' || peek(lex) == '%')
                advance(lex);

            char *id = arena_strndup(&lex->cs->arena, lex->input + id_start, lex->pos - id_start);
            BTokenType kw = lookup_keyword(id);

            if (kw == BTOK_ASM) {
                /* Enter ASM state */
                lex->state = BLEXST_ASM;
                lex->asm_len = 0;
                lex->asm_lineno = lex->lineno;
                return lex_asm(lex);
            }

            if (kw == BTOK_BIN) {
                /* Enter BIN state for reading binary literal */
                lex->state = BLEXST_BIN;
                return blexer_next(lex);
            }

            if (kw != BTOK_ID) {
                /* Keyword — return keyword token, value = keyword name */
                BToken t = make_tok(lex, kw);
                t.sval = id;
                return t;
            }

            /* Regular identifier */
            /* Check if it's a label */
            if (is_label(lex, tok_pos, BTOK_ID, id)) {
                return make_str_tok(lex, BTOK_LABEL, id);
            }

            /* Check if it's an array variable */
            AstNode *entry = symboltable_get_entry(lex->cs->symbol_table, id);
            if (entry && entry->tag == AST_ID && entry->u.id.class_ == CLASS_array) {
                return make_str_tok(lex, BTOK_ARRAY_ID, id);
            }

            return make_str_tok(lex, BTOK_ID, id);
        }

        /* Unknown character */
        advance(lex);
        zxbc_error(lex->cs, lex->lineno, "ignoring illegal character '%c'", c);
        return make_tok(lex, BTOK_ERROR);
    }

    return make_tok(lex, BTOK_EOF);
}

/* ----------------------------------------------------------------
 * Main lexer entry point
 * ---------------------------------------------------------------- */
BToken blexer_next(BLexer *lex) {
    switch (lex->state) {
        case BLEXST_INITIAL:  return lex_initial(lex);
        case BLEXST_STRING:   return lex_string(lex);
        case BLEXST_ASM:      return lex_asm(lex);
        case BLEXST_PREPROC:  return lex_preproc(lex);
        case BLEXST_COMMENT:  return lex_comment(lex);
        case BLEXST_BIN:      return lex_bin_state(lex);
        default:              return make_tok(lex, BTOK_EOF);
    }
}
