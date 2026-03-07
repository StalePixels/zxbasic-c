/*
 * Lexer for the Z80 assembler.
 * Tokenizes preprocessed ASM input.
 * Mirrors src/zxbasm/asmlex.py
 */
#include "zxbasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ----------------------------------------------------------------
 * Keyword lookup
 * ---------------------------------------------------------------- */
typedef struct Keyword {
    const char *name;     /* lowercase */
    TokenType type;
} Keyword;

static const Keyword instructions[] = {
    {"adc", TOK_ADC}, {"add", TOK_ADD}, {"and", TOK_AND}, {"bit", TOK_BIT},
    {"call", TOK_CALL}, {"ccf", TOK_CCF}, {"cp", TOK_CP}, {"cpd", TOK_CPD},
    {"cpdr", TOK_CPDR}, {"cpi", TOK_CPI}, {"cpir", TOK_CPIR}, {"cpl", TOK_CPL},
    {"daa", TOK_DAA}, {"dec", TOK_DEC}, {"di", TOK_DI}, {"djnz", TOK_DJNZ},
    {"ei", TOK_EI}, {"ex", TOK_EX}, {"exx", TOK_EXX}, {"halt", TOK_HALT},
    {"im", TOK_IM}, {"in", TOK_IN}, {"inc", TOK_INC}, {"ind", TOK_IND},
    {"indr", TOK_INDR}, {"ini", TOK_INI}, {"inir", TOK_INIR}, {"jp", TOK_JP},
    {"jr", TOK_JR}, {"ld", TOK_LD}, {"ldd", TOK_LDD}, {"lddr", TOK_LDDR},
    {"ldi", TOK_LDI}, {"ldir", TOK_LDIR}, {"neg", TOK_NEG}, {"nop", TOK_NOP},
    {"or", TOK_OR}, {"otdr", TOK_OTDR}, {"otir", TOK_OTIR}, {"out", TOK_OUT},
    {"outd", TOK_OUTD}, {"outi", TOK_OUTI}, {"pop", TOK_POP}, {"push", TOK_PUSH},
    {"res", TOK_RES}, {"ret", TOK_RET}, {"reti", TOK_RETI}, {"retn", TOK_RETN},
    {"rl", TOK_RL}, {"rla", TOK_RLA}, {"rlc", TOK_RLC}, {"rlca", TOK_RLCA},
    {"rld", TOK_RLD}, {"rr", TOK_RR}, {"rra", TOK_RRA}, {"rrc", TOK_RRC},
    {"rrca", TOK_RRCA}, {"rrd", TOK_RRD}, {"rst", TOK_RST}, {"sbc", TOK_SBC},
    {"scf", TOK_SCF}, {"set", TOK_SET}, {"sla", TOK_SLA}, {"sll", TOK_SLL},
    {"sra", TOK_SRA}, {"srl", TOK_SRL}, {"sub", TOK_SUB}, {"xor", TOK_XOR},
    {NULL, TOK_EOF}
};

static const Keyword zxnext_instructions[] = {
    {"ldix", TOK_LDIX}, {"ldws", TOK_LDWS}, {"ldirx", TOK_LDIRX},
    {"lddx", TOK_LDDX}, {"lddrx", TOK_LDDRX}, {"ldpirx", TOK_LDPIRX},
    {"outinb", TOK_OUTINB}, {"mul", TOK_MUL_INSTR}, {"swapnib", TOK_SWAPNIB},
    {"mirror", TOK_MIRROR_INSTR}, {"nextreg", TOK_NEXTREG},
    {"pixeldn", TOK_PIXELDN}, {"pixelad", TOK_PIXELAD}, {"setae", TOK_SETAE},
    {"test", TOK_TEST}, {"bsla", TOK_BSLA}, {"bsra", TOK_BSRA},
    {"bsrl", TOK_BSRL}, {"bsrf", TOK_BSRF}, {"brlc", TOK_BRLC},
    {NULL, TOK_EOF}
};

static const Keyword pseudo_ops[] = {
    {"align", TOK_ALIGN}, {"org", TOK_ORG}, {"defb", TOK_DEFB},
    {"defm", TOK_DEFB}, {"db", TOK_DEFB}, {"defs", TOK_DEFS},
    {"defw", TOK_DEFW}, {"ds", TOK_DEFS}, {"dw", TOK_DEFW},
    {"equ", TOK_EQU}, {"proc", TOK_PROC}, {"endp", TOK_ENDP},
    {"local", TOK_LOCAL}, {"end", TOK_END}, {"incbin", TOK_INCBIN},
    {"namespace", TOK_NAMESPACE},
    {NULL, TOK_EOF}
};

static const Keyword regs8[] = {
    {"a", TOK_A}, {"b", TOK_B}, {"c", TOK_C}, {"d", TOK_D}, {"e", TOK_E},
    {"h", TOK_H}, {"l", TOK_L}, {"i", TOK_I}, {"r", TOK_R},
    {"ixh", TOK_IXH}, {"ixl", TOK_IXL}, {"iyh", TOK_IYH}, {"iyl", TOK_IYL},
    {NULL, TOK_EOF}
};

static const Keyword regs16[] = {
    {"af", TOK_AF}, {"bc", TOK_BC}, {"de", TOK_DE}, {"hl", TOK_HL},
    {"ix", TOK_IX}, {"iy", TOK_IY}, {"sp", TOK_SP},
    {NULL, TOK_EOF}
};

static const Keyword flags[] = {
    {"z", TOK_Z}, {"nz", TOK_NZ}, {"nc", TOK_NC},
    {"po", TOK_PO}, {"pe", TOK_PE}, {"p", TOK_P}, {"m", TOK_M},
    {NULL, TOK_EOF}
};

static const Keyword preproc_kw[] = {
    {"init", TOK_INIT},
    {NULL, TOK_EOF}
};

static TokenType lookup_keyword(const char *id_lower, bool zxnext)
{
    for (const Keyword *k = instructions; k->name; k++) {
        if (strcmp(id_lower, k->name) == 0) return k->type;
    }
    for (const Keyword *k = pseudo_ops; k->name; k++) {
        if (strcmp(id_lower, k->name) == 0) return k->type;
    }
    for (const Keyword *k = regs8; k->name; k++) {
        if (strcmp(id_lower, k->name) == 0) return k->type;
    }
    for (const Keyword *k = flags; k->name; k++) {
        if (strcmp(id_lower, k->name) == 0) return k->type;
    }
    if (zxnext) {
        for (const Keyword *k = zxnext_instructions; k->name; k++) {
            if (strcmp(id_lower, k->name) == 0) return k->type;
        }
    }
    for (const Keyword *k = regs16; k->name; k++) {
        if (strcmp(id_lower, k->name) == 0) return k->type;
    }
    return TOK_ID;
}

/* ----------------------------------------------------------------
 * Lexer implementation
 * ---------------------------------------------------------------- */
void lexer_init(Lexer *lex, AsmState *as, const char *input)
{
    lex->as = as;
    lex->input = input;
    lex->pos = 0;
    lex->lineno = 1;
    lex->in_preproc = false;
}

static char lexer_peek(Lexer *lex)
{
    return lex->input[lex->pos];
}

static char lexer_advance(Lexer *lex)
{
    return lex->input[lex->pos++];
}

static bool lexer_eof(Lexer *lex)
{
    return lex->input[lex->pos] == '\0';
}

/* Compute column (1-based) of position p */
static int find_column(Lexer *lex, int p)
{
    int i = p;
    while (i > 0 && lex->input[i - 1] != '\n') i--;
    return p - i + 1;
}

Token lexer_next(Lexer *lex)
{
    Token tok;
    memset(&tok, 0, sizeof(tok));
    tok.lineno = lex->lineno;

    while (!lexer_eof(lex)) {
        char c = lexer_peek(lex);

        /* Skip whitespace (not newline) */
        if (c == ' ' || c == '\t') {
            lexer_advance(lex);
            continue;
        }

        tok.lineno = lex->lineno;

        /* Line continuation */
        if (c == '\\' && lex->input[lex->pos + 1] &&
            (lex->input[lex->pos + 1] == '\n' ||
             (lex->input[lex->pos + 1] == '\r' && lex->input[lex->pos + 2] == '\n'))) {
            lexer_advance(lex); /* skip \ */
            if (lexer_peek(lex) == '\r') lexer_advance(lex);
            lexer_advance(lex); /* skip \n */
            lex->lineno++;
            continue;
        }

        /* Newline */
        if (c == '\n' || c == '\r') {
            if (c == '\r' && lex->input[lex->pos + 1] == '\n') {
                lex->pos += 2;
            } else {
                lex->pos++;
            }
            lex->lineno++;
            lex->in_preproc = false;
            tok.type = TOK_NEWLINE;
            return tok;
        }

        /* Comment: ; to end of line */
        if (c == ';') {
            while (!lexer_eof(lex) && lexer_peek(lex) != '\n' && lexer_peek(lex) != '\r')
                lexer_advance(lex);
            continue;
        }

        /* Character literal: 'x' */
        if (c == '\'' && lex->input[lex->pos + 1] && lex->input[lex->pos + 2] == '\'') {
            lexer_advance(lex); /* skip ' */
            tok.type = TOK_INTEGER;
            tok.ival = (unsigned char)lexer_advance(lex);
            lexer_advance(lex); /* skip ' */
            return tok;
        }

        /* Apostrophe (for EX AF,AF') */
        if (c == '\'') {
            lexer_advance(lex);
            tok.type = TOK_APO;
            return tok;
        }

        /* String literal */
        if (c == '"') {
            lexer_advance(lex); /* skip opening " */
            StrBuf sb;
            strbuf_init(&sb);
            while (!lexer_eof(lex) && lexer_peek(lex) != '\n') {
                if (lexer_peek(lex) == '"') {
                    if (lex->input[lex->pos + 1] == '"') {
                        /* Escaped double quote */
                        strbuf_append_char(&sb, '"');
                        lex->pos += 2;
                    } else {
                        lexer_advance(lex); /* skip closing " */
                        break;
                    }
                } else {
                    strbuf_append_char(&sb, lexer_advance(lex));
                }
            }
            tok.type = TOK_STRING;
            tok.sval = arena_strdup(&lex->as->arena, strbuf_cstr(&sb));
            strbuf_free(&sb);
            return tok;
        }

        /* Hex number: $XX or 0xXX or XXh */
        if (c == '$' && lex->input[lex->pos + 1] &&
            isxdigit((unsigned char)lex->input[lex->pos + 1])) {
            lexer_advance(lex); /* skip $ */
            StrBuf sb;
            strbuf_init(&sb);
            while (!lexer_eof(lex) &&
                   (isxdigit((unsigned char)lexer_peek(lex)) || lexer_peek(lex) == '_')) {
                if (lexer_peek(lex) != '_')
                    strbuf_append_char(&sb, lexer_advance(lex));
                else
                    lexer_advance(lex);
            }
            tok.type = TOK_INTEGER;
            tok.ival = (int64_t)strtoll(strbuf_cstr(&sb), NULL, 16);
            strbuf_free(&sb);
            return tok;
        }

        /* 0x prefix hex */
        if (c == '0' && (lex->input[lex->pos + 1] == 'x' || lex->input[lex->pos + 1] == 'X')) {
            lex->pos += 2;
            StrBuf sb;
            strbuf_init(&sb);
            while (!lexer_eof(lex) &&
                   (isxdigit((unsigned char)lexer_peek(lex)) || lexer_peek(lex) == '_')) {
                if (lexer_peek(lex) != '_')
                    strbuf_append_char(&sb, lexer_advance(lex));
                else
                    lexer_advance(lex);
            }
            tok.type = TOK_INTEGER;
            tok.ival = (int64_t)strtoll(strbuf_cstr(&sb), NULL, 16);
            strbuf_free(&sb);
            return tok;
        }

        /* 0b prefix binary */
        if (c == '0' && (lex->input[lex->pos + 1] == 'b' || lex->input[lex->pos + 1] == 'B')
            && (lex->input[lex->pos + 2] == '0' || lex->input[lex->pos + 2] == '1')) {
            lex->pos += 2;
            StrBuf sb;
            strbuf_init(&sb);
            while (!lexer_eof(lex) &&
                   (lexer_peek(lex) == '0' || lexer_peek(lex) == '1' || lexer_peek(lex) == '_')) {
                if (lexer_peek(lex) != '_')
                    strbuf_append_char(&sb, lexer_advance(lex));
                else
                    lexer_advance(lex);
            }
            tok.type = TOK_INTEGER;
            tok.ival = (int64_t)strtoll(strbuf_cstr(&sb), NULL, 2);
            strbuf_free(&sb);
            return tok;
        }

        /* %binary */
        if (c == '%' && lex->input[lex->pos + 1] &&
            (lex->input[lex->pos + 1] == '0' || lex->input[lex->pos + 1] == '1')) {
            lexer_advance(lex); /* skip % */
            StrBuf sb;
            strbuf_init(&sb);
            while (!lexer_eof(lex) &&
                   (lexer_peek(lex) == '0' || lexer_peek(lex) == '1' || lexer_peek(lex) == '_')) {
                if (lexer_peek(lex) != '_')
                    strbuf_append_char(&sb, lexer_advance(lex));
                else
                    lexer_advance(lex);
            }
            tok.type = TOK_INTEGER;
            tok.ival = (int64_t)strtoll(strbuf_cstr(&sb), NULL, 2);
            strbuf_free(&sb);
            return tok;
        }

        /* Number: decimal, or hex with trailing 'h', or temp label nF/nB */
        if (isdigit((unsigned char)c)) {
            StrBuf sb;
            strbuf_init(&sb);
            strbuf_append_char(&sb, lexer_advance(lex));

            /* Collect digits and underscores and hex chars */
            while (!lexer_eof(lex) &&
                   (isxdigit((unsigned char)lexer_peek(lex)) || lexer_peek(lex) == '_')) {
                if (lexer_peek(lex) != '_')
                    strbuf_append_char(&sb, lexer_advance(lex));
                else
                    lexer_advance(lex);
            }

            const char *numstr = strbuf_cstr(&sb);
            size_t numlen = strlen(numstr);

            /* Check for trailing 'h' or 'H' (hex) */
            if (numlen > 0 && (numstr[numlen - 1] == 'h' || numstr[numlen - 1] == 'H')) {
                /* Hex number with h suffix */
                char *hex = arena_strndup(&lex->as->arena, numstr, numlen - 1);
                tok.type = TOK_INTEGER;
                tok.ival = (int64_t)strtoll(hex, NULL, 16);
                strbuf_free(&sb);
                return tok;
            }

            /* Check for trailing 'b' or 'B' — could be binary or temp label */
            if (numlen > 0 && (numstr[numlen - 1] == 'b' || numstr[numlen - 1] == 'B')) {
                /* Check if all preceding chars are 0/1 — then binary */
                bool is_bin = true;
                for (size_t i = 0; i < numlen - 1; i++) {
                    if (numstr[i] != '0' && numstr[i] != '1') {
                        is_bin = false;
                        break;
                    }
                }
                if (is_bin && numlen > 1) {
                    /* Binary number */
                    char *bin = arena_strndup(&lex->as->arena, numstr, numlen - 1);
                    tok.type = TOK_INTEGER;
                    tok.ival = (int64_t)strtoll(bin, NULL, 2);
                    strbuf_free(&sb);
                    return tok;
                }
                /* Otherwise it's a temporary label reference like "1B" */
                tok.type = TOK_ID;
                /* Uppercase the direction char */
                char *id = arena_strdup(&lex->as->arena, numstr);
                id[numlen - 1] = (char)toupper((unsigned char)id[numlen - 1]);
                tok.sval = id;
                tok.original_id = tok.sval;
                strbuf_free(&sb);
                return tok;
            }

            /* Check for trailing 'f' or 'F' — temp label forward ref */
            if (!lexer_eof(lex) &&
                (lexer_peek(lex) == 'f' || lexer_peek(lex) == 'F')) {
                strbuf_append_char(&sb, (char)toupper((unsigned char)lexer_advance(lex)));
                tok.type = TOK_ID;
                tok.sval = arena_strdup(&lex->as->arena, strbuf_cstr(&sb));
                tok.original_id = tok.sval;
                strbuf_free(&sb);
                return tok;
            }

            /* Plain decimal integer */
            tok.type = TOK_INTEGER;
            tok.ival = (int64_t)strtoll(numstr, NULL, 10);
            strbuf_free(&sb);
            return tok;
        }

        /* Identifier: [._a-zA-Z][._a-zA-Z0-9]* */
        if (c == '_' || c == '.' || isalpha((unsigned char)c)) {
            StrBuf sb;
            strbuf_init(&sb);
            strbuf_append_char(&sb, lexer_advance(lex));
            while (!lexer_eof(lex) &&
                   (lexer_peek(lex) == '_' || lexer_peek(lex) == '.' ||
                    isalnum((unsigned char)lexer_peek(lex)))) {
                strbuf_append_char(&sb, lexer_advance(lex));
            }

            const char *id_original = strbuf_cstr(&sb);

            /* Make lowercase copy for keyword lookup */
            char *id_lower = arena_strdup(&lex->as->arena, id_original);
            for (char *p = id_lower; *p; p++) *p = (char)tolower((unsigned char)*p);

            TokenType kw_type;
            if (lex->in_preproc) {
                /* In preprocessor directive context */
                kw_type = TOK_ID;
                for (const Keyword *k = preproc_kw; k->name; k++) {
                    if (strcmp(id_lower, k->name) == 0) {
                        kw_type = k->type;
                        break;
                    }
                }
            } else {
                kw_type = lookup_keyword(id_lower, lex->as->zxnext);
            }

            tok.type = kw_type;
            if (kw_type == TOK_ID) {
                /* Keep original case for identifiers */
                tok.sval = arena_strdup(&lex->as->arena, id_original);
                tok.original_id = tok.sval;
            } else {
                /* For keywords, store uppercase (matching Python behavior) */
                char *id_upper = arena_strdup(&lex->as->arena, id_original);
                for (char *p = id_upper; *p; p++) *p = (char)toupper((unsigned char)*p);
                tok.sval = id_upper;
                tok.original_id = arena_strdup(&lex->as->arena, id_original);
            }

            strbuf_free(&sb);
            return tok;
        }

        /* Single-char tokens */
        lexer_advance(lex);
        switch (c) {
        case ':': tok.type = TOK_COLON; return tok;
        case ',': tok.type = TOK_COMMA; return tok;
        case '+': tok.type = TOK_PLUS; return tok;
        case '-': tok.type = TOK_MINUS; return tok;
        case '*': tok.type = TOK_MUL; return tok;
        case '/': tok.type = TOK_DIV; return tok;
        case '%': tok.type = TOK_MOD; return tok;
        case '^': tok.type = TOK_POW; return tok;
        case '&': tok.type = TOK_BAND; return tok;
        case '|': tok.type = TOK_BOR; return tok;
        case '~': tok.type = TOK_BXOR; return tok;
        case '(': tok.type = TOK_LP; return tok;
        case ')': tok.type = TOK_RP; return tok;
        case '[': tok.type = TOK_LB; return tok;
        case ']': tok.type = TOK_RB; return tok;
        case '$': tok.type = TOK_ADDR; return tok;
        case '<':
            if (!lexer_eof(lex) && lexer_peek(lex) == '<') {
                lexer_advance(lex);
                tok.type = TOK_LSHIFT;
            } else {
                asm_error(lex->as, lex->lineno, "illegal character '<'");
                continue;
            }
            return tok;
        case '>':
            if (!lexer_eof(lex) && lexer_peek(lex) == '>') {
                lexer_advance(lex);
                tok.type = TOK_RSHIFT;
            } else {
                asm_error(lex->as, lex->lineno, "illegal character '>'");
                continue;
            }
            return tok;
        case '#':
            /* Preprocessor directive (#line from preprocessor output,
             * or #init) */
            if (find_column(lex, lex->pos - 1) == 1) {
                lex->in_preproc = true;
                /* Skip whitespace */
                while (!lexer_eof(lex) && (lexer_peek(lex) == ' ' || lexer_peek(lex) == '\t'))
                    lexer_advance(lex);

                /* Check for "line" keyword */
                if (strncasecmp(&lex->input[lex->pos], "line", 4) == 0 &&
                    !isalnum((unsigned char)lex->input[lex->pos + 4]) &&
                    lex->input[lex->pos + 4] != '_') {
                    /* #line N "filename" */
                    lex->pos += 4;
                    while (!lexer_eof(lex) && (lexer_peek(lex) == ' ' || lexer_peek(lex) == '\t'))
                        lexer_advance(lex);
                    /* Parse line number */
                    int new_line = 0;
                    while (!lexer_eof(lex) && isdigit((unsigned char)lexer_peek(lex))) {
                        new_line = new_line * 10 + (lexer_advance(lex) - '0');
                    }
                    while (!lexer_eof(lex) && (lexer_peek(lex) == ' ' || lexer_peek(lex) == '\t'))
                        lexer_advance(lex);
                    /* Optional filename */
                    if (!lexer_eof(lex) && lexer_peek(lex) == '"') {
                        lexer_advance(lex);
                        StrBuf fn;
                        strbuf_init(&fn);
                        while (!lexer_eof(lex) && lexer_peek(lex) != '"' &&
                               lexer_peek(lex) != '\n') {
                            if (lexer_peek(lex) == '"' && lex->input[lex->pos + 1] == '"') {
                                strbuf_append_char(&fn, '"');
                                lex->pos += 2;
                            } else {
                                strbuf_append_char(&fn, lexer_advance(lex));
                            }
                        }
                        if (!lexer_eof(lex) && lexer_peek(lex) == '"')
                            lexer_advance(lex);
                        lex->as->current_file = arena_strdup(&lex->as->arena, strbuf_cstr(&fn));
                        strbuf_free(&fn);
                    }
                    lex->lineno = new_line;
                    /* Skip to end of line */
                    while (!lexer_eof(lex) && lexer_peek(lex) != '\n' && lexer_peek(lex) != '\r')
                        lexer_advance(lex);
                    lex->in_preproc = false;
                    continue;
                }
                /* Not #line — could be #init or other preprocessor directive */
                /* Return next token in preproc mode */
                continue;
            }
            asm_error(lex->as, lex->lineno, "illegal character '#'");
            continue;

        default:
            asm_error(lex->as, lex->lineno, "illegal character '%c'", c);
            continue;
        }
    }

    tok.type = TOK_EOF;
    tok.lineno = lex->lineno;
    return tok;
}
