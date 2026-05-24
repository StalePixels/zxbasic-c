/*
 * ply_lexdump_harness.c — standalone SINGLE-PASS C lexer token dump.
 *
 * Phase A (lexer token parity). Drives the production lexer (lexer.c) directly
 * — NO parser, so NO backtracking/re-lexing and NO parser-driven symbol-table
 * mutation. With a fresh (empty) symbol table, the lexer's symbol-table-
 * coupled ID/ARRAY_ID resolution never fires (get_entry == None → stays ID),
 * exactly as PLY's lexer behaves on a fresh SYMBOL_TABLE. This isolates the
 * PURE lexical behaviour (token type, value, lineno) for byte-for-byte
 * comparison against csrc/scripts/ply_lexdump_ref.py.
 *
 * (The parse-timing-DEPENDENT cases — ARRAY_ID, label-at-line-start that
 * depends on later state — are validated in Phase E when the PLY engine drives
 * the lexer with PLY's exact reduce timing.)
 *
 * Output format (matches ply_lexdump_ref.py / ply_tokdump.py):
 *   <ply-term-id>\t<TYPE>\t<lineno>\t<value-repr>
 *
 * Usage: ply_lexdump_harness <file.bas>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zxbc.h"
#include "lexer.h"
#include "ply_tables.h"

void compiler_init(CompilerState *cs);
void z80h_pyfloat_repr(double v, char *buf, int sz);

/* Preprocessor-state tracking for the BToken->PlySym ADAPTER translation
 * (the same translation the Phase E engine adapter applies). PLY's lexer has a
 * distinct `preproc` state entered after a directive (_PRAGMA/_INIT/_REQUIRE)
 * and left at NEWLINE; in that state numbers lex as INTEGER and strings as
 * STRING (zxblex t_preproc_INTEGER/STRING), NOT the BASIC NUMBER/STRC the
 * shared C lexer emits (the production parser relies on NUMBER/STRC there —
 * parser.c:4721-4727 — so the shared lexer is NOT changed; the adapter
 * translates instead). */
static int g_in_preproc = 0;

static void emit(FILE *out, const BToken *t) {
    const char *type;
    int id;
    if (t->type == BTOK_EOF) {
        fprintf(out, "%d\t$end\t%d\t\n", PLY_END_ID, t->lineno);
        return;
    }
    type = btok_name(t->type);
    id = ply_term_id(type);

    /* Adapter: enter preproc on a directive token; leave on NEWLINE. */
    int was_preproc = g_in_preproc;
    if (t->type == BTOK__PRAGMA || t->type == BTOK__INIT ||
        t->type == BTOK__REQUIRE || t->type == BTOK__LINE ||
        t->type == BTOK__PUSH || t->type == BTOK__POP)
        g_in_preproc = 1;
    else if (t->type == BTOK_NEWLINE)
        g_in_preproc = 0;

    /* In preproc context, NUMBER->INTEGER (value = raw digit string) and
     * STRC->STRING (zxblex preproc sub-lexer terminals). */
    if (was_preproc && t->type == BTOK_NUMBER) {
        type = "INTEGER";
        id = ply_term_id("INTEGER");
        fprintf(out, "%d\t%s\t%d\t'%s'\n", id, type, t->lineno,
                t->sval ? t->sval : "");
        return;
    }
    if (was_preproc && t->type == BTOK_STRC) {
        type = "STRING";
        id = ply_term_id("STRING");
        fprintf(out, "%d\t%s\t%d\t'%s'\n", id, type, t->lineno,
                t->sval ? t->sval : "");
        return;
    }

    /* NEWLINE faithfulness: PLY captures token.lineno before the increment,
     * the C lexer after — differ by 1 (see parser.c tokdump_emit). */
    int out_lineno = (t->type == BTOK_NEWLINE) ? t->lineno - 1 : t->lineno;

    char buf[128];
    if (t->type == BTOK_NUMBER) {
        /* PLY: decimal literals are float (3.0); hex/bin literals are int
         * (170). The C BToken does not record the source radix, so render
         * the float repr; integer-valued hex/bin literals will show 'N.0'
         * here — a known value-repr-only gap (token TYPE + value magnitude
         * are correct), tracked for Phase C p_error rendering. */
        z80h_pyfloat_repr(t->numval, buf, (int)sizeof(buf));
        fprintf(out, "%d\t%s\t%d\t%s\n", id, type, out_lineno, buf);
        return;
    }

    const char *val;
    char vbuf[128];
    if (t->type == BTOK_NEWLINE) {
        val = "\n";
    } else if (t->sval && (t->type == BTOK_ID || t->type == BTOK_ARRAY_ID ||
                           t->type == BTOK_LABEL || t->type == BTOK_STRC ||
                           t->type == BTOK_ASM)) {
        val = t->sval;
    } else {
        /* keyword/punctuation: value == lexeme/upper-keyword. Reuse the
         * lexer's own name for keywords; punctuation handled by a small map. */
        switch (t->type) {
            case BTOK_PLUS: val = "+"; break;
            case BTOK_MINUS: val = "-"; break;
            case BTOK_MUL: val = "*"; break;
            case BTOK_DIV: val = "/"; break;
            case BTOK_POW: val = "^"; break;
            case BTOK_LP: val = "("; break;
            case BTOK_RP: val = ")"; break;
            case BTOK_LBRACE: val = "{"; break;
            case BTOK_RBRACE: val = "}"; break;
            case BTOK_EQ: val = "="; break;
            case BTOK_LT: val = "<"; break;
            case BTOK_GT: val = ">"; break;
            case BTOK_LE: val = "<="; break;
            case BTOK_GE: val = ">="; break;
            case BTOK_NE: val = "<>"; break;
            case BTOK_WEQ: val = ":="; break;
            case BTOK_CO: val = ":"; break;
            case BTOK_SC: val = ";"; break;
            case BTOK_COMMA: val = ","; break;
            case BTOK_RIGHTARROW: val = "=>"; break;
            case BTOK_ADDRESSOF: val = "@"; break;
            case BTOK_SHL: val = "<<"; break;
            case BTOK_SHR: val = ">>"; break;
            case BTOK_BAND: val = "&"; break;
            case BTOK_BOR: val = "|"; break;
            case BTOK_BXOR: val = "~"; break;
            case BTOK_BNOT: val = "!"; break;
            case BTOK_ERROR:
                val = t->sval ? t->sval : (t->text ? t->text : "");
                break;
            default:
                strncpy(vbuf, type, sizeof(vbuf) - 1);
                vbuf[sizeof(vbuf) - 1] = 0;
                val = vbuf;
                break;
        }
    }
    fprintf(out, "%d\t%s\t%d\t'", id, type, out_lineno);
    for (const char *s = val; *s; s++) {
        if (*s == '\n') fputs("\\n", out);
        else fputc(*s, out);
    }
    fputs("'\n", out);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.bas>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc(n + 1);
    if (fread(src, 1, n, f) != (size_t)n) { /* best effort */ }
    src[n] = 0;
    fclose(f);

    CompilerState cs;
    compiler_init(&cs);
    cs.current_file = argv[1];

    BLexer lex;
    blexer_init(&lex, &cs, src);
    for (;;) {
        BToken t = blexer_next(&lex);
        emit(stdout, &t);
        if (t.type == BTOK_EOF) break;
    }
    free(src);
    return 0;
}
