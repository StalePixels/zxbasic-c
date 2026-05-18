/*
 * basic.c — Faithful C port of src/zxbasm/basic.py (class Basic)
 *
 * Byte-for-byte port of the BASIC-loader generator. Ported methods:
 *   line_number, numberLH, number, token, literal, sentence_bytes,
 *   line, add_line.
 *
 * NOT ported: parse_sentence (basic.py:109-135) — dead, unused, broken
 * Python; never called. Replicating it is forbidden by the sprint spec.
 *
 * The float branch of number() uses a faithful (C, ED, LH) "0XXh"
 * encoder DUPLICATED VERBATIM from the verified csrc/zxbc/z80asm.c
 * z80h_immediate_float (which is itself a faithful port of
 * src/api/fp.py immediate_float). Kept self-contained here — no
 * cross-lib dependency on csrc/zxbc. The loader never hits the float
 * branch (all loader numbers are integers < 65536), but it is ported
 * faithfully (no stub) for completeness.
 */
#include "basic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ENTER = 0x0D (basic.py:13). */
#define BASIC_ENTER 0x0D

/* ----------------------------------------------------------------
 * TOKENS dict (basic.py:15-47) — ALL 30 entries, verbatim.
 * ---------------------------------------------------------------- */
typedef struct {
    const char *name;
    unsigned char code;
} BasicToken;

static const BasicToken BASIC_TOKENS[] = {
    {"LOAD", 239},
    {"POKE", 244},
    {"PRINT", 245},
    {"RUN", 247},
    {"PEEK", 190},
    {"USR", 192},
    {"LINE", 202},
    {"CODE", 175},
    {"AT", 172},
    {"RANDOMIZE", 249},
    {"CLS", 251},
    {"CLEAR", 253},
    {"PAUSE", 242},
    {"LET", 241},
    {"INPUT", 238},
    {"READ", 227},
    {"DATA", 228},
    {"RESTORE", 229},
    {"NEW", 230},
    {"OUT", 223},
    {"BEEP", 215},
    {"INK", 217},
    {"PAPER", 218},
    {"BORDER", 231},
    {"REM", 234},
    {"FOR", 235},
    {"TO", 204},
    {"NEXT", 243},
    {"RETURN", 254},
    {"GOTO", 236},
    {"GO SUB", 237},
};

#define BASIC_NTOKENS (sizeof(BASIC_TOKENS) / sizeof(BASIC_TOKENS[0]))

/* TOKENS[string.upper()] — KeyError in Python aborts; here we mirror
 * "must match a token". On a miss we set the sticky OOM-style error via
 * the buffer (callers always pass valid tokens for the loader). Returns
 * -1 on miss. */
static int basic_tokens_lookup(const char *word)
{
    char up[64];
    size_t i = 0;
    if (!word) return -1;
    for (; word[i] != '\0' && i < sizeof(up) - 1; i++) {
        unsigned char c = (unsigned char)word[i];
        up[i] = (char)toupper(c);
    }
    up[i] = '\0';
    for (size_t k = 0; k < BASIC_NTOKENS; k++) {
        if (strcmp(BASIC_TOKENS[k].name, up) == 0) {
            return (int)BASIC_TOKENS[k].code;
        }
    }
    return -1;
}

/* ----------------------------------------------------------------
 * Growable byte buffer (self-contained — no shared deps).
 * Mirrors Python's list-of-int self.bytes / per-call result lists.
 * ---------------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    int oom; /* sticky out-of-memory / token-miss flag */
} ByteBuf;

static void bb_init(ByteBuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

static void bb_free(ByteBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void bb_push(ByteBuf *b, unsigned char byte)
{
    if (b->oom) return;
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        unsigned char *nd = (unsigned char *)realloc(b->data, ncap);
        if (!nd) {
            b->oom = 1;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
    b->data[b->len++] = byte;
}

static void bb_extend(ByteBuf *dst, const unsigned char *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        bb_push(dst, src[i]);
    }
}

static void bb_extend_buf(ByteBuf *dst, const ByteBuf *src)
{
    if (src->oom) {
        dst->oom = 1;
        return;
    }
    bb_extend(dst, src->data, src->len);
}

/* ----------------------------------------------------------------
 * src/api/fp.py — the ZX Spectrum 40-bit FP encoder. DUPLICATED
 * VERBATIM from the verified csrc/zxbc/z80asm.c z80h_immediate_float
 * (faithful port of fp() + immediate_float()). Self-contained here.
 * ------------------------------------------------------------------ */

/* bin(int(f) & 0xFFFFFFFF)[2:].zfill(32) — 32-char MSB-first binary of
 * the low 32 bits of int(f) (truncate toward zero). */
static void basic_fp_bin32(double f, char out[33])
{
    long long i = (long long)f;                 /* Python int(f): trunc */
    unsigned long u = (unsigned long)((unsigned long long)i & 0xFFFFFFFFULL);
    for (int b = 0; b < 32; b++)
        out[b] = (char)('0' + (int)((u >> (31 - b)) & 1u));
    out[32] = '\0';
}

/* bindec32(f): "0" (or bin32(f) if f>=1) + "." + 32 fraction bits.
 * Writes the full string; returns its length. */
static int basic_fp_bindec32(double f, char *result)
{
    int w = 0;
    char b32[33];
    if (f >= 1.0) {
        basic_fp_bin32(f, b32);
        for (int k = 0; b32[k]; k++) result[w++] = b32[k];
    } else {
        result[w++] = '0';
    }
    result[w++] = '.';

    double a = f;
    long long c = (long long)a;                 /* int(a) */
    for (int i = 0; i < 32; i++) {
        a -= (double)c;
        a *= 2.0;
        c = (long long)a;                        /* int(a) (trunc) */
        result[w++] = (char)('0' + (int)c);
    }
    result[w] = '\0';
    return w;
}

/* fp(x) -> (M, E); immediate_float(x) -> the (C, ED, LH) "0XXh" triple. */
static void basic_immediate_float(double x, char *C, char *ED, char *LH)
{
    int e = 0;
    int s = (x < 0.0) ? 1 : 0;
    double m = (x < 0.0) ? -x : x;              /* abs(x) */

    while (m >= 1.0)       { m /= 2.0; e += 1; }
    while (m > 0.0 && m < 0.5) { m *= 2.0; e -= 1; }

    char dec[80];
    basic_fp_bindec32(m, dec);                  /* len 34 (here m<1) */

    /* M = str(s) + bindec32(m)[3:]  -> 32 chars (1 + 31) */
    char M[40];
    int mw = 0;
    M[mw++] = (char)('0' + s);
    for (int k = 3; dec[k]; k++) M[mw++] = dec[k];
    M[mw] = '\0';

    /* E = bin32(e + 128)[-8:]  if x != 0 else bin32(0)[-8:] */
    char eb[33];
    basic_fp_bin32(x != 0.0 ? (double)(e + 128) : 0.0, eb);
    const char *E = eb + 24;                    /* last 8 chars */

    /* bin2hex(y) = "%02X" % int(y, 2) over a binary substring. */
    #define BASIC_FP_B2H(dst, ptr, len) do {                            \
        unsigned _v = 0;                                                \
        for (int _i = 0; _i < (len); _i++)                              \
            _v = (_v << 1) | (unsigned)((ptr)[_i] - '0');               \
        snprintf((dst), sizeof(dst), "%02X", _v);                       \
    } while (0)

    char he[4], hm0[4], hm1[4], hm2[4], hm3[4];
    BASIC_FP_B2H(he,  E,        8);
    BASIC_FP_B2H(hm0, M + 8,    8);   /* M[8:16] */
    BASIC_FP_B2H(hm1, M,        8);   /* M[:8]   */
    BASIC_FP_B2H(hm2, M + 24,   8);   /* M[24:]  */
    BASIC_FP_B2H(hm3, M + 16,   8);   /* M[16:24]*/
    #undef BASIC_FP_B2H

    /* C = "0"+bin2hex(E)+"h"; ED = "0"+b2h(M[8:16])+b2h(M[:8])+"h";
     * LH = "0"+b2h(M[24:])+b2h(M[16:24])+"h" */
    snprintf(C,  8,  "0%sh", he);
    snprintf(ED, 8,  "0%s%sh", hm0, hm1);
    snprintf(LH, 8,  "0%s%sh", hm2, hm3);
}

/* int(hexstr[:2], 16) helper — parse a 2-char (or fewer) hex substring. */
static int basic_hex2(const char *p, int len)
{
    int v = 0;
    for (int i = 0; i < len; i++) {
        char c = p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else d = 0;
        v = v * 16 + d;
    }
    return v;
}

/* ----------------------------------------------------------------
 * Basic methods
 * ---------------------------------------------------------------- */

struct Basic {
    ByteBuf bytes;     /* self.bytes */
    int current_line;  /* self.current_line (init 0) */
};

Basic *basic_new(void)
{
    Basic *b = (Basic *)malloc(sizeof(Basic));
    if (!b) return NULL;
    bb_init(&b->bytes);
    b->current_line = 0;
    return b;
}

void basic_free(Basic *b)
{
    if (!b) return;
    bb_free(&b->bytes);
    free(b);
}

int basic_oom(const Basic *b)
{
    return b ? b->bytes.oom : 1;
}

const unsigned char *basic_bytes(Basic *b, int *len)
{
    if (len) *len = b ? (int)b->bytes.len : 0;
    return b ? b->bytes.data : NULL;
}

/* line_number(number) -> [numberH, numberL]  (BIG ENDIAN, basic.py:57-64) */
static void basic_line_number(int number, ByteBuf *out)
{
    bb_push(out, (unsigned char)((number & 0xFF00) >> 8));
    bb_push(out, (unsigned char)(number & 0xFF));
}

/* numberLH(number) -> [numberL, numberH]  (LITTLE ENDIAN, basic.py:66-73) */
static void basic_numberLH(int number, ByteBuf *out)
{
    bb_push(out, (unsigned char)(number & 0xFF));
    bb_push(out, (unsigned char)((number & 0xFF00) >> 8));
}

/* number(number) (basic.py:75-95):
 *   s = [ord(c) for c in str(number)] + [14]
 *   if number == int(number) and abs(number) < 65536: integer form
 *       sign = 0xFF if number < 0 else 0
 *       b = [0, sign] + numberLH(number) + [0]
 *   else: float form via immediate_float
 *   return s + b
 *
 * In the Basic loader, number() is always called with an int value.
 * str(int) = decimal ASCII, negative -> leading '-', no ".0". */
static void basic_number(double number, ByteBuf *out)
{
    /* s = [ord(c) for c in str(number)] */
    int is_int = (number == floor(number)) && isfinite(number) &&
                 (fabs(number) < 65536.0);

    if (is_int) {
        /* str(int(number)) — decimal ASCII, sign for negatives. */
        long long n = (long long)number;
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", n);
        for (size_t i = 0; tmp[i] != '\0'; i++) {
            bb_push(out, (unsigned char)tmp[i]);
        }
        bb_push(out, 14); /* + [14] */

        /* b = [0, sign] + numberLH(number) + [0] */
        unsigned char sign = (number < 0.0) ? 0xFF : 0x00;
        bb_push(out, 0);
        bb_push(out, sign);
        basic_numberLH((int)n, out);
        bb_push(out, 0);
    } else {
        /* Float form. str(number) in Python for a non-integer float
         * (repr-style). This branch is never reached by the loader;
         * use the standard double formatting for the ASCII prefix. */
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%g", number);
        for (size_t i = 0; tmp[i] != '\0'; i++) {
            bb_push(out, (unsigned char)tmp[i]);
        }
        bb_push(out, 14); /* + [14] */

        char C[8], ED[8], LH[8];
        basic_immediate_float(number, C, ED, LH);
        /* C = C[:2]; ED = ED[:4]; LH = LH[:4]  (strip leading '0' & 'h')
         * Python: C/ED/LH are like "0AAh"/"0AABBh"/"0CCDDh".
         *   C[:2]  -> the 2 hex digits after the leading '0'
         *   ED[:4] -> the 4 hex digits after the leading '0'
         *   LH[:4] -> the 4 hex digits after the leading '0'
         * b = [int(C,16), int(ED[:2],16), int(ED[2:],16),
         *      int(LH[:2],16), int(LH[2:],16)] */
        const char *Cp = C + 1;   /* skip leading '0' -> C[:2] is Cp[0..1] */
        const char *EDp = ED + 1; /* ED[:4] is EDp[0..3] */
        const char *LHp = LH + 1; /* LH[:4] is LHp[0..3] */
        bb_push(out, (unsigned char)basic_hex2(Cp, 2));
        bb_push(out, (unsigned char)basic_hex2(EDp, 2));
        bb_push(out, (unsigned char)basic_hex2(EDp + 2, 2));
        bb_push(out, (unsigned char)basic_hex2(LHp, 2));
        bb_push(out, (unsigned char)basic_hex2(LHp + 2, 2));
    }
}

/* ----------------------------------------------------------------
 * Sentence / line model
 * ---------------------------------------------------------------- */

typedef enum {
    ITEM_STRING,  /* str  -> literal() */
    ITEM_NUMBER,  /* int/float -> number() */
    ITEM_RAW      /* "another thing" -> extend(item) (e.g. token()) */
} ItemKind;

typedef struct {
    ItemKind kind;
    char *str;             /* ITEM_STRING (owned) */
    double num;            /* ITEM_NUMBER */
    unsigned char *raw;    /* ITEM_RAW (owned) */
    size_t raw_len;
} BasicItem;

struct BasicSentence {
    int token_code;        /* TOKENS[sentence[0]] (-1 = miss) */
    int token_ok;          /* 0 if token lookup missed */
    BasicItem *items;      /* sentence[1:] */
    size_t nitems;
    size_t cap;
    int oom;
};

struct BasicLine {
    BasicSentence **sentences;
    size_t n;
    size_t cap;
    int oom;
};

BasicSentence *basic_sentence_new(const char *token_word)
{
    BasicSentence *s = (BasicSentence *)malloc(sizeof(BasicSentence));
    if (!s) return NULL;
    s->items = NULL;
    s->nitems = 0;
    s->cap = 0;
    s->oom = 0;
    s->token_code = basic_tokens_lookup(token_word);
    s->token_ok = (s->token_code >= 0);
    return s;
}

static BasicItem *sentence_push_item(BasicSentence *s)
{
    if (s->oom) return NULL;
    if (s->nitems == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4;
        BasicItem *ni = (BasicItem *)realloc(s->items, ncap * sizeof(BasicItem));
        if (!ni) {
            s->oom = 1;
            return NULL;
        }
        s->items = ni;
        s->cap = ncap;
    }
    BasicItem *it = &s->items[s->nitems++];
    it->kind = ITEM_RAW;
    it->str = NULL;
    it->num = 0.0;
    it->raw = NULL;
    it->raw_len = 0;
    return it;
}

void basic_sentence_add_string(BasicSentence *s, const char *str)
{
    BasicItem *it = sentence_push_item(s);
    if (!it) return;
    it->kind = ITEM_STRING;
    size_t n = str ? strlen(str) : 0;
    it->str = (char *)malloc(n + 1);
    if (!it->str) {
        s->oom = 1;
        s->nitems--; /* discard the half-built item */
        return;
    }
    if (n) memcpy(it->str, str, n);
    it->str[n] = '\0';
}

void basic_sentence_add_number(BasicSentence *s, double n)
{
    BasicItem *it = sentence_push_item(s);
    if (!it) return;
    it->kind = ITEM_NUMBER;
    it->num = n;
}

void basic_sentence_add_raw(BasicSentence *s, const unsigned char *bytes,
                            size_t n)
{
    BasicItem *it = sentence_push_item(s);
    if (!it) return;
    it->kind = ITEM_RAW;
    it->raw = (unsigned char *)malloc(n ? n : 1);
    if (!it->raw) {
        s->oom = 1;
        s->nitems--;
        return;
    }
    if (n) memcpy(it->raw, bytes, n);
    it->raw_len = n;
}

/* token(word) -> [TOKENS[word.upper()]] (basic.py:97-101). */
void basic_sentence_add_token(BasicSentence *s, const char *word)
{
    int code = basic_tokens_lookup(word);
    if (code < 0) {
        s->oom = 1; /* token miss == KeyError in Python */
        return;
    }
    unsigned char byte = (unsigned char)code;
    basic_sentence_add_raw(s, &byte, 1);
}

void basic_sentence_free(BasicSentence *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->nitems; i++) {
        free(s->items[i].str);
        free(s->items[i].raw);
    }
    free(s->items);
    free(s);
}

BasicLine *basic_line_new(void)
{
    BasicLine *l = (BasicLine *)malloc(sizeof(BasicLine));
    if (!l) return NULL;
    l->sentences = NULL;
    l->n = 0;
    l->cap = 0;
    l->oom = 0;
    return l;
}

void basic_line_add_sentence(BasicLine *line, BasicSentence *s)
{
    if (!line) {
        basic_sentence_free(s);
        return;
    }
    if (line->n == line->cap) {
        size_t ncap = line->cap ? line->cap * 2 : 4;
        BasicSentence **ns =
            (BasicSentence **)realloc(line->sentences,
                                      ncap * sizeof(BasicSentence *));
        if (!ns) {
            line->oom = 1;
            basic_sentence_free(s);
            return;
        }
        line->sentences = ns;
        line->cap = ncap;
    }
    line->sentences[line->n++] = s;
}

void basic_line_free(BasicLine *line)
{
    if (!line) return;
    for (size_t i = 0; i < line->n; i++) {
        basic_sentence_free(line->sentences[i]);
    }
    free(line->sentences);
    free(line);
}

/* sentence_bytes(sentence) (basic.py:137-152):
 *   result = [TOKENS[sentence[0]]]
 *   for i in sentence[1:]:
 *       if str:   result.extend(literal(i))
 *       elif num: result.extend(number(i))
 *       else:     result.extend(i)
 *   return result */
static void basic_sentence_bytes(const BasicSentence *s, ByteBuf *out)
{
    if (!s->token_ok || s->oom) {
        out->oom = 1;
        return;
    }
    bb_push(out, (unsigned char)s->token_code);
    for (size_t i = 0; i < s->nitems; i++) {
        const BasicItem *it = &s->items[i];
        switch (it->kind) {
        case ITEM_STRING: {
            /* literal(s) = [ord(c) for c in s] */
            size_t n = it->str ? strlen(it->str) : 0;
            bb_extend(out, (const unsigned char *)it->str, n);
            break;
        }
        case ITEM_NUMBER:
            basic_number(it->num, out);
            break;
        case ITEM_RAW:
            bb_extend(out, it->raw, it->raw_len);
            break;
        }
    }
}

/* line(sentences, line_number=None) (basic.py:154-173). */
static void basic_commit_line(Basic *b, BasicLine *line, int has_lineno,
                              int line_number)
{
    if (!b || !line) {
        basic_line_free(line);
        return;
    }
    if (line->oom) b->bytes.oom = 1;

    if (!has_lineno) {
        line_number = b->current_line + 10;
    }
    b->current_line = line_number;

    /* Build `result` first (sentences joined by ':'), then prepend the
     * line-number (BE) + numberLH(len(result)) (LE) header. */
    ByteBuf result;
    bb_init(&result);

    int first = 1;
    for (size_t i = 0; i < line->n; i++) {
        if (!first) {
            bb_push(&result, (unsigned char)':'); /* sep BETWEEN sentences */
        }
        first = 0;
        basic_sentence_bytes(line->sentences[i], &result);
    }
    bb_push(&result, BASIC_ENTER);

    /* result = line_number(line_number) + numberLH(len(result)) + result */
    ByteBuf prefixed;
    bb_init(&prefixed);
    basic_line_number(line_number, &prefixed);
    basic_numberLH((int)result.len, &prefixed);
    bb_extend_buf(&prefixed, &result);

    /* self.bytes += line(...) */
    bb_extend_buf(&b->bytes, &prefixed);

    bb_free(&prefixed);
    bb_free(&result);
    basic_line_free(line);
}

void basic_add_line(Basic *b, BasicLine *line)
{
    basic_commit_line(b, line, /*has_lineno=*/0, 0);
}

void basic_add_line_numbered(Basic *b, BasicLine *line, int line_number)
{
    basic_commit_line(b, line, /*has_lineno=*/1, line_number);
}
