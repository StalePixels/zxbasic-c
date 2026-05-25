/*
 * parser.c — BASIC parser for ZX BASIC compiler
 *
 * Hand-written recursive descent parser with Pratt expression parsing.
 * Ported from src/zxbc/zxbparser.py.
 */
#include "parser.h"
#include "errmsg.h"
#include "utils.h"      /* parse_int — same int coercion as the 'org' config arm (args.c:103) */
#include "plyparser/ply_tables.h" /* ply_term_id — Phase A token-parity dump only */

#include <ctype.h>      /* tolower — Python bool-coercion uses value.lower() (options.py:131) */
#include <math.h>
#include <stdio.h>      /* ZXBC_TOKDUMP in-parse token-parity trace (inert unless env set) */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Python str(float) renderer (defined in pyfloat.c, declared in z80asm.h).
 * Forward-declared here to render a NUMBER token's PLY `p.value` in
 * tok_unexpected_error without dragging in the whole z80asm.h surface. */
void z80h_pyfloat_repr(double v, char *buf, int sz);

/* ----------------------------------------------------------------
 * Token management
 * ---------------------------------------------------------------- */

static const char *tok_ply_value(const BToken *t, char *buf, size_t sz);

/* Phase A (lexer token parity) instrumentation. INERT in production: only
 * active when the env var ZXBC_TOKDUMP is set (the value names a file to
 * write, or "-" / "2" for stderr). Emits one line per RAW lexer token —
 * the stream the lexer actually produces, INCLUDING the lexer-error
 * pseudo-token (which PLY's parser sees as <ERROR>) that the parser
 * otherwise skips below. Format matches csrc/scripts/ply_tokdump.py:
 *   <ply-term-id>\t<TYPE>\t<lineno>\t<value-repr>
 * so the C in-parse stream can be diffed byte-for-byte against Python's. */
static FILE *g_tokdump = NULL;
static int g_tokdump_inited = 0;

static void tokdump_init(void) {
    if (g_tokdump_inited) return;
    g_tokdump_inited = 1;
    const char *path = getenv("ZXBC_TOKDUMP");
    if (!path || !*path) return;
    if (strcmp(path, "-") == 0 || strcmp(path, "2") == 0) {
        g_tokdump = stderr;
    } else {
        g_tokdump = fopen(path, "w");
    }
}

/* Render a token's PLY value-repr exactly as Python's repr() would print it
 * in ply_tokdump.py: NUMBER prints the bare float repr (3.0); everything
 * else prints the str repr in single quotes ('PRINT', '(', '\n', 'a'). */
static void tokdump_emit(const BToken *t) {
    if (!g_tokdump) return;
    const char *type;
    int id;
    if (t->type == BTOK_EOF) {
        type = "$end";
        id = PLY_END_ID;
    } else {
        type = btok_name(t->type);
        /* The lexer-error pseudo-token maps to PLY terminal <ERROR>. */
        id = ply_term_id(type);
    }
    /* NEWLINE faithfulness translation (Phase A finding, Phase E adapter):
     * PLY captures tok.lineno = self.lineno BEFORE the NEWLINE rule runs
     * (lex.py:224), then the rule does self.lineno += 1 — so PLY's NEWLINE
     * TOKEN carries the line it TERMINATES (source_line). The C lexer instead
     * increments lex->lineno first, then make_tok reads the new value
     * (lexer.c:765-767), so a C NEWLINE token carries source_line+1. They
     * differ by exactly 1. (The lexer's running counter lex->lineno still
     * matches PLY's self.lineno; only the NEWLINE token's captured .lineno
     * differs — and the existing recursive-descent parser deliberately reads
     * that post-increment value as a stand-in for p.lexer.lineno, so the
     * shared lexer is NOT changed. The PLY engine's lex adapter applies this
     * -1 translation instead.) Also PLY's NEWLINE value is "\n". */
    int out_lineno = t->lineno;
    if (t->type == BTOK_NEWLINE)
        out_lineno = t->lineno - 1;

    char buf[64];
    if (t->type == BTOK_NUMBER) {
        z80h_pyfloat_repr(t->numval, buf, (int)sizeof(buf));
        fprintf(g_tokdump, "%d\t%s\t%d\t%s\n", id, type, out_lineno, buf);
        return;
    }
    if (t->type == BTOK_EOF) {
        fprintf(g_tokdump, "%d\t%s\t%d\t\n", id, type, out_lineno);
        return;
    }
    /* value repr: Python prints repr(str). For our value set the only
     * special char is the newline ('\n'); single quotes/backslashes do not
     * occur in token values here. */
    const char *val;
    if (t->type == BTOK_NEWLINE)
        val = "\n"; /* PLY sets NEWLINE.value = "\n" */
    else
        val = tok_ply_value(t, buf, sizeof(buf));
    fprintf(g_tokdump, "%d\t%s\t%d\t'", id, type, out_lineno);
    for (const char *s = val; *s; s++) {
        if (*s == '\n') fputs("\\n", g_tokdump);
        else fputc(*s, g_tokdump);
    }
    fputs("'\n", g_tokdump);
}

static void advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = blexer_next(&p->lexer);
        tokdump_emit(&p->current);
        if (p->current.type != BTOK_ERROR) break;
        /* Error tokens are reported by the lexer; skip them */
    }
}

static bool check(Parser *p, BTokenType type) {
    return p->current.type == type;
}

static bool match(Parser *p, BTokenType type) {
    if (!check(p, type)) return false;
    advance(p);
    return true;
}

static void consume(Parser *p, BTokenType type, const char *msg) {
    if (check(p, type)) {
        advance(p);
        return;
    }
    zxbc_error(p->cs, p->current.lineno, "%s", msg);
    p->had_error = true;
}

/* Skip newlines (used where optional newlines are allowed) */
static void skip_newlines(Parser *p) {
    while (check(p, BTOK_NEWLINE)) advance(p);
}

/* Consume a newline or EOF */
static void consume_newline(Parser *p) {
    if (check(p, BTOK_EOF)) return;
    if (check(p, BTOK_NEWLINE)) {
        advance(p);
        return;
    }
    zxbc_error(p->cs, p->current.lineno, "Expected newline or end of statement");
    p->had_error = true;
}

/* Check if current token can be used as a name (keyword-as-identifier) */
static bool is_name_token(Parser *p) {
    BTokenType t = p->current.type;
    if (t == BTOK_ID || t == BTOK_ARRAY_ID) return true;
    /* Many keywords can be used as identifiers in certain contexts */
    if (t >= BTOK_ABS && t <= BTOK_XOR) return true;
    return false;
}

static const char *get_name_token(Parser *p) {
    if (p->current.sval) return p->current.sval;
    return btok_name(p->current.type);
}

/* Synchronize after error (skip to next statement boundary) */
static void synchronize(Parser *p) {
    p->panic_mode = false;
    while (!check(p, BTOK_EOF)) {
        if (p->previous.type == BTOK_NEWLINE) return;
        switch (p->current.type) {
            case BTOK_DIM:
            case BTOK_LET:
            case BTOK_IF:
            case BTOK_FOR:
            case BTOK_WHILE:
            case BTOK_DO:
            case BTOK_SUB:
            case BTOK_FUNCTION:
            case BTOK_PRINT:
            case BTOK_RETURN:
            case BTOK_END:
            case BTOK_GOTO:
            case BTOK_GOSUB:
            case BTOK_DECLARE:
                return;
            default:
                advance(p);
        }
    }
}

static void parser_error(Parser *p, const char *msg) {
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error = true;
    zxbc_error(p->cs, p->current.lineno, "%s", msg);
}

/* Render a token's PLY `p.value` exactly as Python's lexer would, into buf.
 * Used only by tok_unexpected_error to reproduce p_error's
 *   "Syntax Error. Unexpected token '%s' <%s>" % (p.value, p.type)
 * formatter (zxbparser.py:3564). For keyword tokens PLY's value is the
 * upper-cased keyword text (== btok_name); for ID/ARRAY_ID/LABEL it's the
 * source text (t.value, sval); for NUMBER it's `float(t.value)` rendered
 * with Python's str(float) (z80h_pyfloat_repr); for punctuation it's the
 * literal lexeme. */
static const char *tok_ply_value(const BToken *t, char *buf, size_t sz) {
    switch (t->type) {
        case BTOK_ID:
        case BTOK_ARRAY_ID:
        case BTOK_LABEL:
        case BTOK_ASM:
            if (t->sval) return t->sval;
            break;
        case BTOK_STRC:
            if (t->sval) return t->sval;
            break;
        case BTOK_ERROR:
            /* PLY's t_...ERROR token carries the offending char as its value
             * (Phase A: BTOK_ERROR.sval). p_error renders `'%' <ERROR>`, not
             * `'ERROR' <ERROR>` (lexerr). */
            if (t->sval) return t->sval;
            break;
        case BTOK_NUMBER: {
            z80h_pyfloat_repr(t->numval, buf, (int)sz);
            return buf;
        }
        case BTOK_PLUS:  return "+";
        case BTOK_MINUS: return "-";
        case BTOK_MUL:   return "*";
        case BTOK_DIV:   return "/";
        case BTOK_POW:   return "^";
        case BTOK_LP:    return "(";
        case BTOK_RP:    return ")";
        case BTOK_LBRACE: return "{";
        case BTOK_RBRACE: return "}";
        case BTOK_EQ:    return "=";
        case BTOK_LT:    return "<";
        case BTOK_GT:    return ">";
        case BTOK_LE:    return "<=";
        case BTOK_GE:    return ">=";
        case BTOK_NE:    return "<>";
        case BTOK_WEQ:   return ":=";
        case BTOK_CO:    return ":";
        case BTOK_SC:    return ";";
        case BTOK_COMMA: return ",";
        case BTOK_RIGHTARROW: return "=>";
        case BTOK_ADDRESSOF: return "@";
        case BTOK_SHL:   return "<<";
        case BTOK_SHR:   return ">>";
        case BTOK_BAND:  return "&";
        case BTOK_BOR:   return "|";
        case BTOK_BXOR:  return "~";
        case BTOK_BNOT:  return "!";
        default:
            break;
    }
    /* Keyword tokens: PLY's reserved-word value is the upper-cased keyword,
     * which is exactly what btok_name returns for the keyword range. */
    return btok_name(t->type);
}

/* Emit p_error's verbatim message for an unexpected token (zxbparser.py:3561
 * p_error: if p.type != "NEWLINE": "Syntax Error. Unexpected token '%s' <%s>"
 * else "Unexpected end of line"). Faults at the token's own line. */
static void tok_unexpected_error(Parser *p, const BToken *t) {
    if (p->panic_mode) return;
    p->panic_mode = true;
    p->had_error = true;
    if (t->type == BTOK_NEWLINE) {
        zxbc_error(p->cs, t->lineno, "Unexpected end of line");
        return;
    }
    char buf[64];
    const char *val = tok_ply_value(t, buf, sizeof(buf));
    zxbc_error(p->cs, t->lineno,
               "Syntax Error. Unexpected token '%s' <%s>",
               val, btok_name(t->type));
}

/* Case-insensitive string equality (Python's str.upper() == ... idiom,
 * used by p_save_code / p_load_code for the SCREEN$/CODE ID text test).
 * Avoids a strcasecmp/strings.h dependency — parser.c only has ctype.h. */
static int ci_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ----------------------------------------------------------------
 * AST helper constructors
 * ---------------------------------------------------------------- */

static AstNode *make_nop(Parser *p) {
    return ast_new(p->cs, AST_NOP, p->previous.lineno);
}

static AstNode *make_number(Parser *p, double value, int lineno, TypeInfo *type) {
    AstNode *n = ast_new(p->cs, AST_NUMBER, lineno);
    n->u.number.value = value;
    if (type) {
        n->type_ = type;
    } else {
        /* Auto-infer type from value */
        SymbolTable *st = p->cs->symbol_table;
        if (value == (int64_t)value) {
            int64_t iv = (int64_t)value;
            if (iv >= 0 && iv <= 255)
                n->type_ = st->basic_types[TYPE_ubyte];
            else if (iv >= -128 && iv <= 127)
                n->type_ = st->basic_types[TYPE_byte];
            else if (iv >= 0 && iv <= 65535)
                n->type_ = st->basic_types[TYPE_uinteger];
            else if (iv >= -32768 && iv <= 32767)
                n->type_ = st->basic_types[TYPE_integer];
            else if (iv >= 0 && iv <= 4294967295LL)
                n->type_ = st->basic_types[TYPE_ulong];
            else if (iv >= -2147483648LL && iv <= 2147483647LL)
                n->type_ = st->basic_types[TYPE_long];
            else
                n->type_ = st->basic_types[TYPE_float];
        } else {
            /* src/symbols/number.py:41-45 — a non-integer literal whose
             * value is in the fixed-point range (-32768.0, 32767) infers
             * TYPE.fixed; otherwise TYPE.float_. */
            if (value > -32768.0 && value < 32767.0)
                n->type_ = st->basic_types[TYPE_fixed];
            else
                n->type_ = st->basic_types[TYPE_float];
        }
    }
    return n;
}

/* Constant-fold a floating-point math builtin (SIN/COS/TAN/ASN/ACS/ATN/
 * LN/EXP/SQR) at parse time, exactly mirroring p_expr_trig's math-function
 * table (src/zxbc/zxbparser.py:3505-3517) fed through BUILTIN.make_node's
 * fold (src/symbols/builtin.py:74-77 -> SymbolNUMBER(func(value), float_)).
 *
 * Each branch is the SAME libm call Python's `math.*` dispatches to — on
 * macOS both engines link the system libm, so the IEEE-754 double result
 * is bit-identical, and the shared Z80 40-bit FP encoder turns it into the
 * same 5-byte constant.  CRITICAL faithfulness point: Python computes LN as
 * `math.log(y, math.exp(1))` — the TWO-argument log, i.e. log(y)/log(e) —
 * NOT the natural log `math.log(y)`.  These can differ in the last ULP, so
 * the C MUST replicate the division form character-for-character.
 *
 * Returns true if kw is one of the nine math builtins (the caller then
 * inspects *out).  Domain/range errors: Python's `math.*` RAISES for them
 * (math.asin(2)/math.acos(2)/math.log(0)/math.sqrt(-1) -> ValueError "math
 * domain error"; math.exp(710) -> OverflowError "math range error"), and
 * p_expr_trig does not catch it, so the exception propagates uncaught and
 * crashes the compiler with exit code 1 (verified: a Python traceback ending
 * in builtin.py:76 `func(operands[0].value)`).  C's libm instead returns a
 * NaN/Inf there, which the caller must NOT fold (a NaN/Inf NUMBER both
 * mis-encodes AND hangs the Z80 40-bit FP encoder on infinities) — the
 * caller detects the non-finite result and fails the compile (exit 1) to
 * match Python's exit code rather than silently producing a bogus binary. */
static bool math_fn_fold(BTokenType kw, double v, double *out) {
    switch (kw) {
        case BTOK_SIN: *out = sin(v);  return true;
        case BTOK_COS: *out = cos(v);  return true;
        case BTOK_TAN: *out = tan(v);  return true;
        case BTOK_ASN: *out = asin(v); return true;   /* ASN -> math.asin */
        case BTOK_ACS: *out = acos(v); return true;   /* ACS -> math.acos */
        case BTOK_ATN: *out = atan(v); return true;   /* ATN -> math.atan */
        case BTOK_LN:  *out = log(v) / log(exp(1.0)); return true;  /* log(y, e) */
        case BTOK_EXP: *out = exp(v);  return true;
        case BTOK_SQR: *out = sqrt(v); return true;
        default:       return false;
    }
}

static AstNode *make_string(Parser *p, const char *value, int lineno) {
    AstNode *n = ast_new(p->cs, AST_STRING, lineno);
    n->u.string.value = arena_strdup(&p->cs->arena, value);
    n->u.string.length = (int)strlen(value);
    n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
    return n;
}

/* Render a numeric constant exactly as Python's SymbolNUMBER.__str__
 * (symbols/number.py:31-36/63-64): the constructor normalises an
 * integer-valued operand to a Python int (`if int(value)==value: value =
 * int(value)`), so str() prints the plain integer decimal (no ".0", no
 * scientific notation) for ANY integer-valued magnitude; a genuine
 * non-integer float keeps str(float) — the shortest round-trip decimal
 * (z80h_pyfloat_repr).  %.0f reproduces str(int(double)) for every
 * integer-valued double, including magnitudes beyond int64 (verified vs
 * Python).  This is used by p_str's constant-fold (STR(<const>) -> the
 * STRING literal str(value)). */
static void py_number_str(double value, char *buf, size_t sz) {
    if (value == trunc(value) && !isinf(value) && !isnan(value))
        snprintf(buf, sz, "%.0f", value);
    else
        z80h_pyfloat_repr(value, buf, (int)sz);
}

/* ----------------------------------------------------------------
 * VAL(<constant string>) constant-folder — a faithful re-implementation
 * of Python's `float(eval(s, {}, {}))` over the NUMERIC sub-domain of
 * Python expression syntax (p_val, src/zxbc/zxbparser.py:3447-3462 ->
 * BUILTIN.make_node fold, src/symbols/builtin.py:74-77).
 *
 * Python's p_val evaluates a CONSTANT string argument at parse time with
 * the local `val(s)` helper:
 *     try:    x = float(eval(s, {}, {}))
 *     except: x = 0; warning(lineno, "Invalid string numeric constant
 *                                     '<s>' evaluated as 0")
 * and feeds x to make_builtin's func, which (operand is a constant string)
 * folds the BUILTIN to SymbolNUMBER(x, type_=float_).  So a compile-time
 * VAL of a string never reaches the runtime `.core.VAL`; it bakes a FLOAT
 * NUMBER constant whose value is the eval()'d expression float()-ed.
 *
 * We reproduce eval()'s NUMERIC domain EXACTLY (Python literal syntax, not
 * BASIC): decimal / 0x / 0o / 0b integer literals (with PEP-515 numeric
 * underscores and Python's no-leading-zero-decimal rule), float literals
 * incl. scientific notation, and the operators  **  (right-assoc, power),
 * unary +/-,  * / // %  (true-div, floor-div, Python modulo),  binary +/-,
 * and parentheses, with Python's precedence and int-vs-float promotion.
 * The whole result is then float()-coerced (see val_fold below).
 *
 * BOUNDARY: Python eval() is a strict superset — it also evaluates
 * comparisons (`1<2`), subscripts (`[1,2][0]`), conditionals
 * (`1 if 1 else 2`), function calls (`abs(-3)`), tuples (`1,2`), names,
 * etc.  Those are OUTSIDE this numeric evaluator.  Crucially we NEVER
 * accept-and-mis-evaluate: any input we cannot parse as a pure numeric
 * expression fails (val_parse returns false) and the caller takes the
 * except->0+warning path — the SAME outcome Python produces for genuinely
 * invalid input, and a documented (flagged) divergence ONLY for the rare
 * non-numeric-but-eval()-able strings that realistic VAL code never uses.
 *
 * Python int arithmetic is ARBITRARY PRECISION; the integer domain
 * (+ - * // % ** on ints) is therefore evaluated with a sign-magnitude
 * BigInt (base 2^32 limbs, see below), then float()-coerced with correct
 * IEEE-754 round-to-nearest-even — so the folded constant is byte-identical
 * to Python's `float(eval(s))` for every representable value (e.g.
 * VAL("2**100") -> 1.2676506002282294e30, not 0).  A value whose float()
 * overflows the double range (>~1.8e308, e.g. 74**800) raises in Python
 * (OverflowError) and is caught -> except->0+warning; the BigInt path
 * reproduces that exactly (BIG_to_double returns +inf -> val_fold fails).
 * Floats and true-division (/) stay double, as in Python.
 * ---------------------------------------------------------------- */

/* Sign-magnitude arbitrary-precision integer in base 2^32 (little-endian
 * limbs).  BIG_LIMBS caps the magnitude well above the double-range ceiling:
 * a finite double needs < 1024 significant bits (32 limbs); any integer that
 * needs more than BIG_LIMBS limbs (8192 bits) float()-overflows anyway, so
 * the cap is a faithful overflow signal (Python would compute the int but its
 * float() would raise -> except->0) rather than a silent wrong answer. */
#define BIG_LIMBS 256
typedef struct {
    int sign;                 /* -1, 0, or +1 (0 magnitude => sign 0) */
    int n;                    /* number of significant limbs (0 => zero) */
    uint32_t d[BIG_LIMBS];    /* little-endian base-2^32 limbs */
    bool overflow;            /* exceeded BIG_LIMBS — treat as float-overflow */
} BigInt;

static void big_zero(BigInt *a) {
    a->sign = 0;
    a->n = 0;
    a->overflow = false;
    memset(a->d, 0, sizeof(a->d));  /* ops read dest limbs (mul/add/shl) */
}

static void big_norm(BigInt *a) {
    while (a->n > 0 && a->d[a->n - 1] == 0) a->n--;
    if (a->n == 0) a->sign = 0;
}

static void big_from_u64(BigInt *a, uint64_t v) {
    big_zero(a);
    if (v == 0) return;
    a->sign = 1;
    a->d[0] = (uint32_t)(v & 0xFFFFFFFFu);
    a->d[1] = (uint32_t)(v >> 32);
    a->n = a->d[1] ? 2 : 1;
}

/* Compare magnitudes only: -1 if |a|<|b|, 0 if equal, 1 if |a|>|b|. */
static int big_cmp_mag(const BigInt *a, const BigInt *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* r = |a| + |b| (magnitudes); sets r->overflow if it exceeds BIG_LIMBS. */
static void big_add_mag(const BigInt *a, const BigInt *b, BigInt *r) {
    const BigInt *x = a->n >= b->n ? a : b;
    const BigInt *y = a->n >= b->n ? b : a;
    uint64_t carry = 0;
    int i;
    BigInt tmp;
    big_zero(&tmp);
    for (i = 0; i < y->n; i++) {
        uint64_t s = (uint64_t)x->d[i] + y->d[i] + carry;
        tmp.d[i] = (uint32_t)s;
        carry = s >> 32;
    }
    for (; i < x->n; i++) {
        uint64_t s = (uint64_t)x->d[i] + carry;
        tmp.d[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry) {
        if (i >= BIG_LIMBS) { r->overflow = true; r->sign = 0; r->n = 0; return; }
        tmp.d[i++] = (uint32_t)carry;
    }
    tmp.n = i;
    tmp.sign = i ? 1 : 0;
    *r = tmp;
}

/* r = |a| - |b|, requires |a| >= |b| (magnitudes). */
static void big_sub_mag(const BigInt *a, const BigInt *b, BigInt *r) {
    int64_t borrow = 0;
    int i;
    BigInt tmp;
    big_zero(&tmp);
    for (i = 0; i < a->n; i++) {
        int64_t s = (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0) - borrow;
        if (s < 0) { s += ((int64_t)1 << 32); borrow = 1; } else borrow = 0;
        tmp.d[i] = (uint32_t)s;
    }
    tmp.n = a->n;
    tmp.sign = 1;
    big_norm(&tmp);
    *r = tmp;
}

/* Signed add: r = a + b. */
static void big_add(const BigInt *a, const BigInt *b, BigInt *r) {
    if (a->overflow || b->overflow) { big_zero(r); r->overflow = true; return; }
    if (a->sign == 0) { *r = *b; return; }
    if (b->sign == 0) { *r = *a; return; }
    if (a->sign == b->sign) {
        int s = a->sign;
        big_add_mag(a, b, r);
        if (!r->overflow && r->n) r->sign = s;
    } else {
        int c = big_cmp_mag(a, b);
        if (c == 0) { big_zero(r); return; }
        if (c > 0) { big_sub_mag(a, b, r); r->sign = a->sign; }
        else       { big_sub_mag(b, a, r); r->sign = b->sign; }
    }
}

static void big_neg(BigInt *a) { if (a->n) a->sign = -a->sign; }

/* Signed subtract: r = a - b. */
static void big_sub(const BigInt *a, const BigInt *b, BigInt *r) {
    BigInt nb = *b;
    big_neg(&nb);
    big_add(a, &nb, r);
}

/* r = a * b (signed). */
static void big_mul(const BigInt *a, const BigInt *b, BigInt *r) {
    if (a->overflow || b->overflow) { big_zero(r); r->overflow = true; return; }
    if (a->sign == 0 || b->sign == 0) { big_zero(r); return; }
    int rn = a->n + b->n;
    if (rn > BIG_LIMBS) { big_zero(r); r->overflow = true; return; }
    BigInt tmp;
    big_zero(&tmp);
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->n; j++) {
            uint64_t s = (uint64_t)a->d[i] * b->d[j] + tmp.d[i + j] + carry;
            tmp.d[i + j] = (uint32_t)s;
            carry = s >> 32;
        }
        tmp.d[i + b->n] += (uint32_t)carry;  /* fits: rn<=BIG_LIMBS */
    }
    tmp.n = rn;
    tmp.sign = a->sign * b->sign;
    big_norm(&tmp);
    *r = tmp;
}

/* Multiply magnitude by a small (<2^32) value and add a small addend — used
 * by the decimal/hex/oct/bin literal scanner (a = a*base + dv). */
static void big_muladd_small(BigInt *a, uint32_t base, uint32_t add) {
    if (a->overflow) return;
    uint64_t carry = add;
    int i;
    for (i = 0; i < a->n; i++) {
        uint64_t s = (uint64_t)a->d[i] * base + carry;
        a->d[i] = (uint32_t)s;
        carry = s >> 32;
    }
    while (carry) {
        if (i >= BIG_LIMBS) { a->overflow = true; a->sign = 0; a->n = 0; return; }
        a->d[i++] = (uint32_t)carry;
        carry >>= 32;
    }
    a->n = i;
    a->sign = i ? 1 : 0;
}

/* Bit length of the magnitude (0 for zero). */
static int big_bitlen(const BigInt *a) {
    if (a->n == 0) return 0;
    uint32_t top = a->d[a->n - 1];
    int bits = (a->n - 1) * 32;
    while (top) { bits++; top >>= 1; }
    return bits;
}

static bool big_test_bit(const BigInt *a, int bit) {
    int limb = bit >> 5, off = bit & 31;
    if (limb >= a->n) return false;
    return (a->d[limb] >> off) & 1u;
}

/* Shift magnitude left by k bits (k arbitrary). */
static void big_shl(const BigInt *a, int k, BigInt *r) {
    BigInt tmp;
    big_zero(&tmp);
    if (a->n == 0 || k < 0) { *r = tmp; return; }
    int limbshift = k >> 5, bitshift = k & 31;
    int nn = a->n + limbshift + 1;
    if (nn > BIG_LIMBS) { big_zero(r); r->overflow = true; return; }
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t v = ((uint64_t)a->d[i] << bitshift) | carry;
        tmp.d[i + limbshift] = (uint32_t)v;
        carry = v >> 32;
    }
    tmp.d[a->n + limbshift] = (uint32_t)carry;
    tmp.n = nn;
    tmp.sign = 1;
    big_norm(&tmp);
    *r = tmp;
}

/* Full magnitude divmod via shift-subtract (binary long division):
 * |a| = q*|b| + rem, 0 <= rem < |b|.  Both outputs are magnitudes (sign 0/1).
 * Requires b != 0. */
static void big_divmod_mag(const BigInt *a, const BigInt *b,
                           BigInt *q, BigInt *rem) {
    BigInt qq, rr;
    big_zero(&qq);
    big_zero(&rr);
    int bits = big_bitlen(a);
    for (int i = bits - 1; i >= 0; i--) {
        /* rr = (rr << 1) | bit_i(a) */
        BigInt shifted;
        big_shl(&rr, 1, &shifted);
        rr = shifted;
        if (big_test_bit(a, i)) {
            if (rr.n == 0) { rr.n = 1; rr.sign = 1; }
            rr.d[0] |= 1u;
            if (rr.sign == 0) rr.sign = 1;
        }
        if (big_cmp_mag(&rr, b) >= 0) {
            BigInt t;
            big_sub_mag(&rr, b, &t);
            rr = t;
            int limb = i >> 5, off = i & 31;
            if (limb >= qq.n) qq.n = limb + 1;
            qq.d[limb] |= (1u << off);
            qq.sign = 1;
        }
    }
    big_norm(&qq);
    big_norm(&rr);
    *q = qq;
    *rem = rr;
}

/* Python floor division a // b (signed, floor toward -inf).  Returns false
 * on b == 0 (ZeroDivisionError -> except->0). */
static bool big_floordiv(const BigInt *a, const BigInt *b, BigInt *out) {
    if (b->sign == 0) return false;
    if (a->sign == 0) { big_zero(out); return true; }
    BigInt q, r;
    big_divmod_mag(a, b, &q, &r);
    int s = a->sign * b->sign;
    if (s < 0 && r.n != 0) {
        /* truncated-toward-zero quotient q; floor needs q+1 in magnitude. */
        BigInt one, q2;
        big_from_u64(&one, 1);
        big_add_mag(&q, &one, &q2);
        q = q2;
    }
    if (q.n) q.sign = s;
    big_norm(&q);
    *out = q;
    return true;
}

/* Python modulo a % b (signed; result sign follows divisor).  Returns false
 * on b == 0. */
static bool big_mod(const BigInt *a, const BigInt *b, BigInt *out) {
    if (b->sign == 0) return false;
    if (a->sign == 0) { big_zero(out); return true; }
    BigInt q, r;
    big_divmod_mag(a, b, &q, &r);
    if (r.n == 0) { big_zero(out); return true; }
    /* r is the magnitude of the truncated remainder (sign of dividend). */
    r.sign = a->sign;
    if (a->sign != b->sign) {
        /* Python's % follows the divisor: add b to bring sign in line. */
        BigInt t;
        big_add(&r, b, &t);
        r = t;
    }
    *out = r;
    return true;
}

/* r = base ** exp (exp >= 0), by square-and-multiply.  exp is an unsigned
 * 64-bit count; overflow past BIG_LIMBS sets r->overflow. */
static void big_pow(const BigInt *base, uint64_t exp, BigInt *r) {
    BigInt result, b;
    big_from_u64(&result, 1);
    b = *base;
    while (exp > 0) {
        if (exp & 1) {
            BigInt t;
            big_mul(&result, &b, &t);
            result = t;
            if (result.overflow) { *r = result; return; }
        }
        exp >>= 1;
        if (exp) {
            BigInt t;
            big_mul(&b, &b, &t);
            b = t;
            if (b.overflow) { big_zero(r); r->overflow = true; return; }
        }
    }
    *r = result;
}

/* Convert a BigInt to double with IEEE-754 round-to-nearest-even, exactly
 * matching Python's int.__float__.  Returns +/-inf on float() overflow
 * (magnitude needs >= 1024 bits of exponent), which the caller treats as
 * Python's OverflowError -> except->0. */
static double big_to_double(const BigInt *a) {
    if (a->overflow) return a->sign < 0 ? -INFINITY : INFINITY;
    if (a->n == 0) return 0.0;
    int bits = big_bitlen(a);
    /* Collect the top min(53, bits) bits into `mant` (MSB first); `scale` is
     * the count of low-order bit positions NOT folded into mant, so the exact
     * value is mant * 2^scale.  Round to nearest-even using the next bit and a
     * sticky OR of everything below it. */
    uint64_t mant = 0;
    int taken = 0;
    int i;
    for (i = bits - 1; i >= 0 && taken < 53; i--, taken++)
        mant = (mant << 1) | (big_test_bit(a, i) ? 1u : 0u);
    int scale = i + 1;                 /* low bits at positions [0 .. i] remain */
    /* i now points just below the consumed window: the round bit is bit i. */
    if (i >= 0) {
        bool round_bit = big_test_bit(a, i);
        bool sticky = false;
        for (int j = i - 1; j >= 0 && !sticky; j--)
            if (big_test_bit(a, j)) sticky = true;
        bool roundup = false;
        if (round_bit) {
            if (sticky) roundup = true;
            else roundup = (mant & 1u) != 0;  /* tie -> round to even */
        }
        if (roundup) {
            mant++;
            if (mant == ((uint64_t)1 << 53)) { mant >>= 1; scale++; }  /* carry */
        }
    }
    /* value = mant * 2^scale.  ldexp yields +inf on overflow (magnitude past
     * the double range); the caller treats a non-finite result as Python's
     * float() OverflowError -> except->0. */
    double val = ldexp((double)mant, scale);
    if (!isfinite(val)) return a->sign < 0 ? -INFINITY : INFINITY;
    return a->sign < 0 ? -val : val;
}

/* A Python value in the numeric domain: an exact int (a BigInt, Python's
 * arbitrary-precision int) or a float.  `big` is meaningful iff is_int;
 * otherwise `d` holds the float. */
typedef struct {
    bool is_int;
    BigInt big;
    double d;
} PyNum;

/* Cursor over the source string for the recursive-descent evaluator. */
typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    bool error;  /* set on any parse/eval failure (incl. bignum overflow) */
} ValParser;

static double pynum_to_double(PyNum v) {
    return v.is_int ? big_to_double(&v.big) : v.d;
}

static void val_skip_ws(ValParser *vp) {
    /* Python's tokenizer treats spaces/tabs/form-feed as insignificant
     * between tokens (newlines are not expected in a VAL string). */
    while (vp->pos < vp->len) {
        char c = vp->s[vp->pos];
        if (c == ' ' || c == '\t' || c == '\f' || c == '\r' || c == '\n')
            vp->pos++;
        else
            break;
    }
}

static bool val_parse_expr(ValParser *vp, PyNum *out);

/* Scan an integer literal body in the given base, honouring PEP-515
 * underscores (a single '_' allowed between digits, never leading/trailing
 * or doubled).  Accumulates into an arbitrary-precision BigInt; returns false
 * only on malformed underscore / no digits / bignum-cap overflow. */
static bool val_scan_int_base(ValParser *vp, int base, BigInt *out) {
    BigInt acc;
    big_zero(&acc);
    bool any = false;
    bool last_us = true;  /* disallow leading underscore */
    while (vp->pos < vp->len) {
        char c = vp->s[vp->pos];
        int dv;
        if (c == '_') {
            if (last_us) return false;  /* leading/doubled underscore */
            last_us = true;
            vp->pos++;
            continue;
        }
        if (c >= '0' && c <= '9') dv = c - '0';
        else if (c >= 'a' && c <= 'f') dv = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') dv = c - 'A' + 10;
        else break;
        if (dv >= base) break;
        big_muladd_small(&acc, (uint32_t)base, (uint32_t)dv);
        if (acc.overflow) return false;  /* >8192-bit literal -> except->0 */
        any = true;
        last_us = false;
        vp->pos++;
    }
    if (!any || last_us) return false;       /* no digits / trailing '_' */
    *out = acc;
    return true;
}

/* Parse a numeric literal (Python syntax) at the cursor.  Handles the
 * 0x/0o/0b integer prefixes, the decimal-int form (with the no-leading-zero
 * rule), and float forms (fraction and/or exponent).  Underscores allowed
 * per PEP-515. */
static bool val_parse_number(ValParser *vp, PyNum *out) {
    size_t start = vp->pos;
    if (vp->pos >= vp->len) return false;

    /* Prefixed integer literals: 0x.. 0o.. 0b.. */
    if (vp->s[vp->pos] == '0' && vp->pos + 1 < vp->len) {
        char p2 = vp->s[vp->pos + 1];
        int base = 0;
        if (p2 == 'x' || p2 == 'X') base = 16;
        else if (p2 == 'o' || p2 == 'O') base = 8;
        else if (p2 == 'b' || p2 == 'B') base = 2;
        if (base) {
            vp->pos += 2;
            BigInt iv;
            if (!val_scan_int_base(vp, base, &iv)) return false;
            out->is_int = true;
            out->big = iv;
            return true;
        }
    }

    /* Build a normalised buffer (digits/'.'/'e'/sign, underscores stripped)
     * to detect int-vs-float and to feed strtod for the float case.  Track
     * whether a '.' or exponent appeared (=> float). */
    char buf[512];
    size_t bi = 0;
    bool is_float = false;
    bool any_digit = false;
    bool last_us = true;       /* disallow leading underscore */
    size_t int_digits = 0;     /* count of leading decimal-int digits */

    /* Integer/fraction digit run before any exponent. */
    while (vp->pos < vp->len) {
        char c = vp->s[vp->pos];
        if (c == '_') {
            if (last_us) return false;
            last_us = true;
            vp->pos++;
            continue;
        }
        if (c >= '0' && c <= '9') {
            if (bi + 1 >= sizeof(buf)) return false;
            buf[bi++] = c;
            any_digit = true;
            if (!is_float) int_digits++;
            last_us = false;
            vp->pos++;
            continue;
        }
        if (c == '.') {
            if (is_float) return false;     /* second dot => malformed */
            if (last_us && bi > 0) return false; /* underscore before dot */
            is_float = true;
            if (bi + 1 >= sizeof(buf)) return false;
            buf[bi++] = c;
            last_us = true;  /* a digit must follow '_' but '.' resets run */
            last_us = false; /* '.' itself is a valid run break; allow .5 */
            vp->pos++;
            continue;
        }
        break;
    }
    if (last_us) return false;  /* trailing underscore in mantissa */

    /* Optional exponent. */
    if (vp->pos < vp->len && (vp->s[vp->pos] == 'e' || vp->s[vp->pos] == 'E')) {
        /* Lookahead: an exponent requires (optional sign +) at least one
         * digit; otherwise the 'e' is not part of this number. */
        size_t save = vp->pos;
        size_t k = vp->pos + 1;
        if (k < vp->len && (vp->s[k] == '+' || vp->s[k] == '-')) k++;
        bool exp_digit = (k < vp->len && vp->s[k] >= '0' && vp->s[k] <= '9');
        if (!exp_digit) {
            vp->pos = save;  /* not an exponent */
        } else {
            is_float = true;
            if (bi + 1 >= sizeof(buf)) return false;
            buf[bi++] = 'e';
            vp->pos++;
            if (vp->s[vp->pos] == '+' || vp->s[vp->pos] == '-') {
                buf[bi++] = vp->s[vp->pos++];
            }
            bool elast_us = true;
            bool edig = false;
            while (vp->pos < vp->len) {
                char c = vp->s[vp->pos];
                if (c == '_') {
                    if (elast_us) return false;
                    elast_us = true;
                    vp->pos++;
                    continue;
                }
                if (c >= '0' && c <= '9') {
                    if (bi + 1 >= sizeof(buf)) return false;
                    buf[bi++] = c;
                    edig = true;
                    elast_us = false;
                    vp->pos++;
                    continue;
                }
                break;
            }
            if (!edig || elast_us) return false;
        }
    }

    if (!any_digit) {
        /* No mantissa digits at all (e.g. bare '.') => not a number. */
        vp->pos = start;
        return false;
    }
    buf[bi] = '\0';

    if (!is_float) {
        /* Decimal integer literal.  Python forbids leading zeros in a
         * non-zero decimal int ("007" is a SyntaxError); only "0",
         * "0_0", "00..0" (all zeros) are allowed.  Detect: more than one
         * digit, first is '0', and some non-zero digit present. */
        if (int_digits > 1 && buf[0] == '0') {
            bool all_zero = true;
            for (size_t j = 0; j < bi; j++)
                if (buf[j] != '0') { all_zero = false; break; }
            if (!all_zero) return false;  /* leading-zero decimal => error */
        }
        /* Re-scan buf (underscores already stripped) as a base-10 BigInt
         * (Python's unbounded int).  Only a >8192-bit literal trips the cap
         * (it would float()-overflow anyway -> except->0). */
        BigInt acc;
        big_zero(&acc);
        for (size_t j = 0; j < bi; j++) {
            big_muladd_small(&acc, 10u, (uint32_t)(buf[j] - '0'));
            if (acc.overflow) return false;
        }
        out->is_int = true;
        out->big = acc;
        return true;
    }

    /* Float literal. */
    char *end = NULL;
    double d = strtod(buf, &end);
    if (end != buf + bi) return false;
    out->is_int = false;
    out->d = d;
    return true;
}

static bool val_parse_atom(ValParser *vp, PyNum *out) {
    val_skip_ws(vp);
    if (vp->pos >= vp->len) return false;
    char c = vp->s[vp->pos];
    if (c == '(') {
        vp->pos++;
        if (!val_parse_expr(vp, out)) return false;
        val_skip_ws(vp);
        if (vp->pos >= vp->len || vp->s[vp->pos] != ')') return false;
        vp->pos++;
        return true;
    }
    if ((c >= '0' && c <= '9') || c == '.') {
        return val_parse_number(vp, out);
    }
    return false;  /* names/brackets/etc. — outside the numeric domain */
}

static bool val_parse_unary(ValParser *vp, PyNum *out);

/* power := atom ('**' unary)?   — '**' is right-associative; its right
 * operand is a unary (so 2**-1 works), its left is an atom (so -2**2 =
 * -(2**2): unary minus binds looser than '**'). */
static bool val_parse_power(ValParser *vp, PyNum *out) {
    PyNum base;
    if (!val_parse_atom(vp, &base)) return false;
    val_skip_ws(vp);
    if (vp->pos + 1 < vp->len && vp->s[vp->pos] == '*' &&
        vp->s[vp->pos + 1] == '*') {
        vp->pos += 2;
        PyNum exp;
        if (!val_parse_unary(vp, &exp)) return false;
        /* Python ** semantics: int**non-negative-int -> int (arbitrary
         * precision); otherwise float (incl. int**negative-int -> float,
         * e.g. 2**-1 == 0.5).  A non-negative integer exponent that exceeds
         * what we can represent as a count, or a result past the BigInt cap,
         * float()-overflows in Python too (the magnitude needs >1024 bits)
         * -> except->0, so we signal failure rather than fold a wrong value. */
        if (base.is_int && exp.is_int && exp.big.sign >= 0) {
            /* Exponent as an unsigned count. A huge exponent (more than fits
             * in 64 bits) makes base**exp astronomically large -> float
             * overflow -> except->0 (unless base is 0/1, handled by value). */
            if (exp.big.n > 2) {
                /* exp > 2^64: only 0**big / 1**big stay finite. */
                if (base.big.n == 0) {            /* 0 ** big = 0 */
                    out->is_int = true; big_zero(&out->big); return true;
                }
                if (base.big.n == 1 && base.big.d[0] == 1) {  /* (+/-1)**big */
                    out->is_int = true;
                    big_from_u64(&out->big, 1);
                    if (base.big.sign < 0 && big_test_bit(&exp.big, 0))
                        out->big.sign = -1;
                    return true;
                }
                vp->error = true; return false;   /* overflow -> except->0 */
            }
            uint64_t e = (uint64_t)exp.big.d[0] |
                         (exp.big.n > 1 ? ((uint64_t)exp.big.d[1] << 32) : 0);
            BigInt res;
            big_pow(&base.big, e, &res);
            if (res.overflow) { vp->error = true; return false; }
            out->is_int = true;
            out->big = res;
        } else {
            out->is_int = false;
            out->d = pow(pynum_to_double(base), pynum_to_double(exp));
        }
        return true;
    }
    *out = base;
    return true;
}

/* unary := ('+'|'-') unary | power */
static bool val_parse_unary(ValParser *vp, PyNum *out) {
    val_skip_ws(vp);
    if (vp->pos < vp->len && (vp->s[vp->pos] == '+' || vp->s[vp->pos] == '-')) {
        char op = vp->s[vp->pos];
        vp->pos++;
        PyNum v;
        if (!val_parse_unary(vp, &v)) return false;
        if (op == '-') {
            if (v.is_int) {
                big_neg(&v.big);  /* sign-magnitude: no overflow */
            } else {
                v.d = -v.d;
            }
        }
        *out = v;
        return true;
    }
    return val_parse_power(vp, out);
}

/* mul := unary (('*'|'/'|'//'|'%') unary)*  (left-associative) */
static bool val_parse_mul(ValParser *vp, PyNum *out) {
    PyNum lhs;
    if (!val_parse_unary(vp, &lhs)) return false;
    for (;;) {
        val_skip_ws(vp);
        if (vp->pos >= vp->len) break;
        char c = vp->s[vp->pos];
        int op = 0;  /* 1=* 2=/ 3=// 4=% */
        if (c == '*') {
            if (vp->pos + 1 < vp->len && vp->s[vp->pos + 1] == '*') break;
            op = 1; vp->pos++;
        } else if (c == '/') {
            if (vp->pos + 1 < vp->len && vp->s[vp->pos + 1] == '/') {
                op = 3; vp->pos += 2;
            } else {
                op = 2; vp->pos++;
            }
        } else if (c == '%') {
            op = 4; vp->pos++;
        } else {
            break;
        }
        PyNum rhs;
        if (!val_parse_unary(vp, &rhs)) return false;
        bool both_int = lhs.is_int && rhs.is_int;
        switch (op) {
            case 1:  /* * */
                if (both_int) {
                    BigInt r;
                    big_mul(&lhs.big, &rhs.big, &r);
                    if (r.overflow) { vp->error = true; return false; }
                    lhs.is_int = true; lhs.big = r;
                } else {
                    lhs.d = pynum_to_double(lhs) * pynum_to_double(rhs);
                    lhs.is_int = false;
                }
                break;
            case 2:  /* / true division -> ALWAYS float */
                if ((rhs.is_int && rhs.big.sign == 0) ||
                    (!rhs.is_int && rhs.d == 0.0))
                    return false;  /* ZeroDivisionError -> except->0 */
                lhs.d = pynum_to_double(lhs) / pynum_to_double(rhs);
                lhs.is_int = false;
                break;
            case 3:  /* // floor division */
                if (both_int) {
                    BigInt r;
                    if (!big_floordiv(&lhs.big, &rhs.big, &r)) return false;
                    lhs.is_int = true; lhs.big = r;
                } else {
                    double bd = pynum_to_double(rhs);
                    if (bd == 0.0) return false;
                    lhs.d = floor(pynum_to_double(lhs) / bd);
                    lhs.is_int = false;
                }
                break;
            case 4:  /* % Python modulo */
                if (both_int) {
                    BigInt r;
                    if (!big_mod(&lhs.big, &rhs.big, &r)) return false;
                    lhs.is_int = true; lhs.big = r;
                } else {
                    double bd = pynum_to_double(rhs);
                    if (bd == 0.0) return false;
                    double ad = pynum_to_double(lhs);
                    double r = fmod(ad, bd);
                    /* fmod sign follows dividend; Python % follows divisor. */
                    if (r != 0.0 && ((r < 0) != (bd < 0))) r += bd;
                    lhs.d = r;
                    lhs.is_int = false;
                }
                break;
        }
    }
    *out = lhs;
    return true;
}

/* expr := mul (('+'|'-') mul)*  (left-associative) */
static bool val_parse_expr(ValParser *vp, PyNum *out) {
    PyNum lhs;
    if (!val_parse_mul(vp, &lhs)) return false;
    for (;;) {
        val_skip_ws(vp);
        if (vp->pos >= vp->len) break;
        char c = vp->s[vp->pos];
        if (c != '+' && c != '-') break;
        vp->pos++;
        PyNum rhs;
        if (!val_parse_mul(vp, &rhs)) return false;
        bool both_int = lhs.is_int && rhs.is_int;
        if (both_int) {
            BigInt r;
            if (c == '+') big_add(&lhs.big, &rhs.big, &r);
            else          big_sub(&lhs.big, &rhs.big, &r);
            if (r.overflow) { vp->error = true; return false; }
            lhs.big = r;
            lhs.is_int = true;
        } else {
            double r = (c == '+') ? pynum_to_double(lhs) + pynum_to_double(rhs)
                                  : pynum_to_double(lhs) - pynum_to_double(rhs);
            lhs.d = r;
            lhs.is_int = false;
        }
    }
    *out = lhs;
    return true;
}

/* Top-level: parse the WHOLE string as one numeric expression and, on
 * success, return its float()-coerced value (matching `float(eval(s))`).
 * Returns false on ANY failure (unparsable, trailing tokens, division by
 * zero, int64 overflow, non-finite float()), so the caller takes the
 * except->0+warning path exactly as Python's `except: x = 0`. */
static bool val_fold(const char *s, size_t len, double *out) {
    ValParser vp = { s, len, 0, false };
    PyNum v;
    if (!val_parse_expr(&vp, &v) || vp.error) return false;
    val_skip_ws(&vp);
    if (vp.pos != vp.len) return false;  /* trailing junk -> SyntaxError */
    double d = pynum_to_double(v);
    /* float() of an int whose magnitude exceeds the double range raises
     * OverflowError in Python (caught -> except->0); big_to_double returns
     * +/-inf there, so the non-finite guard reproduces it.  A literal float
     * like 1e400 also yields inf (Python's float("1e400") is inf, but the
     * Z80 FP encoder can't encode inf), so non-finite uniformly takes the
     * except->0 path.  A finite double — including out-of-Z80-range values
     * like float(2**100)=1.27e30 — is folded; matching Python's bytes there
     * is then the Z80 encoder's job (the shared 40-bit encoder). */
    if (!isfinite(d)) return false;
    *out = d;
    return true;
}

static AstNode *make_sentence_node(Parser *p, const char *kind, int lineno) {
    AstNode *n = ast_new(p->cs, AST_SENTENCE, lineno);
    n->u.sentence.kind = arena_strdup(&p->cs->arena, kind);
    n->u.sentence.sentinel = false;
    return n;
}

static AstNode *make_block_node(Parser *p, int lineno) {
    return ast_new(p->cs, AST_BLOCK, lineno);
}

/* Label DEFINITION at a label site (`<label>:` / line-number label). Performs
 * the symbol-table side effects (Python make_label -> declare_label +
 * DATA_LABELS, zxbparser.py:453-458; the C uses access_label as its
 * declare_label analogue) and returns the SENTENCE("LABEL") > AST_ID node the
 * production parser emits for a label. Extracted from parse_statement's label
 * branch so the Phase-D `label : LABEL` reduce-action builds the byte-for-byte
 * same tree + symbol-table state (C-vs-C identity). Behaviour-preserving. */
static AstNode *label_define(Parser *p, const char *label_text, int lineno) {
    /* Create label in symbol table (labels are always global) */
    AstNode *label_node = symboltable_access_label(p->cs->symbol_table, p->cs,
                                                    label_text, lineno);
    if (label_node && label_node->u.id.class_ == CLASS_label) {
        if (label_node->u.id.declared) {
            zxbc_error(p->cs, lineno, "Label '%s' already used at %s:%d",
                       label_text, p->cs->current_file, label_node->lineno);
        }
        label_node->u.id.declared = true;
        /* declare_label (symboltable.py:626): entry.type_ = PTR_TYPE. */
        label_node->type_ = p->cs->symbol_table->basic_types[TYPE_uinteger];
    }
    if (label_node)
        label_capture_scope_owner(p->cs, label_node);
    if (label_node)
        hashmap_set(&p->cs->data_labels, label_text,
                    p->cs->data_ptr_current ? p->cs->data_ptr_current : "");

    AstNode *lbl_sent = make_sentence_node(p, "LABEL", lineno);
    AstNode *lbl_id = ast_new(p->cs, AST_ID, lineno);
    lbl_id->u.id.name = arena_strdup(&p->cs->arena, label_text);
    lbl_id->u.id.class_ = CLASS_label;
    ast_add_child(p->cs, lbl_sent, lbl_id);
    return lbl_sent;
}

static AstNode *make_asm_node(Parser *p, const char *code, int lineno) {
    AstNode *n = ast_new(p->cs, AST_ASM, lineno);
    n->u.asm_block.code = arena_strdup(&p->cs->arena, code);
    /* Capture node.filename at construction (zxbparser:255 →
     * sym.ASM(..., gl.FILENAME)). visit_ASM (translator.py:963-967) emits
     * `#line N "node.filename"` — must be the file that *held* the ASM
     * block, including #include'd files. cs->current_file moves with
     * each #line directive at parse time; the translator runs *after*
     * parse and sees only the final value, so snapshot it now. */
    n->u.asm_block.filename = p->cs->current_file
        ? arena_strdup(&p->cs->arena, p->cs->current_file)
        : NULL;
    return n;
}

/* Add statement to a block, creating/extending as needed */
static AstNode *block_append(Parser *p, AstNode *block, AstNode *stmt) {
    if (!stmt || stmt->tag == AST_NOP) return block;
    if (!block) return stmt;

    if (block->tag != AST_BLOCK) {
        AstNode *b = make_block_node(p, block->lineno);
        ast_add_child(p->cs, b, block);
        ast_add_child(p->cs, b, stmt);
        return b;
    }
    ast_add_child(p->cs, block, stmt);
    return block;
}

/* ----------------------------------------------------------------
 * Type helpers
 * ---------------------------------------------------------------- */

/* Get the common type for binary operations */
/* Parse a type name token (BYTE, UBYTE, INTEGER, etc.) */
static TypeInfo *parse_type_name(Parser *p) {
    BTokenType tt = p->current.type;
    BasicType bt = TYPE_unknown;

    switch (tt) {
        case BTOK_BYTE:     bt = TYPE_byte; break;
        case BTOK_UBYTE:    bt = TYPE_ubyte; break;
        case BTOK_INTEGER:  bt = TYPE_integer; break;
        case BTOK_UINTEGER: bt = TYPE_uinteger; break;
        case BTOK_LONG:     bt = TYPE_long; break;
        case BTOK_ULONG:    bt = TYPE_ulong; break;
        case BTOK_FIXED:    bt = TYPE_fixed; break;
        case BTOK_FLOAT:    bt = TYPE_float; break;
        case BTOK_STRING:   bt = TYPE_string; break;
        default:
            /* Could be a user-defined type name */
            if (tt == BTOK_ID && p->current.sval) {
                TypeInfo *t = symboltable_get_type(p->cs->symbol_table, p->current.sval);
                if (t) {
                    advance(p);
                    return type_new_ref(p->cs, t, p->previous.lineno, false);
                }
            }
            return NULL;
    }

    advance(p);
    TypeInfo *t = p->cs->symbol_table->basic_types[bt];
    return type_new_ref(p->cs, t, p->previous.lineno, false);
}

/* Resolve a TypeInfo to its final BasicType (mirrors compiler.c
 * resolve_basic_type / ast.c type_is_string's final_type unwrap). */
static BasicType typeref_basic(const TypeInfo *t) {
    if (!t) return TYPE_unknown;
    const TypeInfo *f = t->final_type ? t->final_type : t;
    return f->basic_type;
}

/* Parse optional "AS type" clause. Returns NULL if no AS found. */
static TypeInfo *parse_typedef(Parser *p) {
    if (!match(p, BTOK_AS)) return NULL;
    TypeInfo *t = parse_type_name(p);
    if (!t) {
        /* Accept any identifier as a type name (forward ref / user type) */
        if (check(p, BTOK_ID) && p->current.sval) {
            const char *name = p->current.sval;
            advance(p);
            t = type_new(p->cs, name, p->previous.lineno);
            return t;
        }
        parser_error(p, "Expected type name after AS");
        return NULL;
    }
    return t;
}

/* ----------------------------------------------------------------
 * Expression parser (Pratt)
 *
 * Precedence table matches Python's:
 *   OR < AND < XOR < NOT < comparisons < BOR < BAND/BXOR/SHL/SHR
 *   < BNOT/+/- < MOD < star/slash < UMINUS < ^
 * ---------------------------------------------------------------- */

static Precedence get_precedence(BTokenType type) {
    switch (type) {
        case BTOK_OR:       return PREC_OR;
        case BTOK_AND:      return PREC_AND;
        case BTOK_XOR:      return PREC_XOR;
        case BTOK_LT: case BTOK_GT: case BTOK_EQ:
        case BTOK_LE: case BTOK_GE: case BTOK_NE:
                            return PREC_COMPARISON;
        case BTOK_BOR:      return PREC_BOR;
        case BTOK_BAND: case BTOK_BXOR:
        case BTOK_SHL: case BTOK_SHR:
                            return PREC_BAND;
        case BTOK_PLUS: case BTOK_MINUS:
                            return PREC_BNOT_ADD;
        case BTOK_MOD:      return PREC_MOD;
        case BTOK_MUL: case BTOK_DIV:
                            return PREC_MULDIV;
        case BTOK_POW:      return PREC_POWER;
        default:            return PREC_NONE;
    }
}

static const char *operator_name(BTokenType type) {
    switch (type) {
        case BTOK_PLUS:  return "PLUS";
        case BTOK_MINUS: return "MINUS";
        case BTOK_MUL:   return "MULT";
        case BTOK_DIV:   return "DIV";
        case BTOK_MOD:   return "MOD";
        case BTOK_POW:   return "POW";
        case BTOK_EQ:    return "EQ";
        case BTOK_LT:    return "LT";
        case BTOK_GT:    return "GT";
        case BTOK_LE:    return "LE";
        case BTOK_GE:    return "GE";
        case BTOK_NE:    return "NE";
        case BTOK_OR:    return "OR";
        case BTOK_AND:   return "AND";
        case BTOK_XOR:   return "XOR";
        case BTOK_BOR:   return "BOR";
        case BTOK_BAND:  return "BAND";
        case BTOK_BXOR:  return "BXOR";
        case BTOK_SHL:   return "SHL";
        case BTOK_SHR:   return "SHR";
        default:         return "?";
    }
}

/* Is this operator right-associative? */
static bool is_right_assoc(BTokenType type) {
    return type == BTOK_POW;
}

/* Forward declarations for expression parsing */
static AstNode *parse_call_or_array(Parser *p, const char *name, int lineno, bool expr_context, bool addressof_ctx);
static AstNode *make_call_node(Parser *p, const char *name, int lineno,
                               AstNode *arglist, bool expr_context,
                               bool addressof_ctx, bool next_is_lp);
static AstNode *make_builtin_node(Parser *p, AstNode *n, AstNode *arg,
                                  BTokenType kw, int lineno);
static AstNode *parse_arglist(Parser *p);

/* Forward decls used by the SymbolARRAYACCESS.offset port (computed at
 * ARRAYACCESS-node construction, defined alongside the BOUND helpers). */
static bool zxbc_eval_to_num(const AstNode *n, double *out);
static void compute_arrayaccess_offset(Parser *p, AstNode *acc,
                                       AstNode *entry, AstNode *arglist);

/* Port of the constant-folding tail of Python's STRSLICE.make_node
 * (src/symbols/strslice.py:84-113).  `slice` is a freshly-built
 * AST_STRSLICE with children [s, lower, upper]; lower/upper have already
 * had OPTIONS.string_base subtracted and been TYPECAST to STR_INDEX_TYPE
 * (uinteger) by the caller, mirroring make_node:74-83.  This reproduces
 * the four constant-fold outcomes Python applies before returning a real
 * SymbolSTRSLICE:
 *   1. constant bound clamping (lower -> >= MIN_STRSLICE_IDX=0,
 *      upper -> <= MAX_STRSLICE_IDX=65534), mutating the NUMBER in place;
 *   2. lo > up                 -> STRING("")           (empty slice);
 *   3. s is a constant string  -> STRING(s.value[lo:up+1]);
 *   4. lo==0 and up==65534     -> s   (the no-op full slice `a$( TO )`).
 * Otherwise the STRSLICE node is returned unchanged.  Constant detection
 * uses check.is_number (NUMBER or numeric CONST), exactly as Python. */
static AstNode *strslice_fold(Parser *p, AstNode *slice) {
    if (!slice || slice->child_count < 3) return slice;
    AstNode *s     = slice->children[0];
    AstNode *lower = slice->children[1];
    AstNode *upper = slice->children[2];

    bool lo_num = check_is_number(lower);
    bool up_num = check_is_number(upper);
    long lo = 0, up = 0;

    if (lo_num) {
        double v;
        if (!zxbc_eval_to_num(lower, &v)) {
            lo_num = false;
        } else {
            lo = (long)v;
            if (lo < 0) {                 /* MIN_STRSLICE_IDX == 0 */
                lo = 0;
                if (lower->tag == AST_NUMBER) lower->u.number.value = 0;
            }
        }
    }
    if (up_num) {
        double v;
        if (!zxbc_eval_to_num(upper, &v)) {
            up_num = false;
        } else {
            up = (long)v;
            if (up > 65534) {             /* MAX_STRSLICE_IDX == 65534 */
                up = 65534;
                if (upper->tag == AST_NUMBER) upper->u.number.value = 65534;
            }
        }
    }

    if (lo_num && up_num) {
        if (lo > up)
            return make_string(p, "", slice->lineno);

        /* s.token in ("STRING", "CONST"): a constant string -> slice now.
         * STRING literal (AST_STRING) or a CLASS_const string id whose
         * default value is a string literal. */
        const char *sval = NULL;
        if (s->tag == AST_STRING) {
            sval = s->u.string.value;
        } else if (s->tag == AST_ID && s->u.id.class_ == CLASS_const &&
                   type_is_string(s->type_) && s->u.id.default_value_expr &&
                   s->u.id.default_value_expr->tag == AST_STRING) {
            sval = s->u.id.default_value_expr->u.string.value;
        }
        if (sval) {
            long n = (long)strlen(sval);
            long a = lo;
            long b = up + 1;              /* Python: up += 1 */
            if (a > n) a = n;
            if (b > n) b = n;
            if (b < a) b = a;
            char *buf = arena_alloc(&p->cs->arena, (size_t)(b - a) + 1);
            memcpy(buf, sval + a, (size_t)(b - a));
            buf[b - a] = '\0';
            return make_string(p, buf, slice->lineno);
        }

        /* a$(0 TO INF.) == a$ : the no-op full slice. */
        if (lo == 0 && up == 65534)
            return s;
    }
    return slice;
}

/* make_strslice (zxbparser.py:243-245 -> STRSLICE.make_node, strslice.py:
 * 59-111): build a string-slice node from a string base `s` and (lower,
 * upper) bounds. Faithful port of make_node:
 *   - any of s/lower/upper NULL -> NULL;
 *   - check_type(string, s): the base must be string, else "Wrong expression
 *     type '<t>'. Expected 'string'" at lineno + NULL;
 *   - each bound := TYPECAST(STR_INDEX_TYPE=uinteger,
 *                            MINUS(bound, NUMBER(OPTIONS.string_base)));
 *   - then strslice_fold (the constant clamp / empty / const-string / no-op
 *     full-slice tail).
 * This is the read-side STRSLICE builder the substr-consuming reduces share;
 * it produces the SAME [s, lower_cast, upper_cast] (or folded) tree the
 * production parser's inline `name$(from TO to)` path yields (parser.c:
 * 2656-2814), so the engine matches the production C-vs-C. `lower`/`upper`
 * are the substr-tuple bounds (already uinteger-pre-cast by the substr
 * reduces, mirroring the `substr` non-terminal at zxbparser.py:2600-2638). */
static AstNode *make_strslice_node(Parser *p, AstNode *s, AstNode *lower,
                                   AstNode *upper, int lineno) {
    if (!s || !lower || !upper) return NULL;
    /* check_type(lineno, Type.string, s) (strslice.py:70). */
    if (!type_is_string(s->type_)) {
        const TypeInfo *sft = (s->type_ && s->type_->final_type)
                                  ? s->type_->final_type : s->type_;
        const char *stn = (sft && sft->tag == AST_BASICTYPE)
                              ? basictype_to_string(sft->basic_type) : "unknown";
        zxbc_error(p->cs, lineno,
                   "Wrong expression type '%s'. Expected 'string'", stn);
        return NULL;
    }
    TypeInfo *str_idx = p->cs->symbol_table->basic_types[TYPE_uinteger];
    AstNode *lo_sub = make_binary_node(
        p->cs, "MINUS", lower,
        make_number(p, p->cs->opts.string_base, lineno, NULL), lineno, NULL);
    AstNode *lo = make_typecast(p->cs, str_idx, lo_sub, lineno);
    AstNode *up_sub = make_binary_node(
        p->cs, "MINUS", upper,
        make_number(p, p->cs->opts.string_base, lineno, NULL), lineno, NULL);
    AstNode *up = make_typecast(p->cs, str_idx, up_sub, lineno);
    if (!lo || !up) return NULL;  /* make_node:86-87 */
    AstNode *slice = ast_new(p->cs, AST_STRSLICE, lineno);
    ast_add_child(p->cs, slice, s);
    ast_add_child(p->cs, slice, lo);
    ast_add_child(p->cs, slice, up);
    slice->type_ = p->cs->symbol_table->basic_types[TYPE_string];
    return strslice_fold(p, slice);
}

/* make_strslice over a string SCALAR VARIABLE `s$(...)` — the CLASS_var-string
 * branch of make_call (zxbparser.py:399-411 / parse_call_or_array). Builds the
 * STRSLICE[entry, bound...] node directly from the arglist (each ARGUMENT's
 * bound is base-subtracted + uinteger-typecast). Handles BOTH the READ
 * (expr_context — `Print s$(n)`) and the single-index WRITE lvalue
 * (!expr_context — `s$(n) = rhs`, p_substr_assignment_no_let) shapes, which
 * differ in the MINUS application (see the inline comment). Extracted from
 * parse_call_or_array (behaviour-preserving) so the Phase-D substr-write
 * reduce-actions (80/81) build the byte-identical lvalue. */
static AstNode *make_strslice_var(Parser *p, AstNode *entry, AstNode *arglist,
                                  bool expr_context, int lineno) {
    /* S5.8a: STRSLICE-node construction does NOT mark the string accessed in
     * Python (only the READ path does, via mark_entry_as_accessed). The
     * substring lvalue write-target productions resolve only via
     * access_call/access_var (never .accessed) — gate on expr_context so a
     * write-only string's `accessed` stays False (O>1 DCE prunes it, matching
     * Python). */
    AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
    if (expr_context)
        entry->u.id.accessed = true;
    ast_add_child(p->cs, n, entry);
    /* OPTIONS.string_base adjustment + STR_INDEX_TYPE promotion —
     * STRSLICE.make_node (symbols/strslice.py:74-83) wraps each slice bound in
     * `TYPECAST(STR_INDEX_TYPE, MINUS(<idx>, <base>))`. The TYPECAST(uinteger)
     * is UNCONDITIONAL (widens a small constant index to 16-bit). The MINUS:
     *   READ (expr_context): UNCONDITIONAL, base auto-inferred (NULL) — exactly
     *     Python's bare NUMBER(string_base); folds `idx-0` for a constant.
     *   WRITE (!expr_context): only when string_base != 0, base typed str_idx —
     *     p_substr_assignment_no_let (zxbparser.py:1221-1245); for base 0 the
     *     translator supplies it. */
    int strbase = p->cs->opts.string_base;
    TypeInfo *str_idx = p->cs->symbol_table->basic_types[TYPE_uinteger];
    for (int i = 0; i < arglist->child_count; i++) {
        AstNode *ch = arglist->children[i];
        if (ch) {
            AstNode *idx = ch;
            AstNode *parent = NULL;
            int parent_idx = -1;
            if (ch->tag == AST_ARGUMENT && ch->child_count > 0) {
                idx = ch->children[0];
                parent = ch;
                parent_idx = 0;
            }
            AstNode *val = idx;
            if (expr_context) {
                AstNode *base = make_number(p, strbase, lineno, NULL);
                val = make_binary_node(p->cs, "MINUS", idx, base, lineno, NULL);
            } else if (strbase != 0) {
                AstNode *base = make_number(p, strbase, lineno, str_idx);
                val = make_binary_node(p->cs, "MINUS", val, base, lineno, NULL);
            }
            AstNode *cast = make_typecast(p->cs, str_idx, val, lineno);
            if (cast) {
                if (parent) {
                    parent->children[parent_idx] = cast;
                    parent->type_ = cast->type_;
                } else {
                    ch = cast;
                }
            }
        }
        ast_add_child(p->cs, n, ch);
    }
    n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
    return n;
}

/* Build the LETARRAYSUBSTR lvalue for a string-ARRAY element substring
 * assignment — the single-paren-group / comma-subscript family that
 * Python parses with its dedicated p_let_arr_substr* productions
 * (zxbparser.py:2733-2815) and the p_let_arr string branch
 * (zxbparser.py:1199-1205), all routed through make_array_substr_assign
 * (zxbparser.py:328-362 -> "LETARRAYSUBSTR").
 *
 * The C port has no separate LETARRAYSUBSTR sentence; tr_visit_letarray
 * (translator.c:3290) detects "Shape A": an AST_STRSLICE lvalue whose
 * child[0] is an ARRAYACCESS (the array element a$(dims)) and child[1] is
 * an inner AST_STRSLICE [lower, upper] (the substring bounds), and routes
 * it to tr_letarraysubstr_emit.  This helper builds exactly that node so
 * the single-paren shapes share the postfix shape's translation path.
 *
 * Mirrors make_array_substr_assign:
 *   - access_call(id_)  -> entry  (already resolved by caller, passed in)
 *   - entry.type_ != string -> "Array '%s' is not of type String" (:336-337)
 *   - make_array_access(id_, dim arglist) -> the ARRAYACCESS over the
 *     DIM subscripts only (the substring index already split off); this
 *     runs the BOUND_TYPE typecast + dim-count check on the dim args.
 * `dim_args` is a list of the dim subscript nodes (each an AST_ARGUMENT
 * or a bare expr); `lower`/`upper` are the substring bounds.  Returns the
 * STRSLICE lvalue, or NULL after emitting the matching Python error. */
static void compute_arrayaccess_offset(Parser *p, AstNode *acc,
                                       AstNode *entry, AstNode *arglist);
static AstNode *build_array_substr_lvalue(Parser *p, const char *name,
                                          int lineno, AstNode **dim_args,
                                          int ndim_args, AstNode *lower,
                                          AstNode *upper) {
    /* access_call (zxbparser.py:332) — resolves/auto-declares the id. */
    AstNode *entry = symboltable_access_call(p->cs->symbol_table, p->cs,
                                             name, lineno, NULL);
    if (entry == NULL)
        return NULL;

    /* make_array_substr_assign:336-338 — element type must be String. */
    if (!type_is_string(entry->type_)) {
        zxbc_error(p->cs, lineno, "Array '%s' is not of type String", name);
        return NULL;
    }

    /* make_array_access (zxbparser.py:311-325) over the DIM args only.
     * Build the dim arglist, BOUND_TYPE-typecast each subscript (:316),
     * run the dim-count check (arrayaccess.py:100-104), then build the
     * AST_ARRAYACCESS [entry, arg0, arg1, ...] and compute its constant
     * offset — identical to the CLASS_array branch of parse_call_or_array. */
    AstNode *dim_arglist = ast_new(p->cs, AST_ARGLIST, lineno);
    TypeInfo *bound_type = p->cs->symbol_table->basic_types[TYPE_uinteger];
    for (int i = 0; i < ndim_args; i++) {
        AstNode *arg = dim_args[i];
        if (arg && arg->tag == AST_ARGUMENT && arg->child_count > 0) {
            AstNode *cast = make_typecast(p->cs, bound_type,
                                          arg->children[0], lineno);
            if (!cast) return NULL;  /* make_array_access returns None */
            arg->children[0] = cast;
            arg->type_ = cast->type_;
        } else if (arg) {
            /* Bare expr — wrap in an ARGUMENT for the ARRAYACCESS shape. */
            AstNode *cast = make_typecast(p->cs, bound_type, arg, lineno);
            if (!cast) return NULL;
            AstNode *wrap = ast_new(p->cs, AST_ARGUMENT, lineno);
            wrap->u.argument.byref = p->cs->opts.default_byref;
            ast_add_child(p->cs, wrap, cast);
            wrap->type_ = cast->type_;
            arg = wrap;
        }
        ast_add_child(p->cs, dim_arglist, arg);
    }

    /* arrayaccess.py:100-104 — len(bounds) != len(args) for a non-param
     * array -> "Array '%s' has %i dimensions, not %i".  The substring
     * index is already popped, so this sees the true dim count. */
    if (entry->u.id.scope != SCOPE_parameter) {
        int ndecl = entry->u.id.arr_boundlist
                        ? entry->u.id.arr_boundlist->child_count : 0;
        if (ndecl != dim_arglist->child_count) {
            zxbc_error(p->cs, lineno, "Array '%s' has %d dimensions, not %d",
                       entry->u.id.name, ndecl, dim_arglist->child_count);
            return NULL;
        }
    }

    /* SymbolARRAYACCESS.__init__ (arrayaccess.py:34-37). */
    entry->u.id.is_dynamically_accessed = true;

    AstNode *acc = ast_new(p->cs, AST_ARRAYACCESS, lineno);
    acc->u.arrayaccess.is_load = false;   /* write target (LETARRAY lvalue) */
    ast_add_child(p->cs, acc, entry);
    for (int i = 0; i < dim_arglist->child_count; i++)
        ast_add_child(p->cs, acc, dim_arglist->children[i]);
    acc->type_ = entry->type_;
    compute_arrayaccess_offset(p, acc, entry, dim_arglist);

    /* Wrap as Shape A: STRSLICE[ ARRAYACCESS, STRSLICE[lower, upper] ].
     * tr_visit_letarray (translator.c:3290) reads exactly this shape. */
    AstNode *inner = ast_new(p->cs, AST_STRSLICE, lineno);
    ast_add_child(p->cs, inner, lower);
    ast_add_child(p->cs, inner, upper);

    AstNode *lv = ast_new(p->cs, AST_STRSLICE, lineno);
    lv->type_ = entry->type_;
    ast_add_child(p->cs, lv, acc);
    ast_add_child(p->cs, lv, inner);
    return lv;
}

/* Parse builtin function: ABS, SIN, COS, etc. */
static AstNode *parse_builtin_func(Parser *p, const char *fname, BTokenType kw) {
    int lineno = p->previous.lineno;

    /* Some builtins take no args */
    if (kw == BTOK_RND) {
        if (match(p, BTOK_LP)) consume(p, BTOK_RP, "Expected ')' after RND(");
        AstNode *n = ast_new(p->cs, AST_BUILTIN, lineno);
        n->u.builtin.fname = arena_strdup(&p->cs->arena, "RND");
        n->type_ = p->cs->symbol_table->basic_types[TYPE_float];
        return n;
    }
    if (kw == BTOK_INKEY) {
        if (match(p, BTOK_LP)) consume(p, BTOK_RP, "Expected ')' after INKEY$(");
        AstNode *n = ast_new(p->cs, AST_BUILTIN, lineno);
        n->u.builtin.fname = arena_strdup(&p->cs->arena, "INKEY");
        n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        return n;
    }
    if (kw == BTOK_PI) {
        return make_number(p, M_PI, lineno, p->cs->symbol_table->basic_types[TYPE_float]);
    }

    /* Builtins with parenthesized argument(s): FUNC(expr [, expr ...]) or FUNC expr */
    bool had_paren = match(p, BTOK_LP);

    /* Special case: PEEK(type, addr) and POKE type addr, val (when used as expression) */
    AstNode *n = ast_new(p->cs, AST_BUILTIN, lineno);
    n->u.builtin.fname = arena_strdup(&p->cs->arena, fname);

    if (had_paren && (kw == BTOK_PEEK)) {
        /* Check if first arg is a type name */
        TypeInfo *peek_type = parse_type_name(p);
        if (peek_type) {
            /* PEEK(type, addr) — p_expr_peektype_ (zxbparser.py:3302-3304)
             * make_typecast(uinteger, addr): the address must be a 16-bit
             * pointer; a Float/Long addr needs the runtime narrow. */
            consume(p, BTOK_COMMA, "Expected ',' after PEEK type");
            AstNode *addr = parse_expression(p, PREC_NONE + 1);
            if (addr) {
                AstNode *cast = make_typecast(p->cs,
                    p->cs->symbol_table->basic_types[TYPE_uinteger],
                    addr, lineno);
                ast_add_child(p->cs, n, cast ? cast : addr);
            }
            n->type_ = peek_type;
            consume(p, BTOK_RP, "Expected ')' after PEEK arguments");
            return n;
        }
    }

    /* Without parens, builtins bind at unary precedence (matching Python %prec UMINUS).
     * e.g. LEN x - 1 parses as (LEN x) - 1, not LEN(x - 1). */
    AstNode *arg = parse_expression(p, had_paren ? PREC_NONE + 1 : PREC_UNARY);
    if (!arg) {
        if (had_paren) consume(p, BTOK_RP, "Expected ')' after builtin argument");
        return n;
    }
    ast_add_child(p->cs, n, arg);

    /* Handle multi-arg builtins (CHR$, LBOUND, UBOUND, etc.) */
    int closing_rp_lineno = lineno;
    if (had_paren) {
        while (match(p, BTOK_COMMA)) {
            AstNode *extra = parse_expression(p, PREC_NONE + 1);
            if (extra) ast_add_child(p->cs, n, extra);
        }
        consume(p, BTOK_RP, "Expected ')' after builtin argument");
        closing_rp_lineno = p->previous.lineno;  /* p.lineno(6) — the RP */
    }

    /* p_expr_lbound / p_expr_lbound_expr (zxbparser.py:3317-3378) —
     * LBOUND/UBOUND, both the no-dimension form `LBOUND(arr)` and the
     * explicit-dimension form `LBOUND(arr, expr)`. The C grammar funnels
     * both through this generic builtin builder, so the production's
     * constant-fold AND its array-descriptor side effect must be
     * reproduced here.
     *
     * No-dimension form (p_expr_lbound, :3317-3332): if the array scope is
     * parameter → make_builtin(p[1], [entry, NUMBER(0)]) (runtime, dim
     * arg 0). Otherwise → make_number(len(entry.bounds)) — i.e. BOTH
     * LBOUND(arr) and UBOUND(arr) constant-fold to the array's DIMENSION
     * COUNT (Python returns the rank for the no-arg form, not the first
     * bound). The C parses the bare ARRAY_ID into n->children[0] (an
     * AST_ID, CLASS_array) and no second child.
     *
     * Explicit-dimension form (p_expr_lbound_expr, :3335-3378):
     * num = make_typecast(uinteger, expr). If is_number(num) and the array
     * scope is local/global, the result constant-propagates:
     *     val == 0           -> len(entry.bounds)        (dim count)
     *     val, LBOUND        -> bounds[val-1].lower
     *     val, UBOUND        -> bounds[val-1].upper
     * and an out-of-range val (<0 or >len(bounds)) is a "Dimension out of
     * range" error. Otherwise (non-constant dim, or a parameter array) it
     * sets entry.ref.lbound_used / .ubound_used — the gate
     * VarTranslator.visit_ARRAYDECL (var_translator.py:58/61) reads to
     * emit the `<mangled>.__LBOUND__` / `.__UBOUND__` descriptor slot +
     * trailing bound table for a non-zero-based array (var_translator.c
     * :530-544 consume these flags) — and keeps the runtime BUILTIN node.
     *
     * make_number is typed TYPE.uinteger exactly like Python (3330/3332/
     * 3366/3368/3370) so the enclosing LET's typecast/codegen matches. The
     * array entry was already marked accessed when parse_primary resolved
     * the bare ARRAY_ID (parser.c:1442), mirroring Python's
     * mark_entry_as_accessed (zxbparser.py:3326/3349) — so the folded
     * NUMBER still keeps the array's DIM emission alive. */
    if (kw == BTOK_LBOUND || kw == BTOK_UBOUND) {
        AstNode *arr = n->child_count > 0 ? n->children[0] : NULL;
        if (arr && arr->tag == AST_ID && arr->u.id.class_ == CLASS_array) {
            AstNode *bl = arr->u.id.arr_boundlist;
            int nbounds = bl ? bl->child_count : 0;
            TypeInfo *uint_t =
                p->cs->symbol_table->basic_types[TYPE_uinteger];

            if (n->child_count < 2) {
                /* p_expr_lbound (no-dimension form). */
                if (arr->u.id.scope == SCOPE_parameter) {
                    /* Parameter array: make_builtin(p[1],
                     * [entry, NUMBER(0)]) — keep the runtime BUILTIN with
                     * an explicit dim-0 argument (Python :3329-3330). */
                    AstNode *zero = make_number(p, 0, lineno, uint_t);
                    if (zero) ast_add_child(p->cs, n, zero);
                } else {
                    /* Non-parameter: fold to the dimension COUNT. */
                    return make_number(p, nbounds, lineno, uint_t);
                }
            } else {
                /* p_expr_lbound_expr (explicit-dimension form). */
                AstNode *dim = n->children[1];
                AstNode *num = make_typecast(p->cs, uint_t, dim, lineno);
                if (num) n->children[1] = num;
                /* Python's const-prop branch additionally requires the
                 * array scope to be local/global; a parameter-scope array
                 * always takes the flag-setting else-branch. */
                bool const_dim =
                    num && (arr->u.id.scope == SCOPE_global ||
                            arr->u.id.scope == SCOPE_local) &&
                    check_is_number(num);
                if (const_dim) {
                    long val = (long) num->u.number.value;
                    if (val < 0 || val > nbounds) {
                        zxbc_error(p->cs, closing_rp_lineno,
                                   "Dimension out of range");
                        return NULL;
                    }
                    if (val == 0) {
                        /* val == 0 -> number of dimensions. */
                        return make_number(p, nbounds, lineno, uint_t);
                    }
                    /* bounds[val-1] geometry: BOUND child[0]=lower NUMBER,
                     * child[1]=upper NUMBER (parser.c:2099-2102;
                     * bound.py:33-37). */
                    AstNode *bd = bl->children[val - 1];
                    long lo = (bd && bd->child_count > 0 &&
                               bd->children[0]->tag == AST_NUMBER)
                                  ? (long) bd->children[0]->u.number.value
                                  : 0;
                    long hi = (bd && bd->child_count > 1 &&
                               bd->children[1]->tag == AST_NUMBER)
                                  ? (long) bd->children[1]->u.number.value
                                  : 0;
                    return make_number(
                        p, kw == BTOK_LBOUND ? lo : hi, lineno, uint_t);
                } else {
                    if (kw == BTOK_LBOUND)
                        arr->u.id.lbound_used = true;
                    else
                        arr->u.id.ubound_used = true;
                }
            }
        }
    }

    return make_builtin_node(p, n, arg, kw, lineno);
}

/* Builtin-node build core (extracted from parse_builtin_func so the Phase-D
 * single-arg builtin-bexpr reduce-actions build the byte-for-byte same node).
 * Takes the AST_BUILTIN `n` (fname set, arg attached as child[0]) + `arg`
 * (== n->children[0]) + `kw` + `lineno`. Performs the per-builtin constant
 * folds (LEN/INT/ABS/STR/VAL/SGN/trig math) and result-type assignment, and
 * returns the final node (possibly a folded NUMBER/STRING, or `n`). Faithful
 * to the p_len/p_expr_int/p_abs/p_str/p_val/p_sgn/p_expr_trig/... actions.
 * Behaviour-preserving extraction. */
static AstNode *make_builtin_node(Parser *p, AstNode *n, AstNode *arg,
                                  BTokenType kw, int lineno) {
    /* p_len (zxbparser.py:3381-3394) — full faithful port of the LEN
     * production, in Python's exact branch order:
     *
     *     bexpr : LEN bexpr %prec UMINUS
     *     arg = p[2]
     *     if arg is None:                              -> p[0] = None
     *     elif arg.token == "VAR" and arg.class_ == CLASS.array:
     *         p[0] = make_number(len(arg.bounds), p.lineno(1))   # dims fold
     *     elif arg.type_ != TYPE.string:
     *         syntax_error_expected_string(...); p[0] = None     # non-string
     *     elif is_string(arg):                                   # const str
     *         p[0] = make_number(len(arg.value), p.lineno(1))    # length fold
     *     else:
     *         p[0] = make_builtin(p.lineno(1), "LEN", arg, type_=uinteger)
     *
     * Both folds call make_number WITHOUT a type_, so the NUMBER re-infers
     * its implicit type from the value (make_number above, NULL arg) — e.g.
     * LEN("ZXBASIC") -> 7 infers ubyte, so the enclosing LET stores a 1-byte
     * scalar and the implicit-type W100 says 'ubyte', exactly like Python
     * (the runtime BUILTIN-node path is uinteger -> 2-byte / 'uinteger').
     *
     * The array branch folds to the DIMENSION COUNT (len(bounds)), like
     * LBOUND(arr)/UBOUND(arr) above; arr_boundlist->child_count == len(arg.
     * bounds).  is_string is exactly check.is_string single-arg form: a bare
     * STRING literal, or a CLASS_const id of string type (constref value is
     * the folded STRING) — so a runtime string VARIABLE (AST_ID, CLASS_var)
     * is NOT folded and takes the else branch (runtime __STRLEN), matching
     * Python. */
    if (kw == BTOK_LEN) {
        AstNode *larg = n->child_count > 0 ? n->children[0] : NULL;
        if (larg) {
            /* Branch 1: LEN(<array variable>) -> #dimensions, folded. */
            if (larg->tag == AST_ID && larg->u.id.class_ == CLASS_array) {
                AstNode *bl = larg->u.id.arr_boundlist;
                int nbounds = bl ? bl->child_count : 0;
                return make_number(p, nbounds, lineno, NULL);
            }
            /* Branch 2: non-string, non-array -> expected-string error. */
            if (!type_is_string(larg->type_)) {
                const TypeInfo *aft =
                    (larg->type_ && larg->type_->final_type)
                        ? larg->type_->final_type : larg->type_;
                const char *atn = (aft && aft->tag == AST_BASICTYPE)
                                      ? basictype_to_string(aft->basic_type)
                                      : "unknown";
                err_expected_string(p->cs, lineno, atn);
                return NULL;
            }
            /* Branch 3: LEN(<constant string>) -> length, folded.
             * is_string single-arg: a STRING literal or a CLASS_const id of
             * string type.  const_string_value_node resolves either to the
             * underlying AST_STRING whose .length is len(arg.value). */
            const AstNode *sv = const_string_value_node(larg);
            if (sv && sv->tag == AST_STRING) {
                return make_number(p, sv->u.string.length, lineno, NULL);
            }
            /* Branch 4 (else): runtime LEN — fall through to the
             * result-type switch below, which sets type_=uinteger and keeps
             * the BUILTIN node (__STRLEN). */
        }
    }

    /* p_expr_int (zxbparser.py:3540-3542): INT is NOT a builtin — it is
     * exactly make_typecast(TYPE.long_, p[2], lineno). Build the
     * TYPECAST so codegen runs the faithful long conversion (the
     * BUILTIN-node path would mis-emit, e.g. cast_i32tou32/ltee8). */
    if (kw == BTOK_INT) {
        return make_typecast(p->cs,
                             p->cs->symbol_table->basic_types[TYPE_long],
                             arg, lineno);
    }

    /* p_abs (zxbparser.py:3545-3552): ABS of an unsigned value is the
     * value itself (redundant); ABS of a numeric constant folds via
     * make_builtin/make_node (builtin.py:74-77). Only the BUILTIN-node
     * path (signed non-const) reaches visit_BUILTIN. */
    if (kw == BTOK_ABS && arg->type_) {
        const TypeInfo *ft = arg->type_->final_type ? arg->type_->final_type
                                                     : arg->type_;
        if (ft->tag == AST_BASICTYPE &&
            basictype_is_unsigned(ft->basic_type)) {
            return arg;  /* is_unsigned -> p[0] = p[2] */
        }
        if (arg->tag == AST_NUMBER) {
            /* make_node fold (builtin.py:74-77): SymbolNUMBER(func(v),
             * type_=None) — type_ not passed to make_builtin in p_abs,
             * so the folded NUMBER re-infers its type from the value. */
            double val = arg->u.number.value;
            return make_number(p, val >= 0 ? val : -val, lineno, NULL);
        }
    }

    /* p_str constant-fold (zxbparser.py:3409-3412):
     *     string : STR expr %prec UMINUS
     *     if is_number(p[2]):  # NUMBER or numeric CONST (check.py:312-316)
     *         p[0] = sym.STRING(str(p[2].value), p.lineno(1))
     * A compile-time numeric argument folds directly to a STRING literal —
     * the constant's value rendered via SymbolNUMBER.__str__ (the same
     * normalised int-or-float str() py_number_str reproduces).  This must
     * run BEFORE the float typecast / BUILTIN-node construction below: the
     * folded STRING takes the static __STORE_STR path (a .LABEL data block)
     * instead of the runtime __STR_FAST + __STORE_STR2 the BUILTIN node
     * lowers to.  is_number is exactly check_is_number (NUMBER node or a
     * numeric CLASS_const id), so a non-constant variable STAYS runtime
     * (not over-folded), matching Python's else branch. */
    if (kw == BTOK_STR && check_is_number(arg)) {
        double v;
        if (zxbc_eval_to_num(arg, &v)) {
            char buf[64];
            py_number_str(v, buf, sizeof(buf));
            return make_string(p, buf, lineno);
        }
    }

    /* p_val / p_code (zxbparser.py:3458 / 3478): the argument must be a
     * string; otherwise syntax_error_expected_string (errmsg.py:201) at the
     * keyword line (p.lineno(1)), and p[0]=None. Faithful port of the
     * `if p[2].type_ != TYPE.string` guard. */
    if ((kw == BTOK_VAL || kw == BTOK_CODE) && !type_is_string(arg->type_)) {
        const TypeInfo *aft = (arg->type_ && arg->type_->final_type)
                                  ? arg->type_->final_type : arg->type_;
        const char *atn = (aft && aft->tag == AST_BASICTYPE)
                              ? basictype_to_string(aft->basic_type) : "unknown";
        err_expected_string(p->cs, lineno, atn);
        return NULL;
    }

    /* p_val constant-fold (zxbparser.py:3447-3462 -> builtin.py:74-77):
     * VAL of a COMPILE-TIME-CONSTANT string is evaluated at parse time as
     * float(eval(s, {}, {})) and folded to a FLOAT NUMBER constant — the
     * runtime `.core.VAL` (+ heap/__FTOU32REG) is never emitted.  is_string
     * single-arg (const_string_value_node) gates this: a bare STRING literal
     * or a CLASS_const id of string type folds; a runtime string VARIABLE
     * stays the BUILTIN node (Python's BUILTIN.make_node only folds a
     * constant operand), so a non-constant VAL(s$) is NOT over-folded.
     *
     * val_fold reproduces eval()'s numeric domain (see its definition).  On
     * success it yields the float()-coerced value; on ANY failure (invalid
     * numeric expression, division by zero, big-int overflow, or a string
     * Python's eval() could evaluate but our numeric evaluator can't) we
     * take Python's `except: x = 0` path: warn with the EXACT message
     * "Invalid string numeric constant '<s>' evaluated as 0" and fold to a
     * FLOAT 0.0 NUMBER — byte- and stderr-identical to Python for the
     * genuinely-invalid inputs, and a documented flagged divergence only for
     * the rare non-numeric-but-eval()-able strings realistic VAL never uses. */
    if (kw == BTOK_VAL) {
        const AstNode *sv = const_string_value_node(arg);
        if (sv && sv->tag == AST_STRING) {
            double v;
            const char *str = sv->u.string.value ? sv->u.string.value : "";
            size_t slen = (size_t)(sv->u.string.length >= 0
                                       ? sv->u.string.length : 0);
            if (val_fold(str, slen, &v)) {
                return make_number(p, v, lineno,
                                   p->cs->symbol_table->basic_types[TYPE_float]);
            }
            /* except -> 0 + the faithful warning (uncoded, like Python's
             * bare warning() in p_val.val). */
            zxbc_warning(p->cs, lineno,
                         "Invalid string numeric constant '%s' evaluated as 0",
                         str);
            return make_number(p, 0.0, lineno,
                               p->cs->symbol_table->basic_types[TYPE_float]);
        }
    }

    /* p_sgn (zxbparser.py:3489): a string argument is rejected with the
     * literal "Expected a numeric expression, got TYPE.string instead"
     * (note the verbatim Python enum repr), p[0]=None. */
    if (kw == BTOK_SGN && type_is_string(arg->type_)) {
        zxbc_error(p->cs, lineno,
                   "Expected a numeric expression, got TYPE.string instead");
        return NULL;
    }

    /* Set result type based on function */
    SymbolTable *st = p->cs->symbol_table;
    switch (kw) {
        case BTOK_ABS:
            n->type_ = arg->type_;
            break;
        case BTOK_INT:
            n->type_ = st->basic_types[TYPE_long];
            break;
        case BTOK_SGN:
            n->type_ = st->basic_types[TYPE_byte];
            break;
        case BTOK_SIN: case BTOK_COS: case BTOK_TAN:
        case BTOK_ACS: case BTOK_ASN: case BTOK_ATN:
        case BTOK_EXP: case BTOK_LN: case BTOK_SQR:
            /* p_expr_trig (zxbparser.py:3502-3520): every math fn
             * typecasts its arg to TYPE.float — the runtime trig/log/sqrt
             * routines all operate on the FP register set. Without it a
             * non-float operand (e.g. a UByte var, test 70's `SQR b`)
             * skips the .core.__U8TOFREG conversion and the chained
             * calls read garbage (push 0 / ld a,0). */
            n->type_ = st->basic_types[TYPE_float];
            if (arg && n->child_count > 0) {
                AstNode *cast = make_typecast(p->cs,
                    st->basic_types[TYPE_float], arg, lineno);
                if (cast) n->children[0] = cast;
                /* p_expr_trig fold (zxbparser.py:3502-3520 ->
                 * builtin.py:74-77): make_builtin receives the float-cast
                 * arg; BUILTIN.make_node folds when func!=None, len==1, and
                 * is_number(operand) -> SymbolNUMBER(func(operand.value),
                 * type_=float_).  After the make_typecast above a constant
                 * NUMBER (or numeric CONST id) stays is_number with its
                 * value already float()-cast, so a compile-time argument
                 * folds straight to the resulting FP constant (the shared
                 * Z80 float encoder then emits the 5-byte literal) instead
                 * of the runtime `call .core.SIN` + FP-stack.  is_number is
                 * exactly check_is_number — a non-constant variable STAYS
                 * runtime (Python's else branch keeps the BUILTIN node). */
                AstNode *cn = n->children[0];
                double av, fv;
                if (check_is_number(cn) && zxbc_eval_to_num(cn, &av) &&
                    math_fn_fold(kw, av, &fv)) {
                    /* Domain/range error parity: Python's math.* RAISES
                     * (uncaught -> compiler exits 1) exactly when the IEEE
                     * result would be NaN/Inf (e.g. ASN 2, ACS 2, LN 0,
                     * SQR(-1), EXP 710).  C's libm returns NaN/Inf instead,
                     * so guard isfinite: fold only a finite result; on a
                     * non-finite result fail the compile (exit 1) to match
                     * Python rather than fold a bogus constant — without
                     * this an Inf NUMBER also hangs the FP encoder. */
                    if (isfinite(fv)) {
                        return make_number(p, fv, lineno,
                                           st->basic_types[TYPE_float]);
                    }
                    zxbc_error(p->cs, lineno, "math domain error");
                    return NULL;
                }
            }
            break;
        case BTOK_LEN:
            n->type_ = st->basic_types[TYPE_uinteger];
            break;
        case BTOK_PEEK:
            /* p_expr_peek (zxbparser.py:3292-3299): make_typecast(uinteger,
             * addr) — narrow a Float/Long address to a 16-bit pointer
             * (lcd3: PEEK adr where adr is an implicit Float needs
             * .core.__FTOU32REG; without it the C reads adr's low 2
             * bytes as the pointer). */
            n->type_ = st->basic_types[TYPE_ubyte];
            if (arg && n->child_count > 0) {
                AstNode *cast = make_typecast(p->cs,
                    st->basic_types[TYPE_uinteger], arg, lineno);
                if (cast) n->children[0] = cast;
            }
            break;
        case BTOK_CODE:
            n->type_ = st->basic_types[TYPE_ubyte];
            break;
        case BTOK_USR:
            /* p_expr_usr (zxbparser.py:3272-3282): if string -> USR_STR
             * (keep STRING child unchanged); otherwise USR with the
             * operand make_typecast'd to uinteger so a ubyte/integer
             * argument widens correctly (opt1_usr-class: USR ubyte). */
            n->type_ = st->basic_types[TYPE_uinteger];
            if (arg && arg->type_ &&
                type_is_string(arg->type_)) {
                n->u.builtin.fname =
                    arena_strdup(&p->cs->arena, "USR_STR");
            } else if (arg && n->child_count > 0) {
                AstNode *cast = make_typecast(p->cs,
                                              st->basic_types[TYPE_uinteger],
                                              arg, lineno);
                if (cast) n->children[0] = cast;
            }
            break;
        case BTOK_STR:
            /* p_str (zxbparser.py:3409-3419): the non-constant branch
             * wraps the arg in make_typecast(TYPE.float, p[2]) so the
             * later visit_STR's ic_fparam(float, arg.t) reads a float
             * operand (emit_fparamf consumes 5 bytes as A/DE/BC). For
             * a UByte/Long/etc. source the typecast pre-emits the
             * runtime cast (e.g. .core.__U8TOFREG) — without it the
             * backend reads non-float source bytes as if they were
             * float (strparam1: `ld de, (_b+1); ld bc, (_b+3)` over a
             * UByte). */
            n->type_ = st->basic_types[TYPE_string];
            if (arg && n->child_count > 0) {
                AstNode *cast = make_typecast(p->cs,
                    st->basic_types[TYPE_float], arg, lineno);
                if (cast) n->children[0] = cast;
            }
            break;
        case BTOK_CHR:
            /* p_chr / p_chr_one (zxbparser.py:3427-3444): every
             * argument is make_typecast'd to TYPE.ubyte at parse time;
             * the CHR$ runtime expects 8-bit char codes. Without the
             * typecast the visit_BUILTIN/CHR handler would emit a
             * uinteger push (ld hl, ...; push hl) instead of the
             * narrowed `ld a, l; push af` form Python produces. */
            n->type_ = st->basic_types[TYPE_string];
            for (int i = 0; i < n->child_count; i++) {
                AstNode *ch = n->children[i];
                if (ch) {
                    AstNode *cast = make_typecast(p->cs,
                        st->basic_types[TYPE_ubyte], ch, lineno);
                    if (cast) n->children[i] = cast;
                }
            }
            break;
        case BTOK_VAL:
            n->type_ = st->basic_types[TYPE_float];
            break;
        case BTOK_LBOUND:
        case BTOK_UBOUND:
            /* LBOUND/UBOUND are always uinteger regardless of the array's
             * element type — Python zxbparser.py:3330/3332/3378
             * (make_builtin/make_number type_=TYPE.uinteger). Without this
             * they fall to default → arg->type_ (the element type). */
            n->type_ = st->basic_types[TYPE_uinteger];
            break;
        case BTOK_IN:
            /* IN returns a byte from a 16-bit port address — Python
             * zxbparser.py:3307-3314 p_expr_in: make_typecast(uinteger,
             * arg) on the operand, type_=ubyte on the result. Without
             * the result-side ubyte, the default branch picks up the
             * operand's type (uinteger) and the LET-store later inserts
             * a uinteger->ubyte narrowing (`ld h, a; ld a, l`) after
             * the `in a, (c)`. */
            if (arg && n->child_count > 0) {
                AstNode *cast = make_typecast(p->cs,
                                              st->basic_types[TYPE_uinteger],
                                              arg, lineno);
                if (cast) n->children[0] = cast;
            }
            n->type_ = st->basic_types[TYPE_ubyte];
            break;
        default:
            n->type_ = arg->type_;
            break;
    }
    return n;
}

/* Address-of a bare identifier (`bexpr : ADDRESSOF singleid`, p_addr_of_id
 * zxbparser.py:2667). access_id(ignore_explicit) + has_address + mark accessed
 * + UNARY ADDRESS (PTR_TYPE), CONSTEXPR-wrapped when the entry is not dynamic.
 * Extracted from parse_primary's @ branch so the Phase-D reduce reuses it
 * (C-vs-C identity). Behaviour-preserving. */
static AstNode *addr_of_id(Parser *p, const char *name, int lineno) {
    AstNode *entry = symboltable_access_id_noexplicit(
                         p->cs->symbol_table, p->cs, name, lineno,
                         NULL, CLASS_unknown);
    if (entry == NULL)
        return NULL;
    entry->u.id.has_address = true;
    if (!(p->cs->function_level.len > 0 &&
          entry->u.id.class_ == CLASS_function))
        mark_label_accessed(entry);
    AstNode *n = make_unary_node(p->cs, "ADDRESS", entry, lineno);
    if (n)
        n->type_ = p->cs->symbol_table->basic_types[TYPE_uinteger];
    if (n && !check_is_dynamic(entry)) {
        AstNode *ce = ast_new(p->cs, AST_CONSTEXPR, lineno);
        ast_add_child(p->cs, ce, n);
        ce->type_ = n->type_;
        return ce;
    }
    return n;
}

/* Parse primary expression (atoms, unary ops, parenthesized exprs) */
AstNode *parse_primary(Parser *p) {
    /* Number literal */
    if (match(p, BTOK_NUMBER)) {
        return make_number(p, p->previous.numval, p->previous.lineno, NULL);
    }

    /* String literal */
    if (match(p, BTOK_STRC)) {
        return make_string(p, p->previous.sval ? p->previous.sval : "", p->previous.lineno);
    }

    /* PI constant */
    if (match(p, BTOK_PI)) {
        return make_number(p, M_PI, p->previous.lineno,
                           p->cs->symbol_table->basic_types[TYPE_float]);
    }

    /* Parenthesized expression */
    if (match(p, BTOK_LP)) {
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_RP, "Expected ')' after expression");
        return expr;
    }

    /* Unary minus */
    if (match(p, BTOK_MINUS)) {
        int lineno = p->previous.lineno;
        AstNode *operand = parse_expression(p, PREC_UNARY);
        return make_unary_node(p->cs, "MINUS", operand, lineno);
    }

    /* Unary plus (no-op) */
    if (match(p, BTOK_PLUS)) {
        return parse_expression(p, PREC_UNARY);
    }

    /* NOT (logical) */
    if (match(p, BTOK_NOT)) {
        int lineno = p->previous.lineno;
        AstNode *operand = parse_expression(p, PREC_NOT);
        return make_unary_node(p->cs, "NOT", operand, lineno);
    }

    /* BNOT (bitwise) */
    if (match(p, BTOK_BNOT)) {
        int lineno = p->previous.lineno;
        AstNode *operand = parse_expression(p, PREC_BNOT_ADD);
        return make_unary_node(p->cs, "BNOT", operand, lineno);
    }

    /* CAST(type, expr) */
    if (match(p, BTOK_CAST)) {
        int lineno = p->previous.lineno;
        consume(p, BTOK_LP, "Expected '(' after CAST");
        TypeInfo *target = parse_type_name(p);
        if (!target) {
            parser_error(p, "Expected type name in CAST");
            return NULL;
        }
        consume(p, BTOK_COMMA, "Expected ',' in CAST");
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_RP, "Expected ')' after CAST");
        if (!expr) return NULL;

        /* p_cast (zxbparser.py:2522-2524) is exactly make_typecast(type,
         * expr) — which constant-folds a NUMBER/CONST operand into a
         * retyped literal rather than emitting a runtime cast. Building a
         * raw AST_TYPECAST here skipped that fold, so CAST(UInteger, 3)
         * emitted `ld a,3; ld l,a; ld h,0` instead of Python's `ld hl, 3`
         * (test print). Route through make_typecast for the fold +
         * string<->value error checks. */
        return make_typecast(p->cs, target, expr, lineno);
    }

    /* Address-of (@id or @id(args)) */
    if (match(p, BTOK_ADDRESSOF)) {
        int lineno = p->previous.lineno;
        if (!check(p, BTOK_ID) && !check(p, BTOK_ARRAY_ID)) {
            parser_error(p, "Expected identifier after @");
            return NULL;
        }
        advance(p);
        const char *name = p->previous.sval;

        /* Check for array/call access: @name(...) — a DIFFERENT Python
         * production (`bexpr : ADDRESSOF ID arg_list`, p_addr_of_array_id)
         * handled by parse_call_or_array; keep the bare UNARY ADDRESS. */
        if (check(p, BTOK_LP)) {
            AstNode *n = ast_new(p->cs, AST_UNARY, lineno);
            n->u.unary.operator = arena_strdup(&p->cs->arena, "ADDRESS");
            /* addressof_ctx=true selects the "Undeclared array"
             * diagnostic for an undeclared <name>. */
            AstNode *operand = parse_call_or_array(p, name, lineno,
                                                   true, true);
            ast_add_child(p->cs, n, operand);
            n->type_ = p->cs->symbol_table->basic_types[TYPE_uinteger];
            return n;
        }

        /* p_addr_of_id (zxbparser.py:2667-2685, `bexpr : ADDRESSOF
         * singleid`):
         *   entry = access_id(name, lineno, ignore_explicit_flag=True)
         *           # default_class == CLASS.unknown
         *   entry.has_address = True
         *   mark_entry_as_accessed(entry)
         *   result = make_unary("ADDRESS", entry, type_=PTR_TYPE)
         *   p[0] = result if is_dynamic(entry) else make_constexpr(result)
         *
         * access_id shares the symbol-table entry by name, so a later
         * `<name>:` label definition (symboltable_access_label) converts
         * this same CLASS_unknown entry to a label (to_label,
         * _id.py:143) — the UNARY operand IS that shared entry, so its
         * mangled/.t resolve like the LabelRef at translate time. */
        return addr_of_id(p, name, lineno);
    }

    /* Builtin functions */
    BTokenType kw = p->current.type;
    switch (kw) {
        case BTOK_ABS: case BTOK_INT: case BTOK_SGN:
        case BTOK_SIN: case BTOK_COS: case BTOK_TAN:
        case BTOK_ACS: case BTOK_ASN: case BTOK_ATN:
        case BTOK_EXP: case BTOK_LN: case BTOK_SQR:
        case BTOK_PEEK: case BTOK_USR: case BTOK_CODE:
        case BTOK_LEN: case BTOK_VAL: case BTOK_STR:
        case BTOK_CHR: case BTOK_RND: case BTOK_INKEY:
        case BTOK_SIZEOF: case BTOK_LBOUND: case BTOK_UBOUND:
        case BTOK_BIN: case BTOK_IN:
        {
            const char *fname = btok_name(kw);
            advance(p);
            return parse_builtin_func(p, fname, kw);
        }
        default:
            break;
    }

    /* Identifier or function/array call */
    if (match(p, BTOK_ID) || match(p, BTOK_ARRAY_ID)) {
        const char *name = p->previous.sval;
        int lineno = p->previous.lineno;

        /* Check for function call or array access: ID(...) */
        if (check(p, BTOK_LP)) {
            return parse_call_or_array(p, name, lineno, true, false);
        }

        /* Variable reference — resolve via symbol table (auto-declares if implicit)
         * Matches Python p_id_expr: uses access_id (not access_var) so labels,
         * functions, and arrays are accepted without class-check errors. */
        AstNode *entry = symboltable_access_id(p->cs->symbol_table, p->cs,
                                                name, lineno, NULL, CLASS_var);
        if (entry) {
            entry->u.id.accessed = true;
            /* If class is still unknown, set it to var */
            if (entry->u.id.class_ == CLASS_unknown)
                entry->u.id.class_ = CLASS_var;
            /* Python p_id_expr (zxbparser.py:2649-2651): when the existing
             * entry has TYPE.unknown/auto, promote it to DEFAULT_TYPE and
             * emit warning_implicit_type. Without this, a `LET a = a + 1`
             * with `a` newly auto-declared (TYPE_unknown via the new LET
             * pre-access below) leaves the RHS read at TYPE_unknown, so
             * the later LET-store sees var.type_==unknown and emits no
             * meaningful store IC. Faithful net effect: an undeclared
             * scalar read promotes to DEFAULT_TYPE on first read. */
            if (entry->type_ && entry->type_->final_type &&
                entry->type_->final_type->basic_type == TYPE_unknown) {
                TypeInfo *promoted = type_new_ref(p->cs, p->cs->default_type,
                                                  lineno, true);
                entry->type_ = promoted;
                warn_implicit_type(p->cs, lineno, name,
                                   p->cs->default_type->name);
            }
            /* Function with 0 args — treat as call (matching Python p_id_expr).
             * Python also has `bexpr : ID bexpr` (zxbparser.py:2839-2852)
             * for the parenless single-arg form (e.g. `LET c = xx 1`):
             * if the next token starts a bexpr, consume one bexpr and
             * pass it as the single argument. Only fires when the entry
             * has already been resolved to a FUNCTION (Python's grammar
             * never reaches make_call for VAR/STRSLICE here — :2846 just
             * marks-accessed). */
            if (entry->u.id.class_ == CLASS_function) {
                AstNode *args = ast_new(p->cs, AST_ARGLIST, lineno);
                /* Inline expr-start predicate: tokens that can begin a
                 * bexpr per parse_primary. We exclude EQ/COMMA/NEWLINE/
                 * EOF/CO/RP/THEN/TO/STEP/etc. so the bare-ID 0-arg call
                 * is unaffected when no expression follows. */
                BTokenType nt = p->current.type;
                bool starts_expr =
                    (nt == BTOK_NUMBER || nt == BTOK_STRC || nt == BTOK_PI ||
                     nt == BTOK_MINUS  || nt == BTOK_PLUS  || nt == BTOK_NOT ||
                     nt == BTOK_BNOT   || nt == BTOK_ADDRESSOF ||
                     nt == BTOK_LP     || nt == BTOK_ID    || nt == BTOK_ARRAY_ID ||
                     nt == BTOK_LABEL);
                if (starts_expr) {
                    AstNode *arg_expr = parse_expression(p, PREC_NONE + 1);
                    if (arg_expr) {
                        AstNode *arg = ast_new(p->cs, AST_ARGUMENT,
                                                arg_expr->lineno);
                        ast_add_child(p->cs, arg, arg_expr);
                        arg->type_ = arg_expr->type_;
                        ast_add_child(p->cs, args, arg);
                    }
                }
                AstNode *call = ast_new(p->cs, AST_FUNCCALL, lineno);
                ast_add_child(p->cs, call, entry);
                ast_add_child(p->cs, call, args);
                call->type_ = entry->type_;
                /* SymbolCALL.filename = gl.FILENAME at parse point
                 * (symbols/call.py:42) — for the R3-R10 fname=. */
                if (p->cs->current_file)
                    call->u.call.filename =
                        arena_strdup(&p->cs->arena, p->cs->current_file);
                /* Python call.py:102 inline-vs-deferred dispatch:
                 * callee already a finished definition (real PARAMLIST,
                 * not forwarded) == entry.declared and not forwarded. */
                call->u.call.callee_inline =
                    (entry->u.id.params != NULL && !entry->u.id.forwarded);
                vec_push(p->cs->function_calls, call);
                return call;
            }
            /* SUB used in expression context — error (matching Python p_id_expr) */
            if (entry->u.id.class_ == CLASS_sub) {
                zxbc_error(p->cs, lineno, "'%s' is a SUB not a FUNCTION", name);
                return NULL;
            }
            return entry;
        }

        /* access_id returned NULL (error) — create placeholder */
        AstNode *n = ast_new(p->cs, AST_ID, lineno);
        n->u.id.name = arena_strdup(&p->cs->arena, name);
        n->type_ = p->cs->default_type;
        n->u.id.class_ = CLASS_unknown;
        return n;
    }

    /* Label as expression */
    if (match(p, BTOK_LABEL)) {
        const char *label_text = p->previous.sval;
        double label_val = p->previous.numval;
        int lineno = p->previous.lineno;

        if (label_text) {
            AstNode *n = ast_new(p->cs, AST_ID, lineno);
            n->u.id.name = arena_strdup(&p->cs->arena, label_text);
            n->u.id.class_ = CLASS_label;
            return n;
        }
        return make_number(p, label_val, lineno, NULL);
    }

    parser_error(p, "Expected expression");
    return NULL;
}

/* Inline (def-first) argument check, run at PARSE time.
 *
 * Faithful port of Python symbols/call.py:102-103: when the resolved
 * callee is already a finished definition (entry.declared and not
 * entry.forwarded — the C `callee_inline` predicate), Python runs the
 * full check.check_call_arguments AT THE CALL SITE during parsing,
 * rather than deferring it to check_pending_calls. Running it here (not
 * in the post-parse deferred loop) reproduces Python's parse-time
 * firing order: the call-argument errors precede the later-emitted
 * implicit-type [W100] warnings of the surrounding statement (e.g. the
 * LET target in `let a = test(1)`), exactly as the oracle orders them.
 * check_call_arguments also performs the codegen-visible side effects
 * (default-fill, arg->param typecast, arg.byref propagation), so the
 * inline call no longer needs the bespoke minimal pass that the
 * deferred loop's `callee_inline` branch used to run.
 *
 * The callee entry is the call node's child[0]; resolve the global-scope
 * definition the same way the deferred loop does so a callee whose
 * params were stamped on the global FUNCDECL entry is seen. */
static void inline_check_call_arguments(Parser *p, AstNode *call) {
    if (!call || call->child_count < 1) return;
    AstNode *callee = call->children[0];
    if (!callee || callee->tag != AST_ID) return;
    if (!call->u.call.callee_inline) return;

    const char *name = callee->u.id.name;
    AstNode *entry = callee;
    if (name) {
        /* Strip a single deprecated suffix for the global lookup, mirroring
         * check_pending_calls' resolution. */
        size_t len = strlen(name);
        char stripped[256];
        const char *lookup_name = name;
        if (len > 0 && len < sizeof(stripped) &&
            is_deprecated_suffix(name[len - 1])) {
            memcpy(stripped, name, len - 1);
            stripped[len - 1] = '\0';
            lookup_name = stripped;
        }
        AstNode *g = symboltable_lookup(p->cs->symbol_table, lookup_name);
        if (g) entry = g;
    }

    /* Python passes fname=filename (SymbolCALL.filename) for R3-R6 and
     * fname=arg.filename for R7/R9 — both the call-site file. Swap
     * cs->current_file for the duration (the faithful fname= analogue),
     * exactly as the deferred loop does. */
    char *saved_file = p->cs->current_file;
    if (call->u.call.filename)
        p->cs->current_file = call->u.call.filename;
    check_call_arguments(p->cs, call, entry, name);
    p->cs->current_file = saved_file;
}

/* Parse function call or array access: name(...) */
static AstNode *parse_call_or_array(Parser *p, const char *name, int lineno, bool expr_context, bool addressof_ctx) {
    consume(p, BTOK_LP, "Expected '('");

    /* Parse argument list */
    AstNode *arglist = ast_new(p->cs, AST_ARGLIST, lineno);

    bool has_to = false;
    if (!check(p, BTOK_RP)) {
        do {
            /* Check for TO without lower bound: (TO expr) — Python's
             * p_subind_strTO / p_subind_TO always fill defaults so the
             * STRSLICE child pair is well-formed downstream: lower
             * defaults to make_number(0), upper defaults to
             * make_number(MAX_STRSLICE_IDX=65534).  Both are
             * uinteger-typecast (zxbparser.py:2608-2638). */
            AstNode *arg_expr = NULL;
            if (check(p, BTOK_TO)) {
                has_to = true;
                advance(p);
                AstNode *upper = NULL;
                if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                    upper = parse_expression(p, PREC_NONE + 1);
                AstNode *slice = ast_new(p->cs, AST_STRSLICE, lineno);
                TypeInfo *uint_t =
                    p->cs->symbol_table->basic_types[TYPE_uinteger];
                AstNode *low = make_typecast(p->cs, uint_t,
                                             make_number(p, 0, lineno, NULL),
                                             lineno);
                ast_add_child(p->cs, slice, low);
                if (!upper)
                    upper = make_number(p, 65534, lineno, NULL);
                upper = make_typecast(p->cs, uint_t, upper, lineno);
                ast_add_child(p->cs, slice, upper);
                ast_add_child(p->cs, arglist, slice);
                continue;
            }
            /* Check for named argument: name := expr */
            if ((check(p, BTOK_ID) || is_name_token(p))) {
                int save_pos = p->lexer.pos;
                BToken save_cur = p->current;
                advance(p);
                if (check(p, BTOK_WEQ)) {
                    /* Named argument */
                    advance(p);
                    arg_expr = parse_expression(p, PREC_NONE + 1);
                    if (arg_expr) {
                        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, arg_expr->lineno);
                        arg->u.argument.name = arena_strdup(&p->cs->arena, save_cur.sval ? save_cur.sval : btok_name(save_cur.type));
                        arg->u.argument.byref = p->cs->opts.default_byref;
                        ast_add_child(p->cs, arg, arg_expr);
                        arg->type_ = arg_expr->type_;
                        ast_add_child(p->cs, arglist, arg);
                    }
                    continue;
                }
                /* Not a named argument — restore */
                p->current = save_cur;
                p->lexer.pos = save_pos;
            }
            arg_expr = parse_expression(p, PREC_NONE + 1);
            /* Check for string slice: expr TO [expr] — Python's
             * p_subind_str / p_subind_TOstr always emit a (lower,upper)
             * pair; default the upper to 65534 when absent
             * (zxbparser.py:2616-2626). The outer typecast loop at
             * line ~1008 wraps each bound in TYPECAST(uinteger) using
             * the OUTER call-site lineno — leave the typecast to that
             * loop so the test-50 error line stays at the (-opening
             * line, not the bound symbol's declaration line. */
            if (match(p, BTOK_TO)) {
                has_to = true;
                AstNode *upper = NULL;
                if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                    upper = parse_expression(p, PREC_NONE + 1);
                int sl_ln = arg_expr ? arg_expr->lineno : lineno;
                AstNode *slice = ast_new(p->cs, AST_STRSLICE, sl_ln);
                if (!arg_expr)
                    arg_expr = make_number(p, 0, sl_ln, NULL);
                ast_add_child(p->cs, slice, arg_expr);
                if (!upper)
                    upper = make_number(p, 65534, sl_ln, NULL);
                ast_add_child(p->cs, slice, upper);
                ast_add_child(p->cs, arglist, slice);
            } else if (arg_expr) {
                AstNode *arg = ast_new(p->cs, AST_ARGUMENT, arg_expr->lineno);
                arg->u.argument.byref = p->cs->opts.default_byref;
                ast_add_child(p->cs, arg, arg_expr);
                arg->type_ = arg_expr->type_;
                ast_add_child(p->cs, arglist, arg);
            }
        } while (match(p, BTOK_COMMA));
    }

    consume(p, BTOK_RP, "Expected ')' after arguments");

    /* String-ARRAY element substring assignment, single-paren-group family:
     *   a$(dims..., lo TO hi) = rhs   (TO inside the subscript group)
     *   a$(dims..., idx)      = rhs   (one extra subscript, no TO)
     * Python parses these with dedicated statement productions
     * (p_let_arr_substr* :2733-2815, p_let_arr string branch :1199-1205),
     * all -> make_array_substr_assign (:328-362) -> LETARRAYSUBSTR.  These
     * shapes occur ONLY as write targets — there is no read production for
     * `ARRAY_ID (args TO ...)` (verified on the oracle) and a read with an
     * extra subscript is the dim-count error.  So act only on the write
     * target (expr_context == false) when `name` is a declared string
     * ARRAY.  Without this the C collapses the TO form into a scalar
     * STRSLICE on the array NAME (losing the element) and treats the
     * no-TO extra subscript as an N-arg array access — both wrong binaries
     * (let_array_substr9..13, sys_letarrsubstr0..2). */
    /* EXCLUDE the chained-postfix form `a(i,j)(k)` (a `(` follows this
     * `)`): there `a(i,j)` is the inner postfix-base array access and
     * Python runs the full dim-count check on it (the trailing `(k)` is
     * routed by p_let_arr_substr_single, not these single-group
     * productions), so a 1-dim `a(3,4)(1)` is the dim-count error, not a
     * substring assign (let_array_substr8).  Same `!check(BTOK_LP)` guard
     * shape as str_substr_assign_shape in the CLASS_array branch. */
    if (!expr_context && arglist->child_count >= 2 && !check(p, BTOK_LP)) {
        /* get_entry strips a trailing deprecated suffix ($/%/&) so a
         * suffixed array name `a$(...)` resolves to the entry stored
         * under `a` (symboltable.py:76-77 / compiler.c:286). */
        AstNode *probe = symboltable_get_entry(p->cs->symbol_table, name);
        if (probe && probe->tag == AST_ID &&
            probe->u.id.class_ == CLASS_array &&
            type_is_string(probe->type_)) {
            AstNode *last = arglist->children[arglist->child_count - 1];
            int ndecl = probe->u.id.arr_boundlist
                            ? probe->u.id.arr_boundlist->child_count : 0;
            AstNode *lower = NULL, *upper = NULL;
            bool is_substr_shape = false;
            if (last && last->tag == AST_STRSLICE && last->child_count >= 2) {
                /* TO-form: the trailing subscript group is `lo TO hi`
                 * (parse loop built it as an inner STRSLICE).  All
                 * preceding elements are the DIM subscripts. */
                lower = last->children[0];
                upper = last->children[1];
                is_substr_shape = true;
            } else if (!has_to && last &&
                       arglist->child_count == ndecl + 1) {
                /* No-TO form with exactly one extra subscript: pop it as
                 * the single substring index (p_let_arr string branch,
                 * lower==upper==idx). */
                AstNode *idx = (last->tag == AST_ARGUMENT &&
                                last->child_count > 0)
                                   ? last->children[0] : last;
                lower = idx;
                upper = idx;
                is_substr_shape = true;
            }
            if (is_substr_shape) {
                int ndim = arglist->child_count - 1;  /* drop the substr */
                AstNode *lv = build_array_substr_lvalue(
                    p, name, lineno, arglist->children, ndim, lower, upper);
                return lv;  /* NULL on the typed/dim error -> p[0]=None */
            }
        }
    }

    /* String slice: name$(from TO to) */
    if (has_to) {
        AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
        /* Look up through symbol table — respects explicit mode */
        AstNode *id_node = symboltable_access_var(p->cs->symbol_table, p->cs, name, lineno,
                                                   p->cs->symbol_table->basic_types[TYPE_string]);
        if (!id_node) {
            /* access_var already reported the error (e.g. undeclared in explicit mode) */
            id_node = ast_new(p->cs, AST_ID, lineno);
            id_node->u.id.name = arena_strdup(&p->cs->arena, name);
            id_node->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        }
        /* p_expr_id_substr (zxbparser.py:2567-2579): the non-CONST
         * `string : ID substr` READ production does access_var +
         * mark_entry_as_accessed(entry); the CONST-string fast path
         * (entry.token=="CONST") does NOT mark. The substring-LVALUE
         * target (`a$(i TO j) = x`) is a DIFFERENT Python production
         * (p_let_substr) that does NOT go through p_expr_id_substr and
         * does NOT mark accessed — so gate on expr_context exactly like
         * the CLASS_array branch below (expr_context is false only on
         * the LET lvalue target path). Without this `a$ = a$(x TO)`
         * leaves a$ unaccessed and O>1 DCE drops the program. */
        if (expr_context && id_node->tag == AST_ID &&
            id_node->u.id.class_ != CLASS_const)
            id_node->u.id.accessed = true;
        ast_add_child(p->cs, n, id_node);
        /* STRSLICE.make_node (symbols/strslice.py:71): the FIRST semantic
         * check is check.check_type(lineno, Type.string, s) (check.py:40-55)
         * — the sliced base `s` must be a string, else
         * "Wrong expression type '<t>'. Expected 'string'" at the ID line
         * (make_strslice is called with p.lineno(1)), p[0]=None. An
         * undeclared name was access_var'd with default_type=string above,
         * so it is already string and never trips this. Runs BEFORE the
         * bound TYPECASTs/fold, exactly Python's order.
         *
         * GATED on expr_context: only the READ productions
         * (p_expr_id_substr / p_string_* -> make_strslice -> make_node)
         * run check_type. The WRITE/lvalue target (`n(i TO j) = rhs`,
         * p_str_assign zxbparser.py:1308-1335) builds the LETSUBSTR
         * sentence directly and does NOT call make_node — so Python
         * ACCEPTS a non-string slice TARGET (verified on the oracle:
         * `Dim n As Integer : n(1 TO 2)="x"` -> exit 0 + W150). Without
         * this gate the check fires on the lvalue path and over-rejects
         * (err_letsubstr_lhs_nonstring FALSE_POS). */
        if (expr_context && !type_is_string(id_node->type_)) {
            const TypeInfo *sft = (id_node->type_ && id_node->type_->final_type)
                                      ? id_node->type_->final_type : id_node->type_;
            const char *stn = (sft && sft->tag == AST_BASICTYPE)
                                  ? basictype_to_string(sft->basic_type) : "unknown";
            zxbc_error(p->cs, lineno,
                       "Wrong expression type '%s'. Expected 'string'", stn);
            return NULL;
        }
        /* Python STRSLICE.make_node (symbols/strslice.py:74-83) wraps
         * each bound in TYPECAST(STR_INDEX_TYPE=uinteger, MINUS(bound,
         * OPTIONS.string_base)).  TWO layers, both required:
         *
         *   1. The `substr` non-terminal (zxbparser.py:2600-2638) already
         *      `make_typecast(TYPE.uinteger, bound)` each TO bound BEFORE
         *      make_strslice — this is the `pre` cast below.  It catches
         *      the `a$(a TO b)` case (fixtures 50/51) where a/b resolve
         *      via deprecated-suffix sharing to a string-typed entry and
         *      trip "Cannot convert string to a value. Use VAL()".
         *
         *   2. make_node then `MINUS(bound, string_base)` and TYPECAST
         *      again.  The MINUS is UNCONDITIONAL (Python never special-
         *      cases base==0): it folds `bound - 0` for a constant bound
         *      but builds a real MINUS node for a non-constant one,
         *      yielding the extra `subu16 t,bound,0` -> `pop hl; push hl`
         *      at -O0 (binary.py:144; the peephole strips it at O>0).
         *      Since the `pre` cast already widened the bound to uinteger,
         *      the MINUS common-types to u16 — base type is irrelevant, so
         *      make_number auto-infers (NULL), matching Python's bare
         *      NUMBER(string_base).
         *
         * The C built each `lower TO upper` pair as an inner AST_STRSLICE
         * in `arglist` with raw bound exprs as children — apply both
         * layers now so the error surfaces at parse time and the bound
         * round-trip is byte-faithful.  Single-index callers (the second
         * STRSLICE site below at line ~1196) apply the equivalent in
         * their own loop. */
        TypeInfo *str_idx = p->cs->symbol_table->basic_types[TYPE_uinteger];
        for (int i = 0; i < arglist->child_count; i++) {
            AstNode *inner = arglist->children[i];
            if (inner && inner->tag == AST_STRSLICE) {
                for (int j = 0; j < inner->child_count; j++) {
                    AstNode *bound = inner->children[j];
                    if (!bound) continue;
                    /* Python passes the strslice lineno — the line the
                     * substring expression appeared on, not the line
                     * the bound's symbol was first declared on.  The
                     * inner STRSLICE was created with `arg_expr->lineno`
                     * (which is the bound *symbol*'s lineno = declaration
                     * line) so use the OUTER parse_call_or_array
                     * `lineno` (the source line where `name(` opened)
                     * to match Python (e.g. fixture 50: error at line
                     * 2, the substring site, not line 1, where `a$` was
                     * first declared). */
                    AstNode *pre = make_typecast(p->cs, str_idx, bound,
                                                 lineno);
                    /* make_typecast returns NULL on type error and
                     * already emitted the message; leave the child
                     * untouched in that case so downstream code does
                     * not deref NULL.  On success it returns either the
                     * same node (type already matches) or a fresh
                     * TYPECAST wrapper. */
                    if (!pre) continue;
                    /* MINUS — READ vs WRITE differ for the TO form:
                     *
                     * READ (expr_context): STRSLICE.make_node wraps each
                     * bound in MINUS(bound, string_base) UNCONDITIONALLY
                     * (strslice.py:74-83).  The `substr` non-terminal
                     * already pre-typecast the bound to uinteger
                     * (zxbparser.py:2600-2638) — that is `pre` — so the
                     * MINUS common-types to u16 (`cast; subu16`).  base is
                     * auto-inferred (NULL) like Python's NUMBER(string_base).
                     *
                     * WRITE (!expr_context): the TO lvalue `s$(i TO j) = rhs`
                     * is Python's p_str_assign (zxbparser.py:1308-1335),
                     * which builds LETSUBSTR with the RAW substr bounds and
                     * applies NO MINUS for ANY base; visit_LETSUBSTR
                     * (translator.py:366-402) / the C tr_letsubstr_emit
                     * subtract per their own contract.  So the WRITE keeps
                     * ONLY the typecast (`pre`) — byte-for-byte the parent
                     * c9c34b2c form (which applied only the typecast here). */
                    AstNode *cast;
                    if (expr_context) {
                        AstNode *sub = make_binary_node(
                            p->cs, "MINUS", pre,
                            make_number(p, p->cs->opts.string_base, lineno, NULL),
                            lineno, NULL);
                        cast = make_typecast(p->cs, str_idx, sub, lineno);
                    } else {
                        cast = pre;  /* parent form: typecast only, no MINUS */
                    }
                    if (cast) inner->children[j] = cast;
                }
                /* Flatten: Python's STRSLICE has 3 direct children
                 * [s, lower, upper] (strslice.py:18-31). The C arglist
                 * construction wraps each `lower TO upper` pair as a
                 * NESTED AST_STRSLICE which we now splice out so the
                 * outer is [id, lower, upper] — visit_STRSLICE reads
                 * children[1]/[2] directly (translator.c:3362-3364).
                 * Pre-fix the dual-slice path (slice0/slice2) ran the
                 * recursive visit_STRSLICE on the still-nested inner
                 * and the outer's lower/upper both resolved to the
                 * inner node's empty .t (== 65534/65534). */
                for (int k = 0; k < inner->child_count; k++)
                    ast_add_child(p->cs, n, inner->children[k]);
            } else if (inner) {
                ast_add_child(p->cs, n, inner);
            }
        }
        n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        /* STRSLICE.make_node constant-fold tail (strslice.py:84-113):
         * fold `a$( TO )` (full slice) to the bare string, empty/constant
         * slices to a STRING literal. */
        return strslice_fold(p, n);
    }

    return make_call_node(p, name, lineno, arglist, expr_context,
                          addressof_ctx, check(p, BTOK_LP));
}

/* make_call node-build core (zxbparser.py make_call :365-420 +
 * make_func_call/make_array_access/make_strslice). Resolves the entry via
 * access_call and builds ARRAYACCESS/ARRAYLOAD (array), STRSLICE (string
 * var/const), or FUNCCALL — from an ALREADY-BUILT arglist. Extracted from
 * parse_call_or_array so the Phase-D `func_call : ID|ARRAY_ID arg_list`
 * reduce-actions reuse it (C-vs-C identity). `next_is_lp` is the caller's
 * lookahead (was `check(p, BTOK_LP)` inline) for the chained-postfix
 * string-array dim-count gate. Behaviour-preserving extraction. */
static AstNode *make_call_node(Parser *p, const char *name, int lineno,
                               AstNode *arglist, bool expr_context,
                               bool addressof_ctx, bool next_is_lp) {
    /* Resolve via symbol table: auto-declares if needed (matching Python's access_call) */
    AstNode *entry = symboltable_access_call(p->cs->symbol_table, p->cs, name, lineno, NULL);

    /* In Python, newly implicitly declared CLASS_unknown entries have callable=False.
     * Only access_func (used for statement-level sub calls) sets callable=True.
     * In expression context, CLASS_unknown non-string entries are not callable.
     * In statement context (could be a forward sub call), they're allowed. */
    if (expr_context && entry && entry->u.id.class_ == CLASS_unknown && !type_is_string(entry->type_)) {
        /* @<id>(args) where <id> is undeclared (auto-declared
         * CLASS_unknown) is Python's dedicated p_err_undefined_arr_access
         * production (zxbparser.py:2835) → 'Undeclared array "%s"'.
         * A plain undeclared x(args) call keeps the generic message
         * (mcleod3-class — addressof_ctx is false there). */
        if (addressof_ctx)
            err_undeclared_array(p->cs, lineno, name);
        else
            err_not_array_nor_func(p->cs, lineno, name);
        return NULL;
    }

    if (entry && entry->u.id.class_ == CLASS_array) {
        /* Array access. ARRAYACCESS-node construction itself does NOT mark
         * the array accessed in Python: make_call's array branch
         * (zxbparser.py:389-397) and ARRAYACCESS.make_node never call
         * mark_entry_as_accessed. Only the expression-read production
         * p_arr_access_expr (`func_call : ARRAY_ID arg_list`,
         * zxbparser.py:2723-2730) marks it; the LETARRAY write-target
         * production p_let_arr (zxbparser.py:1207-1218) does NOT (except
         * the entry.addr case it handles itself). expr_context is true on
         * every read/@-address path (parser.c:517,555) and false only for
         * the LETARRAY lvalue target (parser.c:1483), so this single gate
         * makes a write-only array's `accessed` False — exactly Python —
         * which lets visit_ARRAYDECL's O>1 DCE drop it (matching
         * str_base0 / let_array_substr / array01..05). Array-node-local:
         * confined to the CLASS_array branch (the FUNCCALL branch below
         * is untouched; the STRSLICE branch takes the identical gate
         * under S5.8a for the same reason — see its comment). */
        AstNode *n = ast_new(p->cs, AST_ARRAYACCESS, lineno);
        /* Python make_call (zxbparser.py:390) builds sym.ARRAYLOAD for a
         * READ (expr_context) -> visit_ARRAYLOAD (aload); the LETARRAY
         * lvalue (make_array_access, :325) builds sym.ARRAYACCESS ->
         * visit_ARRAYACCESS (push indices only, no aload). expr_context
         * is the C analogue (false only for the LETARRAY write target,
         * parser.c:1483). */
        n->u.arrayaccess.is_load = expr_context;
        if (expr_context)
            entry->u.id.accessed = true;

        /* Port C — make_array_access (zxbparser.py:311-325) wrapper +
         * sym.ARRAYACCESS.make_node (arrayaccess.py:99-122).
         *
         * (a) Wrapper :316 - every subscript is make_typecast'd to
         *     gl.BOUND_TYPE (== TYPE.uinteger for every target, set in
         *     each arch package __init__).  If any returns None it is a
         *     semantic error (already emitted) and make_array_access
         *     returns None - this is the gating reject for sn_crash
         *     (a String subscript -> "Cannot convert string to a
         *     value." via the already-ported make_typecast,
         *     compiler.c:596).  Python applies this regardless of
         *     scope, BEFORE make_node's dim-count check.
         * (b) make_node :100-104 - for scope != parameter only,
         *     len(variable.bounds) != len(arglist) ->
         *     "Array '%s' has %i dimensions, not %i"
         *     (variable.name, #bounds, #args).  A byref array
         *     parameter has no fixed bound count, so Python skips the
         *     check for it - mirrored by the SCOPE_parameter guard.
         * The const-subscript out-of-range case (:110-111) is a
         * warning, not an error; not required by any owned fixture and
         * intentionally not emitted here (faithful: it does not flip
         * exit and the C corpus has no fixture gated on it). */
        TypeInfo *bound_type = p->cs->symbol_table->basic_types[TYPE_uinteger];
        bool subscript_error = false;
        for (int i = 0; i < arglist->child_count; i++) {
            AstNode *arg = arglist->children[i];
            if (arg && arg->tag == AST_ARGUMENT && arg->child_count > 0) {
                /* Python make_array_access (zxbparser.py:316) passes
                 * arg.lineno - the ARGUMENT's *source* line at the call
                 * site, i.e. the array-access statement line (== this
                 * `lineno`).  The C ARGUMENT node's own lineno tracks
                 * the resolved subscript symbol's declaration line
                 * (e.g. sn_crash's `f` declared at line 2), so use the
                 * call-site lineno to byte-match Python's reported
                 * line (sn_crash -> line 4). */
                AstNode *cast = make_typecast(p->cs, bound_type,
                                              arg->children[0], lineno);
                if (!cast) { subscript_error = true; break; }
                arg->children[0] = cast;
                arg->type_ = cast->type_;
            }
        }
        if (subscript_error)
            return NULL;  /* make_array_access returns None */

        /* Confinement for the C parser's pre-existing array-substring-
         * assignment grammar divergence (S5.10d-owned, OUT OF S5.10b
         * scope).  Python's p_let_arr_substr / p_let_arr_substr_single
         * (zxbparser.py:2733-2756) parse `a$(i, j) = <string>` as a
         * STRING-array element write whose LAST subscript is a SUBSTRING
         * index: they pop it before make_array_access, so Python's
         * dim-count sees nargs-1 (verified: sys_letarrsubstr0 / 1 / 2,
         * let_array_substr13 -> make_array_access nargs == declared
         * dims, accepted rc 0).  The C parser has no such production —
         * it builds the full N-arg ARRAYACCESS — so running the
         * dim-count reject on a string array in that single-paren-group
         * shape would reject what Python accepts (FALSE_POS, the
         * absolute-gate violation).
         *
         * The distinguishing signal, available right here: the
         * substring index is inside the SAME paren group only when NO
         * `(` postfix group follows the closing `)`.  The postfix form
         * `a(3,4)(1) = "HELLO"` (let_array_substr8) builds the inner
         * `a(3,4)` here with `(` as the very next token — there Python
         * DOES pass the full arglist to make_array_access (nargs == 2)
         * and rejects, so the dim-count check must still fire.  A
         * non-string array never triggers the substring grammar, so it
         * is always checked (let_array_wrong_dims).  Faithful narrower
         * subset: skip the dim-count reject ONLY for a string-typed
         * array not followed by a postfix `(` group; never widen past
         * Python.  (The subscript BOUND_TYPE typecast above still runs —
         * Python's make_array_substr_assign also typecasts its arglist,
         * and the cast is idempotent/harmless for the popped index.) */
        bool str_substr_assign_shape =
            type_is_string(entry->type_) && !next_is_lp;

        if (entry->u.id.scope != SCOPE_parameter && !str_substr_assign_shape) {
            int ndecl = entry->u.id.arr_boundlist
                            ? entry->u.id.arr_boundlist->child_count : 0;
            if (ndecl != arglist->child_count) {
                zxbc_error(p->cs, lineno, "Array '%s' has %d dimensions, not %d",
                           entry->u.id.name, ndecl, arglist->child_count);
                return NULL;
            }
        }

        /* SymbolARRAYACCESS.__init__ (arrayaccess.py:34-37) sets
         * self.entry.ref.is_dynamically_accessed = True unconditionally
         * for EVERY successfully constructed array access — read
         * (p_arr_access_expr) AND write target (p_let_arr ->
         * make_array_access, zxbparser.py:1207). It is independent of the
         * subscript being constant (score(1) still flips it) and of
         * read/write context, so it is set here — after the dim-count
         * gate (the Python `return None` paths above never reach the
         * constructor) and unconditionally (no expr_context gate, unlike
         * `accessed`). VarTranslator.visit_ARRAYDECL (var_translator.py
         * :58) reads this OR-ed with lbound_used to decide the
         * `<mangled>.__LBOUND__` descriptor slot + bound table for a
         * non-zero-based global array (einar01: score(1 TO 2) accessed
         * by score(1)/score(2) -> _score.__LBOUND__). */
        entry->u.id.is_dynamically_accessed = true;

        ast_add_child(p->cs, n, entry);
        for (int i = 0; i < arglist->child_count; i++)
            ast_add_child(p->cs, n, arglist->children[i]);
        n->type_ = entry->type_;

        /* SymbolARRAYACCESS.offset (symbols/arrayaccess.py:68-91) — the
         * constant byte offset when EVERY subscript is compile-time
         * constant. Computed here once (the C analogue of the
         * cached_property; make_call appends the folded NUMBER child for
         * the read path at zxbparser.py:388-392, but the offset itself is
         * intrinsic to the node and also read by visit_LETARRAY for the
         * write target via make_array_access). arglist still holds the
         * BOUND_TYPE-typecast'd index exprs (loop above). */
        compute_arrayaccess_offset(p, n, entry, arglist);
        return n;
    }

    /* Python make_call CONST-string branch (zxbparser.py:399-416):
     *   if entry.class_ in (CLASS.var, CLASS.const):
     *       if len(args) > 1: errmsg.syntax_error_not_array_nor_func; None
     *       ... (var re-resolved via access_var; handled by the CLASS_var
     *            branch below)
     *       if len(args) == 1:
     *           # it's a const
     *           return make_strslice(lineno,
     *                       sym.STRING(entry.value, lineno),
     *                       args[0].value, args[0].value)
     *       mark_entry_as_accessed(entry); return entry
     *
     * A CONST-string call (`ys(2)` / `ys()` / `ys(2 TO)` where ys is a
     * string CONST) must NOT fall through to the FUNCCALL path (it is not
     * a function). The no-TO single-index and zero-arg forms reach here
     * (the `i TO j` form is taken by the has_to slice branch above, which
     * already resolves CONST bases through strslice_fold). For a single
     * index Python slices the const's *value* (wrapped in a fresh STRING)
     * and folds to a STRING constant; for zero args it returns the const
     * id itself (so `xs + ys()` const-folds via the PLUS path). Mirrors
     * ConstRef.value (constref.py:35-40) -> the stored STRING literal. */
    if (entry && entry->u.id.class_ == CLASS_const &&
        type_is_string(entry->type_)) {
        if (arglist->child_count > 1) {
            err_not_array_nor_func(p->cs, lineno, name);
            return NULL;
        }
        if (arglist->child_count == 0) {
            /* len(args) == 0: mark_entry_as_accessed; return entry. */
            if (expr_context)
                entry->u.id.accessed = true;
            return entry;
        }
        /* len(args) == 1: make_strslice(STRING(entry.value), idx, idx). */
        const AstNode *vnode = const_string_value_node(entry);
        AstNode *base_str;
        if (vnode) {
            base_str = make_string(p, vnode->u.string.value
                                          ? vnode->u.string.value : "",
                                   lineno);
            base_str->u.string.length = vnode->u.string.length;
        } else {
            base_str = make_string(p, "", lineno);
        }
        AstNode *a0 = arglist->children[0];
        AstNode *idx = (a0 && a0->tag == AST_ARGUMENT && a0->child_count > 0)
                           ? a0->children[0] : a0;
        /* STRSLICE.make_node (strslice.py:74-83): each bound is
         * TYPECAST(STR_INDEX_TYPE=uinteger, MINUS(idx, string_base)).
         * lower and upper are the SAME index node value (args[0].value
         * twice); build two independent bound chains so a later in-place
         * retype of one does not alias the other.
         *
         * The MINUS is applied UNCONDITIONALLY (Python never special-cases
         * base==0): BINARY.make_node folds `idx - 0` to `idx` for a
         * constant bound (binary.py:103-111), but for a NON-constant bound
         * it builds a real BINARY MINUS node (binary.py:144) which lowers
         * to a `subu16 t, idx, 0` quad -> an extra `pop hl; push hl` at -O0
         * (sub16, _16bit.py:189-195; the peephole optimizer removes it at
         * O>0 — which is why a prior `if (base != 0)` guard looked
         * harmless above -O0). make_binary_node mirrors the same fold, so
         * constant bounds stay byte-identical while expression bounds get
         * the faithful round-trip. */
        TypeInfo *uint_t = p->cs->symbol_table->basic_types[TYPE_uinteger];
        AstNode *lo, *up;
        lo = make_binary_node(p->cs, "MINUS", idx,
                              make_number(p, p->cs->opts.string_base, lineno, NULL),
                              lineno, NULL);
        up = make_binary_node(p->cs, "MINUS", idx,
                              make_number(p, p->cs->opts.string_base, lineno, NULL),
                              lineno, NULL);
        lo = make_typecast(p->cs, uint_t, lo, lineno);
        up = make_typecast(p->cs, uint_t, up, lineno);
        if (!lo || !up) return NULL;
        AstNode *slice = ast_new(p->cs, AST_STRSLICE, lineno);
        ast_add_child(p->cs, slice, base_str);
        ast_add_child(p->cs, slice, lo);
        ast_add_child(p->cs, slice, up);
        slice->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        return strslice_fold(p, slice);
    }

    /* Python make_call (zxbparser.py:386-388):
     *   if entry.class_ is CLASS.unknown and entry.type_ == TYPE.string
     *      and len(args) == 1 and is_numeric(args[0]):
     *       entry = entry.to_var()  # A scalar variable. e.g a$(expr)
     *
     * `<name>$(expr)` where <name>$ is undeclared: access_call's
     * underlying access_id created a CLASS_unknown entry with
     * type_=string (suffix-driven, compiler.c:1033-1042). With a single
     * numeric arg, Python promotes it to a string scalar variable so
     * make_call's CLASS_var-string branch (zxbparser.py:399-411) builds
     * a STRSLICE.  Without this transition the C falls through to the
     * generic FUNCCALL branch and the auto-declared string never lands
     * in data_ast — the W101 `_character: DEFB 00, 00` storage row that
     * Python emits is silently dropped (strsigil's S1-DIVERGE). The
     * accessed=true that the STRSLICE READ branch will set (next block,
     * expr_context gate) feeds VarTranslator.visit_VARDECL so the
     * auto-declared global stays in data_ast at O>1. */
    if (entry && entry->u.id.class_ == CLASS_unknown &&
        type_is_string(entry->type_) &&
        arglist->child_count == 1) {
        AstNode *a0 = arglist->children[0];
        AstNode *a0v = (a0 && a0->tag == AST_ARGUMENT && a0->child_count > 0)
                          ? a0->children[0] : a0;
        if (a0v && a0v->type_ && type_is_numeric(a0v->type_)) {
            entry->u.id.class_ = CLASS_var;
            entry->u.id.declared = true;
        }
    }

    if (entry && entry->u.id.class_ == CLASS_var && type_is_string(entry->type_)) {
        return make_strslice_var(p, entry, arglist, expr_context, lineno);
    }

    /* Function call */
    AstNode *n = ast_new(p->cs, AST_FUNCCALL, lineno);
    if (entry) {
        /* S5.7a: do NOT mark the function entry accessed here. Python's
         * make_call/make_func_call/SymbolCALL.make_node (zxbparser.py:365-420,
         * symbols/call.py:90-112) never call mark_entry_as_accessed for a
         * FUNCCALL; the function `accessed` flag is set transitively and
         * scope-aware ONLY by FunctionGraphVisitor (calls at global scope,
         * optimize.py:161-191 — already ported, functiongraph.c:45-122).
         * Pre-marking here at parse time (scope-blind) wrongly kept a
         * function called only from inside itself/another function
         * `accessed`, defeating the O>1 dead-function DCE
         * (optimize.py:308-318 / optimizer.c:461-473) — the R4
         * `funccall0` mismatch. Global calls are still correctly marked
         * by FunctionGraph, so this is faithful and non-regressive. */
        n->type_ = entry->type_;
    } else {
        /* access_call returned NULL (error already reported) — create placeholder */
        entry = ast_new(p->cs, AST_ID, lineno);
        entry->u.id.name = arena_strdup(&p->cs->arena, name);
        n->type_ = p->cs->default_type;
    }
    ast_add_child(p->cs, n, entry);
    ast_add_child(p->cs, n, arglist);

    /* SymbolCALL.filename = gl.FILENAME at parse point
     * (symbols/call.py:42) — used as fname= for the R3-R10 argument
     * errors in check_call_arguments. */
    if (p->cs->current_file)
        n->u.call.filename = arena_strdup(&p->cs->arena, p->cs->current_file);
    /* Python call.py:102 inline-vs-deferred dispatch (see zxbc.h
     * call.callee_inline): R3-R10 run in the deferred loop only for
     * calls Python would also defer (callee not a finished
     * definition at call-parse time). */
    n->u.call.callee_inline =
        (entry->tag == AST_ID && entry->u.id.params != NULL &&
         !entry->u.id.forwarded);

    /* Track for post-parse validation (check_pending_calls) */
    vec_push(p->cs->function_calls, n);

    /* Python call.py:103 — inline branch runs check_call_arguments at the
     * call site during parsing. Deferred (non-inline) calls are checked
     * later by check_pending_calls (which skips callee_inline calls). */
    inline_check_call_arguments(p, n);

    return n;
}

/* Parse postfix indexing/slicing: expr(...)
 *
 * expr_context distinguishes the READ path (parse_expression, true) from the
 * statement-level WRITE/lvalue target (`a$(i)(j) = rhs`, false). The two
 * diverge in Python's grammar: a READ chains the `string : func_call substr`
 * / `string : func_call LP expr RP` productions (zxbparser.py:2542-2549) into
 * a flat make_strslice, while a WRITE uses the dedicated p_let_arr_substr*
 * productions (zxbparser.py:2733-2815) that the statement handler builds from
 * the ARRAYACCESS-over-ARRAYACCESS shape this function still emits in WRITE
 * context. So the STRSLICE lowering below is gated on expr_context. */
static AstNode *parse_postfix(Parser *p, AstNode *left, bool expr_context) {
    while (check(p, BTOK_LP)) {
        int lineno = p->current.lineno;
        advance(p); /* consume ( */

        /* Parse argument list — may contain TO for string slicing */
        AstNode *arglist = ast_new(p->cs, AST_ARGLIST, lineno);
        bool has_to = false;

        if (!check(p, BTOK_RP)) {
            do {
                /* TO without lower bound — fill defaults per
                 * zxbparser.py:2608-2638 (p_subind_strTO / p_subind_TO):
                 * lower = make_number(0), upper = make_number(65534).
                 * The outer site at ~line 1430 has its own typecast
                 * loop; leave the typecast wrapping to that. */
                if (check(p, BTOK_TO)) {
                    has_to = true;
                    advance(p);
                    AstNode *upper = NULL;
                    if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                        upper = parse_expression(p, PREC_NONE + 1);
                    AstNode *slice = ast_new(p->cs, AST_STRSLICE, lineno);
                    ast_add_child(p->cs, slice,
                                  make_number(p, 0, lineno, NULL));
                    if (!upper)
                        upper = make_number(p, 65534, lineno, NULL);
                    ast_add_child(p->cs, slice, upper);
                    ast_add_child(p->cs, arglist, slice);
                    continue;
                }
                AstNode *arg_expr = parse_expression(p, PREC_NONE + 1);
                if (match(p, BTOK_TO)) {
                    has_to = true;
                    AstNode *upper = NULL;
                    if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                        upper = parse_expression(p, PREC_NONE + 1);
                    int sl_ln = arg_expr ? arg_expr->lineno : lineno;
                    AstNode *slice = ast_new(p->cs, AST_STRSLICE, sl_ln);
                    if (!arg_expr)
                        arg_expr = make_number(p, 0, sl_ln, NULL);
                    ast_add_child(p->cs, slice, arg_expr);
                    if (!upper)
                        upper = make_number(p, 65534, sl_ln, NULL);
                    ast_add_child(p->cs, slice, upper);
                    ast_add_child(p->cs, arglist, slice);
                } else if (arg_expr) {
                    AstNode *arg = ast_new(p->cs, AST_ARGUMENT, arg_expr->lineno);
                    arg->u.argument.byref = false;
                    ast_add_child(p->cs, arg, arg_expr);
                    arg->type_ = arg_expr->type_;
                    ast_add_child(p->cs, arglist, arg);
                }
            } while (match(p, BTOK_COMMA));
        }
        consume(p, BTOK_RP, "Expected ')' after postfix index");

        /* Empty postfix group on a STRING LITERAL base => the string
         * identity (Python p_string_lprp, zxbparser.py:2557-2559):
         *     string : string LP RP   ->   p[0] = p[1]   (returned UNCHANGED)
         * This production reduces only for a bare `string` non-terminal,
         * which a STRC literal IS (p_string_str, :2552-2554 -> `string`).
         * `"STRING"()` therefore yields the static STRING constant itself,
         * taking the __STORE_STR path — not the runtime ARRAYACCESS copy
         * (__STORE_STR2) the else branch below would build.  Chained empty
         * groups (`"STRING"()()`) iterate harmlessly: each `()` reduces the
         * still-`string` literal to itself, matching Python (rc=0).
         *
         * Scoped to AST_STRING ONLY so the accept/reject of every OTHER
         * empty-group base is unchanged: a string VAR (AST_ID, Python's
         * VarRef AttributeError), a func_call (AST_FUNCCALL, no
         * `string : func_call LP RP` production), or an array element
         * (AST_ARRAYACCESS) all fall through to the unmodified else branch
         * exactly as before.  CONST strings never reach here — `c$()` is
         * resolved by parse_call_or_array's CLASS_const branch
         * (parser.c:1832-1837, len(args)==0 -> return entry). */
        if (expr_context && arglist->child_count == 0 &&
            left && left->tag == AST_STRING) {
            continue;  /* p[0] = p[1] — base string unchanged */
        }

        /* String-typed base in a READ context => the postfix group is a
         * substring slice, exactly Python's `string : func_call substr`
         * (zxbparser.py:2543) / `string : func_call LP expr RP` (:2548) /
         * `string : func_call LP RP` (the whole-string copy, :2558 analogue)
         * productions, all of which call make_strslice on the func_call
         * result. A non-string base (real 2-D numeric array, func result,
         * etc.) or a WRITE/lvalue target keeps the ARRAYACCESS path.
         *
         * The empty group `a$(1)()` is EXCLUDED: Python has NO
         * `string : func_call LP RP` production (only `string : string LP RP`,
         * zxbparser.py:2558, for a bare `string` non-terminal — not a
         * func_call), so `func_call ()` is a Syntax Error there. Requiring a
         * non-empty arglist routes the empty group to the legacy ARRAYACCESS
         * path, which rejects the zero-subscript access exactly as before. */
        bool slice_left = expr_context && left &&
                          type_is_string(left->type_) &&
                          arglist->child_count > 0;

        if (slice_left) {
            /* Build a FLAT STRSLICE[base, lower, upper], mirroring
             * make_strslice -> STRSLICE.make_node (strslice.py:59-111) and
             * the canonical NAME-form site (parse_call_or_array:1531-1592):
             *   - each bound: TYPECAST(uinteger, MINUS(bound, string_base))
             *     applied UNCONDITIONALLY — make_binary_node folds the
             *     MINUS for a constant bound but keeps a real node for a
             *     non-constant one (the extra `subu16 t, bound, 0` ->
             *     `pop hl; push hl` at -O0; binary.py:144);
             *   - flatten the per-group inner AST_STRSLICE wrappers into the
             *     outer's 3 direct children [s, lower, upper];
             *   - run strslice_fold (the constant-fold tail) on the result.
             * Both the TO form (`a$(1)(2 TO 4)`) and the single-index form
             * (`a$(1)(2)`, lower==upper==expr) route here. */
            int sbase = p->cs->opts.string_base;
            TypeInfo *uint_t = p->cs->symbol_table->basic_types[TYPE_uinteger];
            AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
            ast_add_child(p->cs, n, left);

            for (int i = 0; i < arglist->child_count; i++) {
                AstNode *inner = arglist->children[i];
                if (!inner) continue;
                if (inner->tag == AST_STRSLICE) {
                    /* TO group: children are [lower, upper].  The `substr`
                     * non-terminal (zxbparser.py:2600-2638, p_subind_str /
                     * p_subind_strTO / p_subind_TOstr / p_subind_TO) ALREADY
                     * `make_typecast(TYPE.uinteger, bound)` each TO bound
                     * BEFORE make_strslice runs.  So the MINUS in
                     * STRSLICE.make_node sees an already-uinteger operand and
                     * common-types to u16 -> `cast ...; subu16 ...,0` (the
                     * MINUS is at 16-bit, AFTER the widening cast).  Mirror
                     * that pre-typecast here, then apply make_node's
                     * MINUS(base)+TYPECAST. */
                    for (int j = 0; j < inner->child_count; j++) {
                        AstNode *bound = inner->children[j];
                        if (!bound) continue;
                        AstNode *pre = make_typecast(p->cs, uint_t, bound,
                                                     lineno);
                        if (pre) bound = pre;
                        bound = make_binary_node(
                            p->cs, "MINUS", bound,
                            make_number(p, sbase, lineno, NULL),
                            lineno, NULL);
                        AstNode *cast = make_typecast(p->cs, uint_t, bound,
                                                      lineno);
                        ast_add_child(p->cs, n, cast ? cast : inner->children[j]);
                    }
                } else {
                    /* Single index: build TWO independent bound chains
                     * (lower==upper==expr), per p_string_func_call_single
                     * make_strslice(s, expr, expr). Unwrap the AST_ARGUMENT
                     * shell the no-TO arglist loop wrapped the expr in. */
                    AstNode *idx = (inner->tag == AST_ARGUMENT &&
                                    inner->child_count > 0)
                                       ? inner->children[0] : inner;
                    AstNode *lo, *up;
                    lo = make_binary_node(
                        p->cs, "MINUS", idx,
                        make_number(p, sbase, lineno, NULL), lineno, NULL);
                    up = make_binary_node(
                        p->cs, "MINUS", idx,
                        make_number(p, sbase, lineno, NULL), lineno, NULL);
                    lo = make_typecast(p->cs, uint_t, lo, lineno);
                    up = make_typecast(p->cs, uint_t, up, lineno);
                    ast_add_child(p->cs, n, lo ? lo : idx);
                    ast_add_child(p->cs, n, up ? up : idx);
                }
            }

            /* slice_left already required a non-empty arglist, so a single
             * index or a TO group always contributes >= 2 bound children
             * (total >= 3 with the base). The guard is purely defensive: a
             * degenerate STRSLICE missing its bounds leaves `left` as the
             * base rather than emitting a malformed node. */
            if (n->child_count >= 3) {
                n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
                left = strslice_fold(p, n);
            }
        } else if (has_to) {
            /* Non-string TO slice in WRITE context, or a non-string base:
             * keep the existing STRSLICE shape (the statement handler reads
             * children[0] to pick its diagnostic; see parser.c:3570). */
            AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
            ast_add_child(p->cs, n, left);
            for (int i = 0; i < arglist->child_count; i++)
                ast_add_child(p->cs, n, arglist->children[i]);
            n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
            left = n;
        } else {
            /* Array access or function call on expression result —
             * always a READ here (postfix on an expression value) =>
             * Python sym.ARRAYLOAD. Preserved unchanged: the WRITE-path
             * chained single-index `a(i)(j) = rhs` relies on this
             * ARRAYACCESS-over-ARRAYACCESS shape (parser.c:3592-3616). */
            AstNode *n = ast_new(p->cs, AST_ARRAYACCESS, lineno);
            n->u.arrayaccess.is_load = true;
            ast_add_child(p->cs, n, left);
            for (int i = 0; i < arglist->child_count; i++)
                ast_add_child(p->cs, n, arglist->children[i]);
            left = n;
        }
    }
    return left;
}

/* Continue parsing binary operators from a given left-hand side.
 * This is the infix portion of the Pratt parser, extracted so it can be
 * used both by parse_expression and by statement-level expression parsing
 * (e.g. expression-as-statement: "test(1) + test(2)"). */
static AstNode *parse_infix(Parser *p, AstNode *left, Precedence min_prec) {
    while (!check(p, BTOK_EOF) && !check(p, BTOK_NEWLINE) && !check(p, BTOK_CO)) {
        Precedence prec = get_precedence(p->current.type);
        if (prec < min_prec) break;

        BTokenType op_type = p->current.type;
        int lineno = p->current.lineno;
        advance(p);

        /* For right-associative operators, use same precedence.
         * For left-associative, use prec+1 */
        Precedence next_prec = is_right_assoc(op_type) ? prec : (Precedence)(prec + 1);
        AstNode *right = parse_expression(p, next_prec);
        if (!right) return left;

        /* Use semantic make_binary_node for type coercion, constant folding,
         * CONSTEXPR wrapping, and string concatenation */
        const char *op_name = operator_name(op_type);
        AstNode *result = make_binary_node(p->cs, op_name, left, right, lineno, NULL);
        if (!result) return left; /* error already reported */
        left = result;
    }

    return left;
}

/* Parse expression with Pratt algorithm */
AstNode *parse_expression(Parser *p, Precedence min_prec) {
    AstNode *left = parse_primary(p);
    if (!left) return NULL;

    /* Handle postfix indexing/slicing: expr(...) */
    left = parse_postfix(p, left, true);

    return parse_infix(p, left, min_prec);
}

/* ----------------------------------------------------------------
 * Statement parsing
 * ---------------------------------------------------------------- */

static AstNode *parse_statement(Parser *p);
static AstNode *parse_if_statement(Parser *p);
static AstNode *parse_for_statement(Parser *p);
static AstNode *parse_while_statement(Parser *p);
static AstNode *parse_do_statement(Parser *p);
static AstNode *parse_dim_statement(Parser *p);
static AstNode *dim_build_scalar(Parser *p, const char **names, int name_count,
                                 const char *name, TypeInfo *type,
                                 bool had_as_clause, AstNode *at_expr,
                                 AstNode *init_expr, bool is_const, int lineno);
static AstNode *dim_build_array(Parser *p, const char *name, AstNode *bounds,
                                TypeInfo *type, AstNode *arr_at_expr,
                                AstNode *init, int lineno);
static AstNode *parse_print_statement(Parser *p);
static AstNode *parse_sub_or_func_decl(Parser *p, bool is_function, bool is_declare);

/* Graphics attributes for PLOT/DRAW/CIRCLE — faithful port of
 * zxbparser.py p_attr_list / p_attr (:2006-2027):
 *
 *   attr      : (OVER|INVERSE|INK|PAPER|BRIGHT|FLASH) expr
 *               -> make_sentence(ln, p[1]+"_TMP",
 *                                 make_typecast(TYPE.ubyte, p[2], ln))
 *   attr_list : attr SC                 -> p[0] = p[1]   (the lone attr)
 *             | attr_list attr SC       -> p[0] = make_block(p[1], p[2])
 *
 * make_block == SymbolBLOCK.make_node (block.py:20-23): builds a BLOCK and
 * append()s; append() FLATTENS nested BLOCKs (block.py:43-51). So the
 * attr_list node is:
 *   - 1 attr  -> the single "<NAME>_TMP" SENTENCE node itself
 *   - N>=2    -> a flat BLOCK whose children are the N "<NAME>_TMP"
 *               SENTENCEs in source order
 * Returned as the SINGLE last child of PLOT/DRAW/DRAW3/CIRCLE so the
 * translator's `yield TMP_HAS_ATTR` visits it (BLOCK->each child, or the
 * lone SENTENCE) -> tr_visit_attr_tmp emits the same attr IC as Python.
 * BOLD/ITALIC are NOT in p_attr (only in PRINT) so they are not matched
 * here — `DRAW BOLD 1;...` correctly fails as a syntax error.
 *
 * Returns NULL when no attr is present (Python: production simply absent;
 * make_sentence then has no attr child -> check_attr returns None). */
static AstNode *parse_gfx_attributes(Parser *p) {
    AstNode *attr_list = NULL;   /* p[0] of attr_list */
    for (;;) {
        const char *attr_name = NULL;
        if (match(p, BTOK_INK))          attr_name = "INK_TMP";
        else if (match(p, BTOK_PAPER))   attr_name = "PAPER_TMP";
        else if (match(p, BTOK_BRIGHT))  attr_name = "BRIGHT_TMP";
        else if (match(p, BTOK_FLASH))   attr_name = "FLASH_TMP";
        else if (match(p, BTOK_OVER))    attr_name = "OVER_TMP";
        else if (match(p, BTOK_INVERSE)) attr_name = "INVERSE_TMP";
        if (!attr_name) break;
        int attr_ln = p->previous.lineno;
        AstNode *val = parse_expression(p, PREC_NONE + 1);
        /* p_attr: make_typecast(TYPE.ubyte, p[2], ln) */
        val = make_typecast(p->cs,
                            p->cs->symbol_table->basic_types[TYPE_ubyte],
                            val, attr_ln);
        AstNode *attr_sent = make_sentence_node(p, attr_name, attr_ln);
        if (val) ast_add_child(p->cs, attr_sent, val);
        consume(p, BTOK_SC, "Expected ';' after graphics attribute");

        if (attr_list == NULL) {
            /* attr_list : attr SC  ->  p[0] = p[1] */
            attr_list = attr_sent;
        } else {
            /* attr_list : attr_list attr SC  ->  make_block(p[1], p[2]),
             * flattening a pre-existing BLOCK (block.py append). */
            if (attr_list->tag != AST_BLOCK) {
                AstNode *b = make_block_node(p, attr_list->lineno);
                ast_add_child(p->cs, b, attr_list);
                attr_list = b;
            }
            ast_add_child(p->cs, attr_list, attr_sent);
        }
    }
    return attr_list;
}

/* apply_pragma_option — the `#pragma NAME = VALUE` option-set side effect,
 * extracted from parse_statement so the Phase-D PLY preproc_line reduces
 * (371/372/373) apply the BYTE-IDENTICAL OPTIONS mutation. Faithful port of
 * src/zxbc/zxbparser.py:3237-3245 p_preproc_line_pragma_option (setattr(OPTIONS,
 * name, value) with the registered-option type coercion; UndefinedOptionError
 * -> W300; InvalidValueError -> exit 1). `raw` is the value token text (a str in
 * every grammar alternative); `name_lineno` is p.lineno(2) (the NAME token). */
static void apply_pragma_option(Parser *p, const char *opt_name,
                                int name_lineno, const char *raw) {
    /* --- Type tables: EXACT Python-registered names --------- *
     * bool options: config.py EXPLICIT/STRICT/STRICT_BOOL/CHECK_MEMORY/
     *   CHECK_ARRAYS/ENABLE_BREAK/CASE_INS/DEFAULT_BYREF/USE_BASIC_LOADER/
     *   AUTORUN/FORCE_ASM_BRACKET/ASM_ZXNEXT/EMIT_BACKEND/HIDE_WARNING_CODES,
     *   "sinclair"; backend "headerless".
     * int options: config.py DEBUG(debug_level)/O_LEVEL/ARRAY_BASE/STR_BASE/
     *   MAX_SYN_ERRORS/EXPECTED_WARNINGS; backend org/heap_size/heap_address.
     * str options: config.py MEMORY_MAP/OUTPUT_FILE_TYPE/INCLUDE_PATH/ARCH/
     *   OUTPUT_FILENAME/INPUT_FILENAME/STDERR_FILENAME/PROJECT_FILENAME.
     * Registered-but-no-C-field (heap_*_label str, opt_strategy, stdin/out/err,
     * __DEFINES dict): still KNOWN to Python, so NOT an unknown pragma — accept
     * without warning; no owned fixture sets them so no observable asm effect. */
    #define NAME_IS(s) (strcmp(opt_name, (s)) == 0)

    /* Python bool coercion of a str (options.py:121-131), value.lower() keyed.
     * Returns 1/0, or -1 if not a key (KeyError -> InvalidValueError path). */
    int bcoerce = -1;
    {
        char lo[16]; size_t i = 0;
        for (; raw[i] && i < sizeof(lo) - 1; i++)
            lo[i] = (char)tolower((unsigned char)raw[i]);
        lo[i] = '\0';
        if (!raw[i]) {  /* only short tokens can match a key */
            if (!strcmp(lo, "true") || !strcmp(lo, "on") ||
                !strcmp(lo, "+")    || !strcmp(lo, "yes")) bcoerce = 1;
            else if (!strcmp(lo, "false") || !strcmp(lo, "off") ||
                     !strcmp(lo, "-")     || !strcmp(lo, "no")) bcoerce = 0;
        }
    }

    bool is_bool_opt = false, is_int_opt = false, is_str_opt = false;
    bool known = true;

    if      (NAME_IS("explicit"))           { is_bool_opt = true; }
    else if (NAME_IS("strict"))             { is_bool_opt = true; }
    else if (NAME_IS("strict_bool"))        { is_bool_opt = true; }
    else if (NAME_IS("memory_check"))       { is_bool_opt = true; }
    else if (NAME_IS("array_check"))        { is_bool_opt = true; }
    else if (NAME_IS("enable_break"))       { is_bool_opt = true; }
    else if (NAME_IS("case_insensitive"))   { is_bool_opt = true; }
    else if (NAME_IS("default_byref"))      { is_bool_opt = true; }
    else if (NAME_IS("use_basic_loader"))   { is_bool_opt = true; }
    else if (NAME_IS("autorun"))            { is_bool_opt = true; }
    else if (NAME_IS("force_asm_brackets")) { is_bool_opt = true; }
    else if (NAME_IS("zxnext"))             { is_bool_opt = true; }
    else if (NAME_IS("emit_backend"))       { is_bool_opt = true; }
    else if (NAME_IS("hide_warning_codes")) { is_bool_opt = true; }
    else if (NAME_IS("sinclair"))           { is_bool_opt = true; }
    else if (NAME_IS("headerless"))         { is_bool_opt = true; }
    else if (NAME_IS("debug_level"))        { is_int_opt = true; }
    else if (NAME_IS("optimization_level")) { is_int_opt = true; }
    else if (NAME_IS("array_base"))         { is_int_opt = true; }
    else if (NAME_IS("string_base"))        { is_int_opt = true; }
    else if (NAME_IS("max_syntax_errors"))  { is_int_opt = true; }
    else if (NAME_IS("expected_warnings"))  { is_int_opt = true; }
    else if (NAME_IS("org"))                { is_int_opt = true; }
    else if (NAME_IS("heap_size"))          { is_int_opt = true; }
    else if (NAME_IS("heap_address"))       { is_int_opt = true; }
    else if (NAME_IS("memory_map"))         { is_str_opt = true; }
    else if (NAME_IS("output_file_type"))   { is_str_opt = true; }
    else if (NAME_IS("include_path"))       { is_str_opt = true; }
    else if (NAME_IS("architecture"))       { is_str_opt = true; }
    else if (NAME_IS("output_filename"))    { is_str_opt = true; }
    else if (NAME_IS("input_filename"))     { is_str_opt = true; }
    else if (NAME_IS("stderr_filename"))    { is_str_opt = true; }
    else if (NAME_IS("project_filename"))   { is_str_opt = true; }
    /* Registered, known to Python, but no observable C field: accept silently. */
    else if (NAME_IS("heap_start_label") || NAME_IS("heap_size_label") ||
             NAME_IS("opt_strategy") || NAME_IS("stdin") ||
             NAME_IS("stdout") || NAME_IS("stderr")) { /* no-op */ }
    else { known = false; }

    if (!known) {
        /* Python: UndefinedOptionError -> warning_ignoring_unknown_pragma
         * (errmsg.py:188) at p.lineno(2) (the NAME token line). */
        warn_unknown_pragma(p->cs, name_lineno, opt_name);
    } else if (is_bool_opt) {
        if (bcoerce < 0) {
            /* Python InvalidValueError (uncaught -> exit 1). */
            zxbc_error(p->cs, name_lineno,
                       "Invalid value '%s' for option '%s'. Value type must be '<class 'bool'>'",
                       raw, opt_name);
        } else {
            bool b = (bcoerce == 1);
            if      (NAME_IS("explicit"))           p->cs->opts.explicit_ = b;
            else if (NAME_IS("strict"))             p->cs->opts.strict = b;
            else if (NAME_IS("strict_bool"))        p->cs->opts.strict_bool = b;
            else if (NAME_IS("memory_check"))       p->cs->opts.memory_check = b;
            else if (NAME_IS("array_check"))        p->cs->opts.array_check = b;
            else if (NAME_IS("enable_break"))       p->cs->opts.enable_break = b;
            else if (NAME_IS("case_insensitive"))   p->cs->opts.case_insensitive = b;
            else if (NAME_IS("default_byref"))      p->cs->opts.default_byref = b;
            else if (NAME_IS("use_basic_loader"))   p->cs->opts.use_basic_loader = b;
            else if (NAME_IS("autorun"))            p->cs->opts.autorun = b;
            else if (NAME_IS("force_asm_brackets")) p->cs->opts.force_asm_brackets = b;
            else if (NAME_IS("zxnext"))             p->cs->opts.zxnext = b;
            else if (NAME_IS("emit_backend"))       p->cs->opts.emit_backend = b;
            else if (NAME_IS("hide_warning_codes")) p->cs->opts.hide_warning_codes = b;
            else if (NAME_IS("sinclair"))           p->cs->opts.sinclair = b;
            else if (NAME_IS("headerless"))         p->cs->opts.headerless = b;
        }
    } else if (is_int_opt) {
        /* Python int(str): all-decimal-digits per the INTEGER regex; a
         * non-numeric str -> ValueError -> uncaught InvalidValueError -> exit 1.
         * parse_int mirrors the 'org' config arm (args.c:103). */
        int iv = 0;
        if (!parse_int(raw, &iv)) {
            zxbc_error(p->cs, name_lineno,
                       "Invalid value '%s' for option '%s'. Value type must be '<class 'int'>'",
                       raw, opt_name);
        } else {
            if      (NAME_IS("debug_level"))        p->cs->opts.debug_level = iv;
            else if (NAME_IS("optimization_level")) p->cs->opts.optimization_level = iv;
            else if (NAME_IS("array_base"))         p->cs->opts.array_base = iv;
            else if (NAME_IS("string_base"))        p->cs->opts.string_base = iv;
            else if (NAME_IS("max_syntax_errors"))  p->cs->opts.max_syntax_errors = iv;
            else if (NAME_IS("expected_warnings"))  p->cs->opts.expected_warnings = iv;
            else if (NAME_IS("org"))                p->cs->opts.org = iv;
            else if (NAME_IS("heap_size"))          p->cs->opts.heap_size = iv;
            else if (NAME_IS("heap_address"))       p->cs->opts.heap_address = iv;
        }
    } else if (is_str_opt) {
        char *sv = arena_strdup(&p->cs->arena, raw);
        if      (NAME_IS("memory_map"))       p->cs->opts.memory_map = sv;
        else if (NAME_IS("output_file_type")) p->cs->opts.output_file_type = sv;
        else if (NAME_IS("include_path"))     p->cs->opts.include_path = sv;
        else if (NAME_IS("architecture"))     p->cs->opts.architecture = sv;
        else if (NAME_IS("output_filename"))  p->cs->opts.output_filename = sv;
        else if (NAME_IS("input_filename"))   p->cs->opts.input_filename = sv;
        else if (NAME_IS("stderr_filename"))  p->cs->opts.stderr_filename = sv;
        else if (NAME_IS("project_filename")) p->cs->opts.project_filename = sv;
    }

    #undef NAME_IS
}

/* data_build — the DATA per-item static/FUNCPTR split + gl.DATAS append,
 * extracted from parse_statement so the Phase-D `statement : DATA arguments`
 * reduce (157) performs the byte-identical side effects. Faithful port of
 * src/zxbc/zxbparser.py:1747-1772 p_data (the FUNCTION_LEVEL guard already
 * checked by the caller's :1742-1745). `values` are the parsed value exprs (==
 * the ARGUMENT.value of each p[2] child). DATA contributes NO AST node (Python
 * never sets p[0]) — this returns void; the caller returns NULL. */
static void data_build(Parser *p, AstNode **values, int nvalues,
                       char *label_name, AstNode *label_entry, int ln) {
    DataItem *items = arena_alloc(&p->cs->arena,
        (size_t)nvalues * sizeof(DataItem));
    int nitems = 0;
    for (int ci = 0; ci < nvalues; ci++) {
        AstNode *value = values[ci];
        if (!value) continue;

        /* :1749-1751  is_static(value) -> keep as a data item. */
        if (check_is_static(value)) {
            items[nitems].is_funcdecl = false;
            items[nitems].node = value;
            nitems++;
            continue;
        }

        /* :1753  new_lbl = f"__DATA__FUNCPTR__{len(DATA_FUNCTIONS)}" */
        char fnbuf[40];
        snprintf(fnbuf, sizeof(fnbuf), "__DATA__FUNCPTR__%d",
                 (int)p->cs->data_functions.len);

        /* :1754-1762  make_func_declaration(new_lbl, lineno, type_=value.type_,
         * class_=CLASS.function) on a fresh unique name -> new FUNCTION entry;
         * fastcall, empty local scope. */
        AstNode *func =
            symboltable_declare(p->cs->symbol_table, p->cs,
                                fnbuf, ln, CLASS_function);
        if (!func)
            continue;
        func->u.id.declared = true;
        func->type_ = value->type_;
        func->u.id.convention = CONV_fastcall;
        func->u.id.local_size = 0;
        func->u.id.param_size = 0;
        func->u.id.local_entries = NULL;
        func->u.id.local_entries_count = 0;

        /* :1765-1766  sent = make_sentence("RETURN", func, value);
         *             func.ref.body = make_block(sent) */
        AstNode *ret = make_sentence_node(p, "RETURN", ln);
        ast_add_child(p->cs, ret, func);
        ast_add_child(p->cs, ret, value);
        AstNode *body = make_block_node(p, ln);
        ast_add_child(p->cs, body, ret);

        AstNode *decl = ast_new(p->cs, AST_FUNCDECL, ln);
        AstNode *params = ast_new(p->cs, AST_PARAMLIST, ln);
        ast_add_child(p->cs, decl, func);
        ast_add_child(p->cs, decl, params);
        ast_add_child(p->cs, decl, body);
        decl->type_ = value->type_;
        func->u.id.body = body;

        /* :1764  gl.DATA_FUNCTIONS.append(func) */
        vec_push(p->cs->data_functions, decl);

        /* :1767  datas_.append(entry) */
        items[nitems].is_funcdecl = true;
        items[nitems].node = decl;
        nitems++;
    }

    /* :1770  gl.DATAS.append(DataRef(label_, datas_)). */
    DataRef *dr = arena_alloc(&p->cs->arena, sizeof(DataRef));
    dr->label_name = label_name;
    dr->label_entry = label_entry;
    dr->items = items;
    dr->item_count = nitems;
    vec_push(p->cs->datas, dr);

    /* :1771-1772  gl.DATA_PTR_CURRENT = current_data_label(). */
    p->cs->data_ptr_current = current_data_label(p->cs);
}

/* read_build — the READ per-target validation + READ-sentence block, extracted
 * from parse_statement so the Phase-D `statement : READ arguments` reduce (161)
 * builds the byte-identical tree. Faithful port of zxbparser.py:1788-1835
 * p_read, matched to the PRODUCTION's target check (the byte-clean baseline):
 * an array ID -> "Cannot read", a non-ID/non-ARRAYACCESS -> "Can only read a
 * variable or an array element". Returns the BLOCK of READ sentences. */
static AstNode *read_build(Parser *p, AstNode **targets, int ntargets, int ln) {
    p->cs->data_is_used = true;  /* gl.DATA_IS_USED = True */
    AstNode *block = make_block_node(p, ln);
    for (int i = 0; i < ntargets; i++) {
        AstNode *s = make_sentence_node(p, "READ", ln);
        AstNode *target = targets[i];
        if (target) {
            if (target->tag == AST_ID && target->u.id.class_ == CLASS_array) {
                zxbc_error(p->cs, ln, "Cannot read '%s'. It's an array",
                           target->u.id.name);
            } else if (target->tag != AST_ID && target->tag != AST_ARRAYACCESS) {
                zxbc_error(p->cs, ln,
                           "Syntax error. Can only read a variable or an array element");
            }
            ast_add_child(p->cs, s, target);
        }
        ast_add_child(p->cs, block, s);
    }
    return block;
}

/* Parse a single statement */
static AstNode *parse_statement(Parser *p) {
    int lineno = p->current.lineno;

    /* Label at start of line */
    if (check(p, BTOK_LABEL)) {
        advance(p);
        const char *label_text = p->previous.sval;
        char label_buf[32];
        if (!label_text) {
            snprintf(label_buf, sizeof(label_buf), "%d", (int)p->previous.numval);
            label_text = label_buf;
        }

        /* Label definition + the SENTENCE("LABEL") node (extracted to
         * label_define so the Phase-D `label : LABEL` reduce reuses it). */
        AstNode *lbl_sent = label_define(p, label_text, p->previous.lineno);

        /* If followed by ':', consume it */
        match(p, BTOK_CO);

        /* A label can be followed by a statement on the same line,
         * but NOT by block-ending keywords */
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) &&
            !check(p, BTOK_LOOP) && !check(p, BTOK_NEXT) &&
            !check(p, BTOK_WEND) && !check(p, BTOK_ELSE) &&
            !check(p, BTOK_ELSEIF) && !check(p, BTOK_ENDIF) &&
            !check(p, BTOK_END)) {
            AstNode *stmt = parse_statement(p);
            AstNode *block = make_block_node(p, lineno);
            ast_add_child(p->cs, block, lbl_sent);
            if (stmt) ast_add_child(p->cs, block, stmt);
            return block;
        }

        return lbl_sent;
    }

    /* LET assignment */
    if (match(p, BTOK_LET)) {
        p->cs->let_assignment = true;
        /* Fall through to ID handling */
        if (!check(p, BTOK_ID) && !check(p, BTOK_ARRAY_ID)) {
            parser_error(p, "Expected variable name after LET");
            p->cs->let_assignment = false;
            return NULL;
        }
    }

    /* DIM */
    if (check(p, BTOK_DIM) || check(p, BTOK_CONST)) {
        return parse_dim_statement(p);
    }

    /* IF */
    if (check(p, BTOK_IF)) {
        return parse_if_statement(p);
    }

    /* FOR */
    if (check(p, BTOK_FOR)) {
        return parse_for_statement(p);
    }

    /* WHILE */
    if (check(p, BTOK_WHILE)) {
        return parse_while_statement(p);
    }

    /* DO */
    if (check(p, BTOK_DO)) {
        return parse_do_statement(p);
    }

    /* PRINT */
    if (check(p, BTOK_PRINT)) {
        return parse_print_statement(p);
    }

    /* ASM inline */
    if (match(p, BTOK_ASM)) {
        return make_asm_node(p, p->previous.sval ? p->previous.sval : "", p->previous.lineno);
    }

    /* GOTO / GOSUB */
    if (match(p, BTOK_GOTO) || match(p, BTOK_GOSUB)) {
        bool is_gosub = (p->previous.type == BTOK_GOSUB);
        const char *kind = is_gosub ? "GOSUB" : "GOTO";
        int ln = p->previous.lineno;
        /* GOSUB not allowed inside SUB or FUNCTION (matching Python) */
        if (is_gosub && p->cs->function_level.len > 0) {
            zxbc_error(p->cs, ln, "GOSUB not allowed within SUB or FUNCTION");
        }
        if (!check(p, BTOK_ID) && !check(p, BTOK_LABEL) && !check(p, BTOK_NUMBER)) {
            parser_error(p, "Expected label after GOTO/GOSUB");
            return NULL;
        }
        advance(p);
        const char *label = p->previous.sval;
        char buf[32];
        if (!label) {
            snprintf(buf, sizeof(buf), "%d", (int)p->previous.numval);
            label = buf;
        }
        AstNode *s = make_sentence_node(p, kind, ln);
        AstNode *lbl = ast_new(p->cs, AST_ID, ln);
        lbl->u.id.name = arena_strdup(&p->cs->arena, label);
        lbl->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, s, lbl);
        return s;
    }

    /* GO TO / GO SUB (two-word form) */
    if (match(p, BTOK_GO)) {
        bool is_sub = match(p, BTOK_SUB);
        if (!is_sub) consume(p, BTOK_TO, "Expected TO or SUB after GO");
        const char *kind = is_sub ? "GOSUB" : "GOTO";
        int ln = p->previous.lineno;
        if (is_sub && p->cs->function_level.len > 0) {
            zxbc_error(p->cs, ln, "GOSUB not allowed within SUB or FUNCTION");
        }
        if (!check(p, BTOK_ID) && !check(p, BTOK_LABEL) && !check(p, BTOK_NUMBER)) {
            parser_error(p, "Expected label after GO TO/GO SUB");
            return NULL;
        }
        advance(p);
        const char *label = p->previous.sval;
        char buf[32];
        if (!label) {
            snprintf(buf, sizeof(buf), "%d", (int)p->previous.numval);
            label = buf;
        }
        AstNode *s = make_sentence_node(p, kind, ln);
        AstNode *lbl = ast_new(p->cs, AST_ID, ln);
        lbl->u.id.name = arena_strdup(&p->cs->arena, label);
        lbl->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, s, lbl);
        return s;
    }

    /* RETURN — S5.7e. Faithful port of Python p_return (zxbparser.py:
     * 2099-2110, "statement : RETURN") and p_return_expr (:2113-2154,
     * "statement : RETURN expr"). The enclosing-function scope stack is
     * p->cs->function_level (Python's gl.FUNCTION_LEVEL); its top entry is
     * the resolved FUNCTION/SUB symbol-table ID (parser.c:2791
     * vec_push(function_level, id_node) — class_/convention/type_/mangled
     * all set). The faithful node shapes:
     *   - global scope (function_level empty), bare RETURN  -> 0 children
     *     (GOSUB return); RETURN expr -> error, drop.
     *   - inside a SUB, bare RETURN -> [funcref]   (1 child).
     *   - inside a FUNCTION, RETURN expr ->
     *       [funcref, make_typecast(func->type_, expr, ln)]  (2 children;
     *       make_typecast returning NULL collapses to [funcref], exactly
     *       as Python's SymbolSENTENCE drops None children, sentence.py:20).
     * visit_RETURN (translator.py:699-706 / tr_visit_return) reads
     * child[0] only for the func mangled (`<mangled>__leave`) and
     * child[1] for the value's type_/t. */
    if (match(p, BTOK_RETURN)) {
        int ln = p->previous.lineno;
        bool has_expr = !check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) &&
                        !check(p, BTOK_CO);
        AstNode *func = (p->cs->function_level.len > 0)
                          ? p->cs->function_level.data[p->cs->function_level.len - 1]
                          : NULL;

        if (!has_expr) {
            /* p_return (zxbparser.py:2099-2110) */
            if (!func) {
                /* not FUNCTION_LEVEL -> GOSUB return, 0-child sentence. */
                return make_sentence_node(p, "RETURN", ln);
            }
            if (func->u.id.class_ != CLASS_sub) {
                /* error(...) "Function must RETURN a value."; p[0]=None. */
                zxbc_error(p->cs, ln,
                           "Syntax Error: Function must RETURN a value.");
                return NULL;
            }
            /* make_sentence(lineno, "RETURN", FUNCTION_LEVEL[-1]) -> 1 child */
            AstNode *s = make_sentence_node(p, "RETURN", ln);
            ast_add_child(p->cs, s, func);
            return s;
        }

        /* p_return_expr (zxbparser.py:2113-2154) */
        if (!func) {
            /* not FUNCTION_LEVEL -> error, drop. Consume the expr so the
             * statement stream stays aligned (Python's parser still
             * reduces `expr`; only p[0] becomes None). */
            zxbc_error(p->cs, ln,
                       "Syntax Error: Returning value out of FUNCTION");
            (void)parse_expression(p, PREC_NONE + 1);
            return NULL;
        }
        if (func->u.id.class_ == CLASS_unknown) {
            /* This function was not correctly declared -> p[0]=None. */
            (void)parse_expression(p, PREC_NONE + 1);
            return NULL;
        }
        if (func->u.id.class_ != CLASS_function) {
            zxbc_error(p->cs, ln,
                       "Syntax Error: SUBs cannot return a value");
            (void)parse_expression(p, PREC_NONE + 1);
            return NULL;
        }
        if (func->type_ == NULL) {
            /* There was an error in the Function declaration -> None. */
            (void)parse_expression(p, PREC_NONE + 1);
            return NULL;
        }

        AstNode *expr = parse_expression(p, PREC_NONE + 1);

        /* is_numeric(p[2]) == expr is basic & numeric == not a string.
         * Python's two string/numeric gates (zxbparser.py:2133-2147):
         *   is_numeric(expr) and func.type_==string  -> error
         *   not is_numeric(expr) and func.type_!=string -> error
         * make_typecast already enforces the string<->number conversion
         * ban (compiler.c:596-603); replicate the *parser-level* gates
         * here so the dropped-statement (p[0]=None) shape matches. */
        if (expr) {
            bool expr_is_string = type_is_string(expr->type_);
            bool func_is_string = type_is_string(func->type_);
            if (!expr_is_string && func_is_string) {
                zxbc_error(p->cs, expr->lineno,
                           "Type Error: Function must return a string, not a numeric value");
                return NULL;
            }
            if (expr_is_string && !func_is_string) {
                zxbc_error(p->cs, expr->lineno,
                           "Type Error: Function must return a numeric value, not a string");
                return NULL;
            }
        }

        /* make_sentence(lineno, "RETURN", FUNCTION_LEVEL[-1],
         *   make_typecast(FUNCTION_LEVEL[-1].type_, p[2], lineno)).
         * make_typecast NULL -> SENTENCE drops it -> [funcref] (1 child). */
        AstNode *cast = make_typecast(p->cs, func->type_, expr, ln);
        AstNode *s = make_sentence_node(p, "RETURN", ln);
        ast_add_child(p->cs, s, func);
        if (cast) ast_add_child(p->cs, s, cast);
        return s;
    }

    /* END */
    if (match(p, BTOK_END)) {
        int ln = p->previous.lineno;
        /* END IF / END SUB / END FUNCTION / END WHILE at top level = syntax error.
         * These should only appear inside their respective blocks. */
        if (check(p, BTOK_IF)) {
            advance(p);
            zxbc_error(p->cs, ln, "Syntax Error. Unexpected token 'IF' <IF>");
            return make_nop(p);
        }
        if (check(p, BTOK_SUB) || check(p, BTOK_FUNCTION) || check(p, BTOK_WHILE)) {
            advance(p);
            return make_nop(p);
        }
        AstNode *s = make_sentence_node(p, "END", ln);
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            if (expr) ast_add_child(p->cs, s, expr);
        } else {
            ast_add_child(p->cs, s, make_number(p, 0, ln, p->cs->symbol_table->basic_types[TYPE_uinteger]));
        }
        return s;
    }

    /* ERROR expr */
    if (match(p, BTOK_ERROR_KW)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "ERROR", ln);
        AstNode *code = parse_expression(p, PREC_NONE + 1);
        if (code) ast_add_child(p->cs, s, code);
        return s;
    }

    /* STOP  (zxbparser.py p_stop_raise ~1665-1674): default code is
     * MINUS(typecast(ubyte,NUMBER(9)), NUMBER(1)) which folds to 8
     * (BASIC error "STOP statement"). Bare-STOP C was emitting 9
     * directly (off-by-one in label_decl3/slice2/stoperr). */
    if (match(p, BTOK_STOP)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "STOP", ln);
        TypeInfo *ubyte_t = p->cs->symbol_table->basic_types[TYPE_ubyte];
        AstNode *code;
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            code = parse_expression(p, PREC_NONE + 1);
        } else {
            AstNode *nine = make_number(p, 9, ln, NULL);
            AstNode *q = make_typecast(p->cs, ubyte_t, nine, ln);
            AstNode *one = make_number(p, 1, ln, NULL);
            code = make_binary_node(p->cs, "MINUS", q, one, ln, NULL);
        }
        ast_add_child(p->cs, s, code);
        return s;
    }

    /* CLS */
    if (match(p, BTOK_CLS)) {
        return make_sentence_node(p, "CLS", p->previous.lineno);
    }

    /* BORDER expr  (zxbparser.py:944-946:
     *   make_sentence(ln,"BORDER",make_typecast(TYPE.ubyte,p[2],ln))) */
    if (match(p, BTOK_BORDER)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "BORDER", ln);
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        expr = make_typecast(p->cs,
                             p->cs->symbol_table->basic_types[TYPE_ubyte],
                             expr, ln);
        if (expr) ast_add_child(p->cs, s, expr);
        return s;
    }

    /* PAUSE expr  (zxbparser.py:2159:
     *   make_sentence(ln,"PAUSE",make_typecast(TYPE.uinteger,p[2],ln))) */
    if (match(p, BTOK_PAUSE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "PAUSE", ln);
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        expr = make_typecast(p->cs,
                             p->cs->symbol_table->basic_types[TYPE_uinteger],
                             expr, ln);
        if (expr) ast_add_child(p->cs, s, expr);
        return s;
    }

    /* POKE [type] addr, val  —  six forms from Python grammar:
     *   POKE expr COMMA expr              |  POKE LP expr COMMA expr RP
     *   POKE type expr COMMA expr         |  POKE LP type expr COMMA expr RP
     *   POKE type COMMA expr COMMA expr   |  POKE LP type COMMA expr COMMA expr RP
     *
     * When LP is consumed, we parse the args inside and expect RP at the end.
     * If addr is followed by RP before COMMA, the LP was a parenthesized
     * expression (e.g. POKE (dataSprite), val) — close it and continue. */
    if (match(p, BTOK_POKE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "POKE", ln);
        bool paren = match(p, BTOK_LP);

        /* Optional type name — stored on POKE sentence for semantic phase */
        TypeInfo *poke_type = parse_type_name(p);
        if (poke_type) {
            match(p, BTOK_COMMA); /* optional comma after type */
            s->type_ = poke_type;
        }

        AstNode *addr = parse_expression(p, PREC_NONE + 1);

        /* Disambiguate: if we consumed LP and see RP before COMMA,
         * the LP was part of a parenthesized address expression.
         * Close the parens and continue as non-paren form. */
        if (paren && check(p, BTOK_RP) && !check(p, BTOK_COMMA)) {
            advance(p); /* consume ) — closes the parenthesized address */
            paren = false;
        }

        consume(p, BTOK_COMMA, "Expected ',' after POKE address");
        AstNode *val = parse_expression(p, PREC_NONE + 1);
        if (paren) consume(p, BTOK_RP, "Expected ')' after POKE");
        /* p_poke/p_poke2/p_poke3 (zxbparser.py:2162-2207):
         *   addr := make_typecast(TYPE.uinteger, addr)
         *   val  := make_typecast(numbertype or TYPE.ubyte, val) */
        addr = make_typecast(p->cs,
                             p->cs->symbol_table->basic_types[TYPE_uinteger],
                             addr, ln);
        TypeInfo *vt = poke_type ? poke_type
                     : p->cs->symbol_table->basic_types[TYPE_ubyte];
        val = make_typecast(p->cs, vt, val, ln);
        if (addr) ast_add_child(p->cs, s, addr);
        if (val) ast_add_child(p->cs, s, val);
        return s;
    }

    /* OUT port, val  (zxbparser.py:2211-2216):
     *   make_sentence(ln,"OUT",
     *     make_typecast(TYPE.uinteger,p[2],p.lineno(3)),
     *     make_typecast(TYPE.ubyte,p[4],p.lineno(4))) */
    if (match(p, BTOK_OUT)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "OUT", ln);
        AstNode *port = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' after OUT port");
        int comma_ln = p->previous.lineno;
        AstNode *val = parse_expression(p, PREC_NONE + 1);
        int val_ln = p->previous.lineno;
        port = make_typecast(p->cs,
                             p->cs->symbol_table->basic_types[TYPE_uinteger],
                             port, comma_ln);
        val = make_typecast(p->cs,
                            p->cs->symbol_table->basic_types[TYPE_ubyte],
                            val, val_ln);
        if (port) ast_add_child(p->cs, s, port);
        if (val) ast_add_child(p->cs, s, val);
        return s;
    }

    /* BEEP duration, pitch  (zxbparser.py:1057-1063):
     *   make_sentence(ln,"BEEP",
     *     make_typecast(TYPE.float_,p[2],p.lineno(1)),
     *     make_typecast(TYPE.float_,p[4],p.lineno(3))) */
    if (match(p, BTOK_BEEP)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "BEEP", ln);
        TypeInfo *float_t = p->cs->symbol_table->basic_types[TYPE_float];
        AstNode *dur = parse_expression(p, PREC_NONE + 1);
        dur = make_typecast(p->cs, float_t, dur, ln);
        consume(p, BTOK_COMMA, "Expected ',' in BEEP");
        int comma_ln = p->previous.lineno;
        AstNode *pitch = parse_expression(p, PREC_NONE + 1);
        pitch = make_typecast(p->cs, float_t, pitch, comma_ln);
        if (dur) ast_add_child(p->cs, s, dur);
        if (pitch) ast_add_child(p->cs, s, pitch);
        return s;
    }

    /* RANDOMIZE [expr]  (zxbparser.py:1047-1054):
     *   no arg -> make_number(0, type_=TYPE.ulong)
     *   expr   -> make_typecast(TYPE.ulong, p[2], ln) */
    if (match(p, BTOK_RANDOMIZE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "RANDOMIZE", ln);
        TypeInfo *ulong_t = p->cs->symbol_table->basic_types[TYPE_ulong];
        AstNode *expr;
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            expr = parse_expression(p, PREC_NONE + 1);
            expr = make_typecast(p->cs, ulong_t, expr, ln);
        } else {
            expr = make_number(p, 0, ln, ulong_t);
        }
        if (expr) ast_add_child(p->cs, s, expr);
        return s;
    }

    /* PLOT [attr_list] expr COMMA expr
     * zxbparser.py p_statement_plot / p_statement_plot_attr (:949-967):
     *   make_sentence(ln,"PLOT", make_typecast(ubyte,x,ln),
     *                            make_typecast(ubyte,y,ln) [, attr_list])
     * Child order: x, y, then attr_list LAST (single node). */
    if (match(p, BTOK_PLOT)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "PLOT", ln);
        TypeInfo *ubyte = p->cs->symbol_table->basic_types[TYPE_ubyte];
        AstNode *attr_list = parse_gfx_attributes(p);
        AstNode *x = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' after PLOT x");
        AstNode *y = parse_expression(p, PREC_NONE + 1);
        x = make_typecast(p->cs, ubyte, x, ln);
        y = make_typecast(p->cs, ubyte, y, ln);
        if (x) ast_add_child(p->cs, s, x);
        if (y) ast_add_child(p->cs, s, y);
        if (attr_list) ast_add_child(p->cs, s, attr_list);
        return s;
    }

    /* DRAW [attr_list] expr COMMA expr [COMMA expr]
     * zxbparser.py p_statement_draw / _draw_attr / _draw3 / _draw3_attr
     * (:970-1012):
     *   DRAW  -> make_sentence(ln,"DRAW", make_typecast(integer,x,ln),
     *                                     make_typecast(integer,y,ln)
     *                                     [, attr_list])
     *   DRAW3 -> make_sentence(ln,"DRAW3", make_typecast(integer,x,ln),
     *                                      make_typecast(integer,y,ln),
     *                                      make_typecast(float_,z,ln)
     *                                      [, attr_list])
     * (a 3rd expr promotes DRAW -> DRAW3). attr_list is LAST. */
    if (match(p, BTOK_DRAW)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "DRAW", ln);
        TypeInfo *integer = p->cs->symbol_table->basic_types[TYPE_integer];
        TypeInfo *float_  = p->cs->symbol_table->basic_types[TYPE_float];
        AstNode *attr_list = parse_gfx_attributes(p);
        AstNode *x = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' after DRAW x");
        AstNode *y = parse_expression(p, PREC_NONE + 1);
        AstNode *z = NULL;
        bool is_draw3 = false;
        if (match(p, BTOK_COMMA)) {
            z = parse_expression(p, PREC_NONE + 1);
            is_draw3 = true;
            s->u.sentence.kind = arena_strdup(&p->cs->arena, "DRAW3");
        }
        x = make_typecast(p->cs, integer, x, ln);
        y = make_typecast(p->cs, integer, y, ln);
        if (x) ast_add_child(p->cs, s, x);
        if (y) ast_add_child(p->cs, s, y);
        if (is_draw3) {
            z = make_typecast(p->cs, float_, z, ln);
            if (z) ast_add_child(p->cs, s, z);
        }
        if (attr_list) ast_add_child(p->cs, s, attr_list);
        return s;
    }

    /* CIRCLE [attr_list] expr COMMA expr COMMA expr
     * zxbparser.py p_statement_circle / _circle_attr (:1014-1032):
     *   make_sentence(ln,"CIRCLE", make_typecast(byte_,x,ln),
     *                              make_typecast(byte_,y,ln),
     *                              make_typecast(byte_,r,ln) [, attr_list])
     * NB: CIRCLE casts ALL THREE positionals to TYPE.byte_ (signed). */
    if (match(p, BTOK_CIRCLE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "CIRCLE", ln);
        TypeInfo *byte_ = p->cs->symbol_table->basic_types[TYPE_byte];
        AstNode *attr_list = parse_gfx_attributes(p);
        AstNode *x = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' in CIRCLE");
        AstNode *y = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' in CIRCLE");
        AstNode *r = parse_expression(p, PREC_NONE + 1);
        x = make_typecast(p->cs, byte_, x, ln);
        y = make_typecast(p->cs, byte_, y, ln);
        r = make_typecast(p->cs, byte_, r, ln);
        if (x) ast_add_child(p->cs, s, x);
        if (y) ast_add_child(p->cs, s, y);
        if (r) ast_add_child(p->cs, s, r);
        if (attr_list) ast_add_child(p->cs, s, attr_list);
        return s;
    }

    /* FUNCTION / SUB declarations */
    if (check(p, BTOK_FUNCTION)) {
        return parse_sub_or_func_decl(p, true, false);
    }
    if (check(p, BTOK_SUB)) {
        return parse_sub_or_func_decl(p, false, false);
    }

    /* DECLARE (forward declaration) */
    if (match(p, BTOK_DECLARE)) {
        bool is_func = check(p, BTOK_FUNCTION);
        if (!is_func && !check(p, BTOK_SUB)) {
            parser_error(p, "Expected FUNCTION or SUB after DECLARE");
            return NULL;
        }
        return parse_sub_or_func_decl(p, is_func, true);
    }

    /* EXIT DO/FOR/WHILE */
    if (match(p, BTOK_EXIT)) {
        int ln = p->previous.lineno;
        const char *loop_kw = NULL;
        const char *q = NULL;
        LoopType lt = LOOP_DO;
        if (match(p, BTOK_DO)) { loop_kw = "EXIT_DO"; q = "DO"; lt = LOOP_DO; }
        else if (match(p, BTOK_FOR)) { loop_kw = "EXIT_FOR"; q = "FOR"; lt = LOOP_FOR; }
        else if (match(p, BTOK_WHILE)) { loop_kw = "EXIT_WHILE"; q = "WHILE"; lt = LOOP_WHILE; }
        else {
            parser_error(p, "Expected DO, FOR, or WHILE after EXIT");
            return NULL;
        }
        AstNode *node = make_sentence_node(p, loop_kw, ln);
        /* p_exit (src/zxbc/zxbparser.py:1950-1960): the sentence node is
         * built first, then the LOOPS stack is scanned for a loop of the
         * named type; if none is found Python reports a regular error
         * (exit 1) "Syntax Error: EXIT <q> out of loop".  Without this
         * the C let the EXIT reach the translator, whose loop_exit_label
         * fell through to the internal `InvalidLoopError` stub and a
         * non-1 exit (CLAUDE.md r8: never leak an internal stub). */
        bool in_loop = false;
        for (int i = 0; i < p->cs->loop_stack.len; i++)
            if (p->cs->loop_stack.data[i].type == lt) { in_loop = true; break; }
        if (!in_loop)
            zxbc_error(p->cs, ln, "Syntax Error: EXIT %s out of loop", q);
        return node;
    }

    /* CONTINUE DO/FOR/WHILE */
    if (match(p, BTOK_CONTINUE)) {
        int ln = p->previous.lineno;
        const char *loop_kw = NULL;
        const char *q = NULL;
        LoopType lt = LOOP_DO;
        if (match(p, BTOK_DO)) { loop_kw = "CONTINUE_DO"; q = "DO"; lt = LOOP_DO; }
        else if (match(p, BTOK_FOR)) { loop_kw = "CONTINUE_FOR"; q = "FOR"; lt = LOOP_FOR; }
        else if (match(p, BTOK_WHILE)) { loop_kw = "CONTINUE_WHILE"; q = "WHILE"; lt = LOOP_WHILE; }
        else {
            parser_error(p, "Expected DO, FOR, or WHILE after CONTINUE");
            return NULL;
        }
        AstNode *node = make_sentence_node(p, loop_kw, ln);
        /* p_continue (src/zxbc/zxbparser.py:1963-1975): same LOOPS-stack
         * gate as p_exit -> "Syntax Error: CONTINUE <q> out of loop". */
        bool in_loop = false;
        for (int i = 0; i < p->cs->loop_stack.len; i++)
            if (p->cs->loop_stack.data[i].type == lt) { in_loop = true; break; }
        if (!in_loop)
            zxbc_error(p->cs, ln, "Syntax Error: CONTINUE %s out of loop", q);
        return node;
    }

    /* DATA — faithful p_data (src/zxbc/zxbparser.py:1732-1772).
     *
     * Python's p_data NEVER sets p[0] on the success or error paths
     * (PLY defaults unassigned p[0] to None), so a DATA statement
     * contributes NO node to the program AST — its entire effect is the
     * gl.DATAS / gl.DATA_FUNCTIONS / gl.DATA_LABELS / gl.DATA_PTR_CURRENT
     * side-effects. The C port returns NULL so parser_parse's
     * `if (stmt && stmt->tag != AST_NOP)` drops it identically (Python
     * ast-dump for a DATA program carries no DATA node — verified). The
     * bare "DATA" sentence is still BUILT (its children are the parsed
     * value exprs) so the static/funcptr split below reads d.value the
     * same way Python reads ARGUMENT.value; the sentence itself is
     * discarded, exactly like Python's p[2].children consumption. */
    if (match(p, BTOK_DATA)) {
        int ln = p->previous.lineno;

        /* :1734  label_ = make_label(gl.DATA_PTR_CURRENT, lineno).
         * Declares the per-DATA label (id == the current data-ptr
         * string) AND records data_labels[id]=data_ptr_current. */
        char *label_name =
            p->cs->data_ptr_current ? p->cs->data_ptr_current
                                    : current_data_label(p->cs);
        AstNode *label_entry =
            symboltable_access_label(p->cs->symbol_table, p->cs,
                                     label_name, ln);
        if (label_entry)
            hashmap_set(&p->cs->data_labels, label_name,
                        p->cs->data_ptr_current ? p->cs->data_ptr_current
                                                : "");

        AstNode *s = make_sentence_node(p, "DATA", ln);
        do {
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            if (expr) ast_add_child(p->cs, s, expr);
        } while (match(p, BTOK_COMMA));

        /* :1738-1740  if p[2] is None: p[0]=None; return  (no items). */
        if (s->child_count == 0)
            return NULL;

        /* :1742-1745  if gl.FUNCTION_LEVEL: error; p[0]=None; return.
         * (no DATAS append inside a function/sub). */
        if (p->cs->function_level.len > 0) {
            zxbc_error(p->cs, ln,
                       "DATA not allowed within Functions nor Subs");
            return NULL;
        }

        data_build(p, s->children, s->child_count, label_name, label_entry, ln);
        return NULL;
    }

    /* READ */
    if (match(p, BTOK_READ)) {
        int ln = p->previous.lineno;
        AstNode *targets[256];
        int nt = 0;
        do {
            AstNode *target = parse_expression(p, PREC_NONE + 1);
            if (nt < (int)(sizeof(targets) / sizeof(targets[0])))
                targets[nt++] = target;  /* may be NULL */
        } while (match(p, BTOK_COMMA));
        return read_build(p, targets, nt, ln);
    }

    /* RESTORE [label] */
    if (match(p, BTOK_RESTORE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "RESTORE", ln);
        if (check(p, BTOK_ID) || check(p, BTOK_LABEL) || check(p, BTOK_NUMBER)) {
            advance(p);
            AstNode *lbl = ast_new(p->cs, AST_ID, p->previous.lineno);
            const char *label = p->previous.sval;
            char buf[32];
            if (!label) {
                snprintf(buf, sizeof(buf), "%d", (int)p->previous.numval);
                label = buf;
            }
            lbl->u.id.name = arena_strdup(&p->cs->arena, label);
            lbl->u.id.class_ = CLASS_label;
            ast_add_child(p->cs, s, lbl);
        }
        return s;
    }

    /* ON expr GOTO/GOSUB label, label, ... */
    if (match(p, BTOK_ON)) {
        int ln = p->previous.lineno;
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        const char *kind = "ON_GOTO";
        if (match(p, BTOK_GOSUB)) kind = "ON_GOSUB";
        else consume(p, BTOK_GOTO, "Expected GOTO or GOSUB after ON expr");
        AstNode *s = make_sentence_node(p, kind, ln);
        if (expr) ast_add_child(p->cs, s, expr);
        do {
            if (!check(p, BTOK_ID) && !check(p, BTOK_LABEL) && !check(p, BTOK_NUMBER)) break;
            advance(p);
            const char *label = p->previous.sval;
            char buf[32];
            if (!label) { snprintf(buf, sizeof(buf), "%d", (int)p->previous.numval); label = buf; }
            AstNode *lbl = ast_new(p->cs, AST_ID, ln);
            lbl->u.id.name = arena_strdup(&p->cs->arena, label);
            lbl->u.id.class_ = CLASS_label;
            ast_add_child(p->cs, s, lbl);
        } while (match(p, BTOK_COMMA));
        return s;
    }

    /* SAVE / LOAD / VERIFY — faithful port of zxbparser.py:
     *   p_save_code (~2233), p_save_data (~2257),
     *   p_load_or_verify (~2289), p_load_code (~2296),
     *   p_load_data (~2326).
     *
     * Each rule builds make_sentence(lineno, p[1], expr, start, length)
     * with EXACTLY 3 children: the string expr, a `start` (uinteger),
     * a `length` (uinteger). p[1] is the literal keyword text
     * ("SAVE"/"LOAD"/"VERIFY") which becomes the sentence kind. */
    if (match(p, BTOK_SAVE) || match(p, BTOK_LOAD) || match(p, BTOK_VERIFY)) {
        BTokenType kw = p->previous.type;
        const char *kind = (kw == BTOK_SAVE)   ? "SAVE"
                         : (kw == BTOK_LOAD)   ? "LOAD"
                                               : "VERIFY";
        int ln = p->previous.lineno;
        TypeInfo *uint_t = p->cs->symbol_table->basic_types[TYPE_uinteger];

        AstNode *expr = parse_expression(p, PREC_NONE + 1);

        /* if expr.type_ != TYPE.string: syntax_error_expected_string */
        if (expr && expr->type_ &&
            !type_equal(expr->type_,
                        p->cs->symbol_table->basic_types[TYPE_string])) {
            err_expected_string(p->cs, ln,
                                expr->type_->name ? expr->type_->name : "");
        }

        AstNode *start = NULL;
        AstNode *length = NULL;

        if (match(p, BTOK_DATA)) {
            /* p_save_data / p_load_data:
             *   {SAVE|LOAD|VERIFY} expr DATA
             *   {SAVE|LOAD|VERIFY} expr DATA ID
             *   {SAVE|LOAD|VERIFY} expr DATA ID LP RP
             * len(p)==4 is the no-id form (DATA with no ID). */
            if (check(p, BTOK_ID) || check(p, BTOK_ARRAY_ID)) {
                advance(p);
                const char *idname = p->previous.sval;
                int id_ln = p->previous.lineno;
                /* optional "( )" */
                if (match(p, BTOK_LP))
                    consume(p, BTOK_RP, "Expected ')' after '('");

                AstNode *entry = symboltable_access_id(p->cs->symbol_table,
                                                       p->cs, idname, id_ln,
                                                       NULL, CLASS_var);
                if (entry == NULL) {
                    /* p[0] = None; return — drop the statement */
                    return make_sentence_node(p, kind, ln);
                }
                entry->u.id.accessed = true;  /* mark_entry_as_accessed */

                start = make_unary_node(p->cs, "ADDRESS", entry, id_ln);
                if (start) start->type_ = uint_t;

                if (entry->u.id.class_ == CLASS_array) {
                    /* length = make_number(entry.memsize) */
                    int memsize = (entry->type_ ? type_size(entry->type_) : 0);
                    if (entry->u.id.arr_boundlist) {
                        AstNode *bl = entry->u.id.arr_boundlist;
                        for (int bi = 0; bi < bl->child_count; bi++) {
                            AstNode *b = bl->children[bi];
                            if (b && b->child_count >= 2 &&
                                b->children[0]->tag == AST_NUMBER &&
                                b->children[1]->tag == AST_NUMBER) {
                                int lo = (int)b->children[0]->u.number.value;
                                int hi = (int)b->children[1]->u.number.value;
                                memsize *= (hi - lo + 1);
                            }
                        }
                    }
                    length = make_number(p, memsize, id_ln, NULL);
                } else {
                    /* length = make_number(entry.type_.size) */
                    length = make_number(p,
                        entry->type_ ? type_size(entry->type_) : 0,
                        id_ln, NULL);
                }
            } else {
                /* No-id form (len(p)==4): the gl.ZXBASIC_USER_DATA /
                 * ZXBASIC_USER_DATA_LEN root-global labels.
                 * Python: SYMBOL_TABLE.access_label(gl.ZXBASIC_USER_DATA,
                 *   p.lineno(3), SYMBOL_TABLE.global_scope) — declare_label
                 * for a name starting with NAMESPACE_SEPARATOR ('.') keeps
                 * entry.mangled = id_ verbatim, sets type_ = PTR_TYPE
                 * (== uinteger) and declared = True (symboltable.py:464 /
                 * declare_label). LabelRef.t == parent.mangled, so the
                 * operand's .t is the literal label name; the ADDRESS
                 * scalar-global path then emits `ld hl, <label>`.
                 *
                 * symboltable_access_label declares the label (global)
                 * with mangled="_<name>"/declared=false; patch the entry
                 * to the '.'-prefix declare_label outcome so it is a
                 * resolved, declared label (check_pending_labels) and the
                 * translator reads the verbatim mangled name. */
                int d_ln = p->previous.lineno;  /* p.lineno(3) == DATA */

                AstNode *lbl_s = symboltable_access_label(p->cs->symbol_table,
                    p->cs, ".core.ZXBASIC_USER_DATA", d_ln);
                if (lbl_s) {
                    char *m = arena_strdup(&p->cs->arena,
                        ".core.ZXBASIC_USER_DATA");
                    lbl_s->u.id.mangled = m;
                    /* LabelRef.t == parent.mangled (labelref.py:34). The
                     * ADDRESS scalar-global path reads operand->t without
                     * visiting it, so stamp .t here (== mangled). */
                    lbl_s->t = m;
                    lbl_s->u.id.class_ = CLASS_label;
                    lbl_s->u.id.scope = SCOPE_global;
                    lbl_s->u.id.declared = true;
                    lbl_s->u.id.accessed = true;
                    lbl_s->type_ = uint_t;
                }
                start = make_unary_node(p->cs, "ADDRESS", lbl_s, d_ln);
                if (start) start->type_ = uint_t;

                AstNode *lbl_l = symboltable_access_label(p->cs->symbol_table,
                    p->cs, ".core.ZXBASIC_USER_DATA_LEN", d_ln);
                if (lbl_l) {
                    char *ml = arena_strdup(&p->cs->arena,
                        ".core.ZXBASIC_USER_DATA_LEN");
                    lbl_l->u.id.mangled = ml;
                    lbl_l->t = ml;  /* LabelRef.t == parent.mangled */
                    lbl_l->u.id.class_ = CLASS_label;
                    lbl_l->u.id.scope = SCOPE_global;
                    lbl_l->u.id.declared = true;
                    lbl_l->u.id.accessed = true;
                    lbl_l->type_ = uint_t;
                }
                length = make_unary_node(p->cs, "ADDRESS", lbl_l, d_ln);
                if (length) length->type_ = uint_t;
            }
        } else if (match(p, BTOK_CODE)) {
            /* CODE form:
             *   SAVE expr CODE expr COMMA expr
             *   {LOAD|VERIFY} expr CODE
             *   {LOAD|VERIFY} expr CODE expr
             *   {LOAD|VERIFY} expr CODE expr COMMA expr
             * SAVE requires `CODE expr COMMA expr`; LOAD/VERIFY allow the
             * bare-CODE (start=0,length=0) and one/two-expr variants. */
            int code_ln = p->previous.lineno;
            if (check(p, BTOK_NEWLINE) || check(p, BTOK_EOF) ||
                check(p, BTOK_CO)) {
                /* bare CODE — only valid for LOAD/VERIFY (p_load_code
                 * "load_or_verify expr ID" with ID.upper()=="CODE":
                 * start=0,length=0). */
                start = make_number(p, 0, code_ln, NULL);
                length = make_number(p, 0, code_ln, NULL);
            } else {
                AstNode *e1 = parse_expression(p, PREC_NONE + 1);
                start = make_typecast(p->cs, uint_t, e1, code_ln);
                if (match(p, BTOK_COMMA)) {
                    int comma_ln = p->previous.lineno;
                    AstNode *e2 = parse_expression(p, PREC_NONE + 1);
                    length = make_typecast(p->cs, uint_t, e2, comma_ln);
                } else {
                    /* {LOAD|VERIFY} expr CODE expr -> length = 0 */
                    length = make_number(p, 0, code_ln, NULL);
                }
            }
        } else if (check(p, BTOK_ID) || check(p, BTOK_ARRAY_ID)) {
            /* {SAVE expr ID|ARRAY_ID} / {LOAD|VERIFY expr ID}:
             * the ID text upper() must be SCREEN / SCREEN$
             * (LOAD/VERIFY also accept "CODE"). */
            advance(p);
            const char *idtxt = p->previous.sval ? p->previous.sval : "";
            int id_ln = p->previous.lineno;
            int is_screen = (ci_eq(idtxt, "SCREEN") ||
                             ci_eq(idtxt, "SCREEN$"));
            int is_code = ci_eq(idtxt, "CODE");

            if (kw == BTOK_SAVE) {
                if (!is_screen) {
                    zxbc_error(p->cs, id_ln,
                        "Unexpected \"%s\" ID. Expected \"SCREEN$\" instead",
                        idtxt);
                    return NULL;
                }
                start = make_number(p, 16384, ln, NULL);
                length = make_number(p, 6912, ln, NULL);
            } else {
                /* LOAD / VERIFY */
                if (!is_screen && !is_code) {
                    zxbc_error(p->cs, id_ln,
                        "Unexpected \"%s\" ID. Expected \"SCREEN$\" instead",
                        idtxt);
                    return NULL;
                }
                if (is_code) {
                    start = make_number(p, 0, id_ln, NULL);
                    length = make_number(p, 0, id_ln, NULL);
                } else {
                    start = make_number(p, 16384, id_ln, NULL);
                    length = make_number(p, 6912, id_ln, NULL);
                }
            }
        } else {
            parser_error(p, "Expected CODE, DATA or SCREEN$ after "
                            "SAVE/LOAD/VERIFY string");
            return NULL;
        }

        AstNode *s = make_sentence_node(p, kind, ln);
        if (expr)   ast_add_child(p->cs, s, expr);
        if (start)  ast_add_child(p->cs, s, start);
        if (length) ast_add_child(p->cs, s, length);
        return s;
    }

    /* Standalone INK/PAPER/BRIGHT/FLASH/OVER/INVERSE/BOLD/ITALIC as statements */
    {
        const char *attr_name = NULL;
        if (match(p, BTOK_INK))     attr_name = "INK";
        else if (match(p, BTOK_PAPER))   attr_name = "PAPER";
        else if (match(p, BTOK_BRIGHT))  attr_name = "BRIGHT";
        else if (match(p, BTOK_FLASH))   attr_name = "FLASH";
        else if (match(p, BTOK_OVER))    attr_name = "OVER";
        else if (match(p, BTOK_INVERSE)) attr_name = "INVERSE";
        else if (match(p, BTOK_BOLD))    attr_name = "BOLD";
        else if (match(p, BTOK_ITALIC))  attr_name = "ITALIC";
        if (attr_name) {
            /* p_simple_instruction (zxbparser.py:2218-2228):
             *   make_sentence(ln, p[1], make_typecast(TYPE.ubyte,p[2],ln)) */
            int ln = p->previous.lineno;
            AstNode *s = make_sentence_node(p, attr_name, ln);
            AstNode *val = parse_expression(p, PREC_NONE + 1);
            val = make_typecast(p->cs,
                                p->cs->symbol_table->basic_types[TYPE_ubyte],
                                val, ln);
            if (val) ast_add_child(p->cs, s, val);
            return s;
        }
    }

    /* Preprocessor directives */
    /* #define and other preprocessor lines that should have been handled by zxbpp */
    if (match(p, BTOK__LINE)) {
        /* Ignore #line directives — already handled by lexer */
        while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF)) advance(p);
        return make_nop(p);
    }
    if (match(p, BTOK__INIT)) {
        if (check(p, BTOK_ID)) {
            advance(p);
            vec_push(p->cs->inits, arena_strdup(&p->cs->arena, p->previous.sval));
        }
        return make_nop(p);
    }
    if (match(p, BTOK__REQUIRE)) {
        /* zxbparser.py:3232-3234 p_preproc_line_require:
         *     """preproc_line : _REQUIRE STRING"""
         *     arch.target.backend.REQUIRES.add(p[2])
         * p[2] is the quote-stripped module name (zxblex.py:511-513
         * t_preproc_STRING strips the surrounding quotes); the C preproc
         * STRC token already arrives quote-stripped (lexer.c lex_preproc).
         * Python's `common.REQUIRES` is the live module global read by
         * emit()/emit_prologue; the C Backend is built later in codegen,
         * so stage the name on cs->requires (seeded into b->requires_
         * after backend_init in codegen_emit). */
        if (check(p, BTOK_STRC)) {
            advance(p);
            vec_push(p->cs->requires,
                     arena_strdup(&p->cs->arena, p->previous.sval));
        }
        return make_nop(p);
    }
    if (match(p, BTOK__PRAGMA)) {
        /* #pragma NAME = VALUE — set compiler option.
         *
         * Faithful port of zxbparser.py:3237-3245 p_preproc_line_pragma_option:
         *     try: setattr(OPTIONS, p[2], p[4])
         *     except UndefinedOptionError:
         *         errmsg.warning_ignoring_unknown_pragma(p.lineno(2), p[2])
         *
         * Semantics established by reading the Python (NOT guessed):
         *  - The value token is ALWAYS a Python `str`: zxblex.py:505
         *    t_preproc_INTEGER does NOT int()-convert (regex [0-9]+, raw
         *    str), t_preproc_STRING strips quotes -> str, the ID arm is a
         *    str.  So p[4] is a str in every grammar alternative.
         *  - setattr -> Options.__setattr__ -> __setitem__ (options.py
         *    :216-239): if NAME is not a registered option key ->
         *    UndefinedOptionError -> caught -> W300 warning.  NAME match is
         *    the dict-key match, i.e. CASE-SENSITIVE (verified empirically:
         *    `#pragma EXPLICIT=true` and `#pragma Org=0` both emit W300).
         *  - Otherwise Option.value setter (options.py:114-144) coerces the
         *    str by the option's registered type_:
         *      * bool option: str -> dict {"false":F,"true":T,"off":F,
         *        "on":T,"-":F,"+":T,"no":F,"yes":T}[value.lower()].  A
         *        KeyError (e.g. value "0"/"1") is caught, value stays str,
         *        then `not isinstance(value,bool)` -> InvalidValueError
         *        (UNCAUGHT in p_preproc_line_pragma_option -> Python exits 1
         *        with a traceback; verified: `#pragma explicit=0`).
         *      * int option: int(value).  ValueError (non-numeric) caught,
         *        value stays str -> InvalidValueError (uncaught, exit 1).
         *        The INTEGER token regex is [0-9]+ so only decimal digits.
         *      * str option: str(value) == value.
         *  - The registered pragma-settable option set + types is the
         *    Options registry: src/api/config.py:194-251 (init) plus
         *    src/arch/z80/backend/main.py:621-633 (org/heap_size/
         *    heap_address/heap_*_label/headerless, ADD_IF_NOT_DEFINED).
         *    Names below use the EXACT Python-registered option names.
         *
         * InvalidValueError (a known option given a value its registered
         * type_ rejects) is uncaught in Python -> process exits 1 with a
         * Python traceback.  A traceback cannot be byte-reproduced in C and
         * is not the fidelity target (asm / diagnostics / line numbers are);
         * the observable contract for that path is the non-zero exit.  No
         * owned fixture exercises it (verified: no `explicit=0`-style
         * fixture), so emitting a clean parse error here (exit 1, like
         * Python) is FALSE_POS-safe by construction and matches Python's
         * exit behaviour for the spec's `explicit=0` probe.
         *
         * #pragma push(NAME) / #pragma pop(NAME) keep their prior
         * behaviour (handled by the trailing line-skip), unchanged. */
        /* `#pragma push(NAME)` / `pop(NAME)` lex `push`/`pop` as
         * BTOK__PUSH / BTOK__POP (lexer.c:152-154), never BTOK_ID, so the
         * BTOK_ID gate selects only the NAME=VALUE form; push/pop fall to
         * the trailing line-skip exactly as before. */
        if (check(p, BTOK_ID)) {
            const char *opt_name = p->current.sval;
            int name_lineno = p->current.lineno;
            advance(p);            /* consume NAME (ID) */
            if (match(p, BTOK_EQ)) {
            /* The value: INTEGER -> BTOK_NUMBER (sval = raw digits),
             * STRING -> BTOK_STRC (sval), ID -> BTOK_ID (sval).  In every
             * case the Python value is a str; mirror with the token's text. */
            const char *raw = NULL;
            if (check(p, BTOK_NUMBER) || check(p, BTOK_STRC) || check(p, BTOK_ID)) {
                raw = p->current.sval;   /* lexer sets sval for all three (lexer.c:778/789/801) */
                advance(p);
            }

            if (raw != NULL)
                apply_pragma_option(p, opt_name, name_lineno, raw);
            }
        }
        /* Skip any remaining tokens on this line (also covers
         * #pragma push(NAME) / #pragma pop(NAME), unchanged). */
        while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF)) advance(p);
        return make_nop(p);
    }

    /* ID or ARRAY_ID at start — either assignment or sub call */
    if (check(p, BTOK_ID) || check(p, BTOK_ARRAY_ID)) {
        advance(p);
        const char *name = p->previous.sval;
        int ln = p->previous.lineno;
        bool start_array_id = (p->previous.type == BTOK_ARRAY_ID);
        bool was_let = p->cs->let_assignment;
        p->cs->let_assignment = false;

        /* Array element assignment: ID(index) = expr or ID(i)(j TO k) = expr */
        if (check(p, BTOK_LP)) {
            AstNode *call_node = parse_call_or_array(p, name, ln, false, false);
            /* Handle chained postfix: a$(b)(1 TO 5). WRITE/lvalue context
             * (expr_context=false): keep the ARRAYACCESS-over-ARRAYACCESS /
             * legacy STRSLICE shape the assignment dispatcher below expects
             * (parser.c:3570-3616) and that the p_let_arr_substr* write
             * productions (zxbparser.py:2733-2815) lower from. */
            call_node = parse_postfix(p, call_node, false);

            /* Check if followed by = (assignment to array element or func return) */
            if (match(p, BTOK_EQ) || was_let) {
                if (p->previous.type != BTOK_EQ) consume(p, BTOK_EQ, "Expected '=' in assignment");
                int eq_lineno = p->previous.lineno;
                AstNode *expr = parse_expression(p, PREC_NONE + 1);
                /* String-slice write targets. The C builds an AST_STRSLICE
                 * lvalue for both Python productions; they diverge by base
                 * shape and emit DIFFERENT diagnostics, so split on it:
                 *
                 *  - base is an AST_ID  -> scalar p_str_assign
                 *    (`s(i TO j) = rhs`, zxbparser.py:1308-1335): the RHS
                 *    must be a string, else syntax_error_expected_string
                 *    (errmsg.py:201) at p.lineno(EQ). p_str_assign does NOT
                 *    check the TARGET type (a non-string scalar target is
                 *    accepted — err_letsubstr_lhs_nonstring), and it does
                 *    NOT abort (the node is still built) -> single error.
                 *
                 *  - base is an AST_ARRAYACCESS -> make_array_substr_assign
                 *    (`a(i)(j TO k) = rhs`, zxbparser.py:336-338): the array
                 *    element type must be string, else "Array '%s' is not of
                 *    type String" at the ARRAY_ID line (p.lineno(i)); this
                 *    check fires BEFORE the RHS typecast, so the RHS-string
                 *    diagnostic is NOT also emitted (let_array_substr4). */
                if (call_node && call_node->tag == AST_STRSLICE &&
                    call_node->child_count > 0) {
                    AstNode *base = call_node->children[0];
                    if (base && base->tag == AST_ID) {
                        if (expr && !type_is_string(expr->type_)) {
                            const TypeInfo *rft = (expr->type_ && expr->type_->final_type)
                                                      ? expr->type_->final_type : expr->type_;
                            const char *rtn = (rft && rft->tag == AST_BASICTYPE)
                                                  ? basictype_to_string(rft->basic_type) : "unknown";
                            err_expected_string(p->cs, eq_lineno, rtn);
                        }
                    } else if (base && base->tag == AST_ARRAYACCESS &&
                               base->child_count > 0) {
                        AstNode *arr_id = base->children[0];
                        if (arr_id && !type_is_string(arr_id->type_)) {
                            const char *anm = (arr_id->tag == AST_ID &&
                                               arr_id->u.id.name)
                                                  ? arr_id->u.id.name : "";
                            zxbc_error(p->cs, ln,
                                       "Array '%s' is not of type String", anm);
                        }
                    }
                } else if (call_node && call_node->tag == AST_ARRAYACCESS &&
                           call_node->child_count > 0 && call_node->children[0] &&
                           call_node->children[0]->tag == AST_ARRAYACCESS) {
                    /* Chained single-index substr `a(i)(j) = rhs`
                     * (p_let_arr_substr_single, zxbparser.py:2746 ->
                     * make_array_substr_assign, :336): Python routes ANY
                     * chained-index assignment through the substr production,
                     * so the array element type must be string, else "Array
                     * '<a>' is not of type String" at the ARRAY_ID line. A
                     * string array's element type passes (valid substr); a
                     * non-string array is always rejected (let_array_substr6).
                     * The no-TO postfix builds ARRAYACCESS-over-ARRAYACCESS
                     * (parser.c parse_postfix), distinct from a comma
                     * multi-subscript `a(i,j)` (single ARRAYACCESS). */
                    AstNode *inner = call_node->children[0];
                    AstNode *arr_id = inner->child_count > 0
                                          ? inner->children[0] : NULL;
                    if (arr_id && !type_is_string(arr_id->type_)) {
                        const char *anm = (arr_id->tag == AST_ID &&
                                           arr_id->u.id.name)
                                              ? arr_id->u.id.name : "";
                        zxbc_error(p->cs, ln,
                                   "Array '%s' is not of type String", anm);
                    }
                }
                AstNode *s = make_sentence_node(p, "LETARRAY", ln);
                ast_add_child(p->cs, s, call_node);
                if (expr) ast_add_child(p->cs, s, expr);
                /* p_let_arr (zxbparser.py:1215-1216): for the LETARRAY
                 * write target, Python skips the general accessed-mark
                 * (handled by expr_context=true elsewhere for reads) but
                 * EXPLICITLY marks the array entry accessed when it has
                 * an AT-clause address — `if entry.addr is not None:
                 * mark_entry_as_accessed(entry)`. Without this, a write-
                 * only `DIM a(...) AS T AT <const>` is dropped at O>1 by
                 * visit_ARRAYDECL's DCE early-out, taking with it the
                 * `_a.__DATA__ EQU <addr>` / `_a:` descriptor / data-ptr
                 * table / `.LABEL.__LABELn:` bounds image AND the
                 * LET-ARRAY const-subscript store `ld (_a.__DATA__ + N),
                 * hl` that translator.c emits referencing them.
                 *
                 * The C analogue of `entry.addr` is
                 * `entry->u.id.addr_expr`. The array ID is child[0] of
                 * the ARRAYACCESS lvalue (call_node). mark_entry_as_
                 * accessed's FUNCTION_LEVEL guard (zxbparser.py:172) is
                 * a no-op here — the entry is a CLASS_array, never a
                 * FUNCTION token. */
                if (call_node && call_node->tag == AST_ARRAYACCESS &&
                    call_node->child_count > 0) {
                    AstNode *array_id = call_node->children[0];
                    if (array_id && array_id->tag == AST_ID &&
                        array_id->u.id.class_ == CLASS_array &&
                        array_id->u.id.addr_expr != NULL) {
                        array_id->u.id.accessed = true;
                    }
                }
                return s;
            }

            /* If followed by a binary operator, Python's statement-level
             * grammar does NOT build `call_result OP rest`.  A bare
             * `ID(...) OP ...` statement reduces via `statement : ID
             * arguments` (zxbparser.py:1069) — i.e. ID applied to a
             * SINGLE argument whose expr is the WHOLE `(...) OP rest`
             * (the `(...)` is just a parenthesised sub-expression, not an
             * arg list).  So `test(1) + test(2)` as a statement is
             * `test((1) + test(2))`, NOT `test(1) + test(2)`
             * (opt2_fastcall_sub).  Note the LET/expression context keeps
             * the binary-of-two-calls reading — only the discarded
             * statement form takes the greedy-argument path.
             *
             * Reproduce: when call_node is a single-argument FUNCCALL/CALL
             * and a binary operator follows, splice the trailing
             * expression onto the call's lone argument and re-parse the
             * infix from there. */
            if (get_precedence(p->current.type) > PREC_NONE) {
                AstNode *arglist = (call_node &&
                    (call_node->tag == AST_FUNCCALL ||
                     call_node->tag == AST_CALL) &&
                    call_node->child_count > 1)
                        ? call_node->children[1] : NULL;
                if (arglist && arglist->tag == AST_ARGLIST &&
                    arglist->child_count == 1 &&
                    arglist->children[0] &&
                    arglist->children[0]->tag == AST_ARGUMENT &&
                    arglist->children[0]->child_count == 1) {
                    AstNode *arg = arglist->children[0];
                    AstNode *inner = arg->children[0];
                    AstNode *ext = parse_infix(p, inner, PREC_NONE + 1);
                    arg->children[0] = ext;
                    if (ext) arg->type_ = ext->type_;
                } else {
                    /* Not the single-arg shape (e.g. parenless or
                     * multi-arg) — fall back to the binary-statement
                     * reading. */
                    call_node = parse_infix(p, call_node, PREC_NONE + 1);
                }
            }

            /* Sub call: ID(args) as statement */
            if (call_node && call_node->tag == AST_FUNCCALL) {
                call_node->tag = AST_CALL;
            }
            return call_node;
        }

        /* Whole-array copy — p_array_copy (zxbparser.py:1137-1179):
         *   statement : ARRAY_ID EQ ARRAY_ID | LET ARRAY_ID EQ ARRAY_ID
         * Both the lvalue and rvalue must be array names; build an
         * ARRAYCOPY sentence [larray, rarray]. Detected BEFORE the scalar
         * assignment so `gridcopy = grid` is not mis-parsed as a LET.
         * Peeked: start ARRAY_ID, '=', then a lone ARRAY_ID followed by a
         * statement terminator. */
        if (start_array_id && (check(p, BTOK_EQ) || was_let)) {
            int save_pos = p->lexer.pos;
            BToken save_cur = p->current;
            bool consumed_eq = false;
            if (check(p, BTOK_EQ)) { advance(p); consumed_eq = true; }
            if ((consumed_eq || was_let) && check(p, BTOK_ARRAY_ID)) {
                int rln = p->current.lineno;
                const char *rname =
                    arena_strdup(&p->cs->arena,
                                 p->current.sval ? p->current.sval : "");
                advance(p);   /* consume RHS ARRAY_ID */
                if (check(p, BTOK_NEWLINE) || check(p, BTOK_EOF) ||
                    check(p, BTOK_CO)) {
                    AstNode *larray = symboltable_access_id(
                        p->cs->symbol_table, p->cs, name, ln, NULL,
                        CLASS_array);
                    AstNode *rarray = symboltable_access_id(
                        p->cs->symbol_table, p->cs, rname, rln, NULL,
                        CLASS_array);
                    if (larray == NULL || rarray == NULL)
                        return make_nop(p);   /* p[0] = None */
                    if (larray->type_ && rarray->type_ &&
                        !type_equal(larray->type_, rarray->type_)) {
                        zxbc_error(p->cs, ln,
                            "Arrays must have the same element type");
                        return make_nop(p);
                    }
                    /* mark_entry_as_accessed(larray/rarray) */
                    larray->u.id.accessed = true;
                    rarray->u.id.accessed = true;
                    AstNode *s = make_sentence_node(p, "ARRAYCOPY", ln);
                    ast_add_child(p->cs, s, larray);
                    ast_add_child(p->cs, s, rarray);
                    return s;
                }
            }
            /* Not an ARRAY_ID = ARRAY_ID copy — restore & fall through to
             * the scalar-assignment path below. */
            p->current = save_cur;
            p->lexer.pos = save_pos;
        }

        /* Simple assignment: ID = expr */
        if (match(p, BTOK_EQ) || was_let) {
            if (p->previous.type != BTOK_EQ) {
                consume(p, BTOK_EQ, "Expected '=' in assignment");
            }
            /* Python p_lexpr (zxbparser.py:1119-1134) fires BEFORE the RHS
             * is parsed and calls `access_id(name, lineno)` with NO
             * default_type — implicitly declaring the lvalue when it
             * doesn't yet exist.  p_assignment (:1100) then re-calls
             * access_id with default_type=RHS.type_, but for an
             * already-existing entry the default_type is ignored.  Net
             * Python behaviour: a newly-auto-declared LET lvalue takes
             * the DEFAULT_IMPLICIT_TYPE (unknown), NOT the RHS type.  The
             * subsequent re-access updates the unknown type from RHS.type_
             * with the bool→ubyte coercion (symboltable.py:359-364 ===
             * compiler.c:1158-1169).  Without this pre-access, `LET c =
             * (a$ = b$)` would type c as `boolean` (rhs.type_), emitting
             * an unknown `storebool` IC and losing the assignment; and
             * `LET a = a + 1` would leave `a` unknown for the RHS read
             * (p_id_expr promotes unknown→DEFAULT_TYPE only when the
             * entry was already declared).  Mirror p_lexpr explicitly. */
            symboltable_access_id(p->cs->symbol_table, p->cs, name, ln,
                                  NULL, CLASS_unknown);
            /* Python p_assignment parses the RHS first, then resolves the
             * lvalue with default_type = RHS.type_ (zxbparser.py:1100) and
             * casts the RHS to the lvalue type (:1115). Parse-order and
             * the typecast both matter for fidelity. */
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            TypeInfo *rhs_type = expr ? expr->type_ : NULL;
            /* Resolve target via symbol table (triggers explicit mode check) */
            AstNode *var = symboltable_access_id(p->cs->symbol_table, p->cs,
                                                  name, ln, rhs_type, CLASS_var);
            if (var) {
                /* Assignment to a non-variable lvalue. Python emits ONE
                 * message for CONST/SUB/FUNCTION alike —
                 * syntax_error_cannot_assign_not_a_var (errmsg.py:283),
                 * "Cannot assign a value to '%s'. It's not a variable".
                 * C's SUB branch already used that text; CONST/FUNCTION
                 * diverged ("'%s' is a CONST/FUNCTION, not a VAR") — S1.2
                 * preserved the literals verbatim and deferred wording to
                 * Phase 3; this is that alignment (S3.1 CAT-4). */
                if (var->u.id.class_ == CLASS_const ||
                    var->u.id.class_ == CLASS_sub ||
                    var->u.id.class_ == CLASS_function) {
                    err_cannot_assign(p->cs, ln, name);
                }
                if (var->u.id.class_ == CLASS_unknown)
                    var->u.id.class_ = CLASS_var;
            } else {
                /* access_id returned NULL (explicit mode error already reported) */
                var = ast_new(p->cs, AST_ID, ln);
                var->u.id.name = arena_strdup(&p->cs->arena, name);
                var->u.id.class_ = CLASS_unknown;
            }
            /* Cast RHS to the lvalue type (make_typecast is a no-op when
             * types match or var->type_ is NULL — fallback path). */
            expr = make_typecast(p->cs, var->type_, expr, ln);
            AstNode *s = make_sentence_node(p, "LET", ln);
            ast_add_child(p->cs, s, var);
            if (expr) ast_add_child(p->cs, s, expr);
            return s;
        }

        /* Sub call WITHOUT parentheses, or a bare-ID label reference.
         * S5.7f — faithful port of Python p_statement_call
         * (zxbparser.py:1067-1081, "statement : ID arg_list | ID
         * arguments | ID"):
         *   bare ID (no args): entry = SYMBOL_TABLE.get_entry(p[1]);
         *     if entry is not None and entry.class_ in (label, unknown)
         *       -> make_label(p[1])         (a label reference)
         *     else
         *       -> make_sub_call(p[1], make_arg_list(None))
         *   ID args -> make_sub_call(p[1], p[2])
         * make_sub_call -> SymbolCALL.make_node (symbols/call.py:90-112):
         *   access_func(id_) (auto-declares CLASS_unknown if absent),
         *   AST child[0] = resolved entry, child[1] = ARGLIST, and
         *   gl.FUNCTION_CALLS.append(result) for the pending-call check.
         * The C lexer diverts label *definitions* (`xx:`) to BTOK_LABEL
         * (parser.c:935) so only bare-ID *uses* reach here; the genuine
         * ambiguity is bare-ID-use vs label-ref, disambiguated exactly
         * as Python via the get_entry class. The pre-resolution VAR/CONST
         * guard is kept verbatim (Python access_func rejects the same). */
        bool has_args = !check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) &&
                        !check(p, BTOK_CO);

        if (!has_args) {
            AstNode *e = symboltable_lookup(p->cs->symbol_table, name);
            if (e && (e->u.id.class_ == CLASS_label ||
                      e->u.id.class_ == CLASS_unknown)) {
                /* make_label(p[1], lineno): a label reference. Same
                 * statement shape the label-definition path emits
                 * (parser.c:978-983) so ast-dump stays identical.
                 *
                 * Python p_statement_call (zxbparser.py:1077) routes
                 * here for entry.class_ in (label, unknown) and calls
                 * make_label, which itself calls declare_label — i.e.
                 * the call is also a DEFINITION when no prior label
                 * declaration exists. Without that promotion the
                 * later check_pending_labels rejects this AST_ID
                 * (class still CLASS_unknown after parse) as an
                 * undeclared label (opt3_lcd6: `@overlay` creates a
                 * CLASS_unknown entry before `overlay:` at column 0).
                 * Mirror via access_label, which check_is_undeclared
                 * 's so a repeat-define still errors faithfully. */
                if (e->u.id.class_ == CLASS_unknown) {
                    AstNode *ln_e = symboltable_access_label(
                        p->cs->symbol_table, p->cs, name, ln);
                    if (ln_e && ln_e->u.id.class_ == CLASS_label) {
                        ln_e->u.id.declared = true;
                        ln_e->type_ =
                            p->cs->symbol_table->basic_types[TYPE_uinteger];
                        /* symboltable.access_label scope_owner capture
                         * (symboltable.py:621-623, mirrored at the BTOK_LABEL
                         * def-site parser.c:1487). The bare-ID label
                         * definition path (Python p_statement_call ->
                         * make_label -> access_label) takes this same
                         * capture: if `@label` (CLASS_unknown placeholder)
                         * was taken inside a SUB earlier, the label entry
                         * already carries accessed=true; capturing
                         * scope_owner here re-fires the cascade so the
                         * enclosing SUB(s) are kept alive (atlabel3:
                         * `Function test2 / b:` defines b inside test2 to
                         * keep test2.accessed). */
                        label_capture_scope_owner(p->cs, ln_e);
                    }
                } else if (e->u.id.class_ == CLASS_label && e->u.id.declared) {
                    /* Python declare_label (symboltable.py:592-603):
                     * check_is_undeclared (re-declaration) returns the
                     * existing entry with a syntax error. Mirror that
                     * here so a duplicate label-def (opt2_labelinfunc2:
                     * `label1:` at column 0 twice) is rejected — the
                     * BTOK_LABEL path at parser.c:1458 already does
                     * this for the lex-classified-LABEL case. */
                    zxbc_error(p->cs, ln,
                               "Label '%s' already used at %s:%d",
                               name, p->cs->current_file, e->lineno);
                }
                AstNode *ls = make_sentence_node(p, "LABEL", ln);
                AstNode *lid = ast_new(p->cs, AST_ID, ln);
                lid->u.id.name = arena_strdup(&p->cs->arena, name);
                lid->u.id.class_ = CLASS_label;
                ast_add_child(p->cs, ls, lid);
                /* S5.8d — make_label's DATA_LABELS write
                 * (zxbparser.py:457, the :1077 make_label caller).
                 * Strictly additive; no parse-surface effect. */
                hashmap_set(&p->cs->data_labels, name,
                            p->cs->data_ptr_current
                                ? p->cs->data_ptr_current : "");
                return ls;
            }
        }

        {
            AstNode *entry = symboltable_lookup(p->cs->symbol_table, name);
            if (entry && entry->u.id.class_ == CLASS_var) {
                zxbc_error(p->cs, ln, "'%s' is a VAR, not a FUNCTION", name);
            } else if (entry && entry->u.id.class_ == CLASS_const) {
                zxbc_error(p->cs, ln, "'%s' is a CONST, not a FUNCTION", name);
            }
        }
        /* make_sub_call -> SymbolCALL.make_node -> access_func(id_): the
         * resolved/auto-declared FUNCTION/SUB symbol-table entry IS
         * child[0] (call.py:33-67). Python's p_statement_call returns the
         * SymbolCALL AST node *directly* (no SENTENCE wrapper); the C
         * equivalent is an AST_FUNCCALL tag node retagged to AST_CALL,
         * exactly as the with-parens statement path does
         * (parse_call_or_array, parser.c:760-785, then the FUNCCALL->CALL
         * retag at parser.c:1623-1624). The tag-node form is load-bearing:
         * FunctionGraph marks callees accessed only for AST_CALL/
         * AST_FUNCCALL *tag* nodes (functiongraph.c:52,120-121); a
         * SENTENCE "CALL" is invisible to it, so the callee body would be
         * DCE'd and the call never folded to its __leave. */
        AstNode *s = ast_new(p->cs, AST_FUNCCALL, ln);
        AstNode *id_node =
            symboltable_access_func(p->cs->symbol_table, p->cs, name, ln, NULL);
        if (id_node) {
            /* S5.7a: do NOT mark accessed here (FunctionGraph does it,
             * scope-aware). n->type_ = entry.type_ exactly as the
             * with-parens FUNCCALL branch (parser.c:774). */
            s->type_ = id_node->type_;
        } else {
            /* access_func reported the error — placeholder, no register */
            id_node = ast_new(p->cs, AST_ID, ln);
            id_node->u.id.name = arena_strdup(&p->cs->arena, name);
            s->type_ = p->cs->default_type;
        }
        ast_add_child(p->cs, s, id_node);
        /* Python make_sub_call always passes a SymbolARGLIST (empty for a
         * bare ID via make_arg_list(None)); ARGLIST is therefore always
         * child[1] — never the 1-child shape the old C arm produced. */
        AstNode *arglist = ast_new(p->cs, AST_ARGLIST, ln);

        if (has_args) {
            do {
                /* Check for named argument: name := expr */
                if ((check(p, BTOK_ID) || is_name_token(p))) {
                    int save_pos = p->lexer.pos;
                    BToken save_cur = p->current;
                    const char *arg_name = get_name_token(p);
                    advance(p);
                    if (check(p, BTOK_WEQ)) {
                        advance(p); /* consume := */
                        AstNode *arg_expr = parse_expression(p, PREC_NONE + 1);
                        if (arg_expr) {
                            AstNode *arg = ast_new(p->cs, AST_ARGUMENT, arg_expr->lineno);
                            arg->u.argument.name = arena_strdup(&p->cs->arena, arg_name);
                            ast_add_child(p->cs, arg, arg_expr);
                            arg->type_ = arg_expr->type_;
                            ast_add_child(p->cs, arglist, arg);
                        }
                        continue;
                    }
                    /* Not named arg — restore */
                    p->current = save_cur;
                    p->lexer.pos = save_pos;
                }
                AstNode *arg_expr = parse_expression(p, PREC_NONE + 1);
                if (arg_expr) {
                    AstNode *arg = ast_new(p->cs, AST_ARGUMENT, arg_expr->lineno);
                    ast_add_child(p->cs, arg, arg_expr);
                    arg->type_ = arg_expr->type_;
                    ast_add_child(p->cs, arglist, arg);
                }
            } while (match(p, BTOK_COMMA));
        }
        /* ARGLIST is always child[1] (Python make_arg_list(None) for the
         * bare-ID form too) — never the old 1-child CALL shape. */
        ast_add_child(p->cs, s, arglist);

        /* gl.FUNCTION_CALLS.append(result) (symbols/call.py:109) — the
         * pending-call list check_pending_calls (compiler.c:1135) walks
         * to validate/back-patch the callee class, exactly as the
         * with-parens path registers the FUNCCALL node (parser.c:785).
         * Skip when the callee is a VAR/CONST mis-resolution — matches
         * Python make_node returning before FUNCTION_CALLS.append. */
        if (id_node && id_node->tag == AST_ID &&
            id_node->u.id.class_ != CLASS_var &&
            id_node->u.id.class_ != CLASS_const) {
            /* SymbolCALL.filename = gl.FILENAME at parse point
             * (symbols/call.py:42) — fname= for R3-R10. Set before the
             * FUNCCALL->CALL retag; the `call` union member is shared
             * by both tags (children-only nodes). */
            if (p->cs->current_file)
                s->u.call.filename =
                    arena_strdup(&p->cs->arena, p->cs->current_file);
            /* Python call.py:102 inline-vs-deferred dispatch (see
             * zxbc.h call.callee_inline). */
            s->u.call.callee_inline =
                (id_node->tag == AST_ID && id_node->u.id.params != NULL &&
                 !id_node->u.id.forwarded);
            vec_push(p->cs->function_calls, s);
            /* Python call.py:103 — inline branch runs check_call_arguments
             * at the parenless call site during parsing (deferred calls are
             * checked later by check_pending_calls, which skips
             * callee_inline calls). */
            inline_check_call_arguments(p, s);
        }

        /* Statement-level sub call: FUNCCALL -> CALL, exactly as the
         * with-parens statement path (parser.c:1623-1624). The tag node
         * (not a SENTENCE) is what FunctionGraph keys on. */
        if (s->tag == AST_FUNCCALL)
            s->tag = AST_CALL;
        return s;
    }

    /* Standalone NUMBER at start of statement — treat as label
     * (handles indented line numbers like "   0 REM ...") */
    if (check(p, BTOK_NUMBER)) {
        advance(p);
        double numval = p->previous.numval;
        int ln = p->previous.lineno;
        if (numval == (int)numval) {
            char label_buf[32];
            snprintf(label_buf, sizeof(label_buf), "%d", (int)numval);
            /* Register the label in symbol table (labels are always global) */
            AstNode *lbl_entry = symboltable_access_label(p->cs->symbol_table, p->cs,
                                                           label_buf, ln);
            if (lbl_entry) lbl_entry->u.id.declared = true;
            AstNode *lbl_sent = make_sentence_node(p, "LABEL", ln);
            AstNode *lbl_id = ast_new(p->cs, AST_ID, ln);
            lbl_id->u.id.name = arena_strdup(&p->cs->arena, label_buf);
            lbl_id->u.id.class_ = CLASS_label;
            ast_add_child(p->cs, lbl_sent, lbl_id);
            match(p, BTOK_CO);
            /* Parse trailing statement if any */
            if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF)) {
                AstNode *stmt = parse_statement(p);
                if (stmt) {
                    AstNode *block = make_block_node(p, ln);
                    ast_add_child(p->cs, block, lbl_sent);
                    ast_add_child(p->cs, block, stmt);
                    return block;
                }
            }
            return lbl_sent;
        }
        /* Non-integer number — error */
        parser_error(p, "Unexpected number");
        return NULL;
    }

    /* Empty statement */
    if (check(p, BTOK_NEWLINE) || check(p, BTOK_EOF) || check(p, BTOK_CO)) {
        return make_nop(p);
    }

    /* Error recovery: a token that cannot start any statement is PLY's
     * p_error "Syntax Error. Unexpected token '%s' <%s>" (zxbparser.py:3564)
     * — e.g. the residual `LOOP` after a DO header desync
     * (doloopuntilsplitted.bas:5). Emit the verbatim message rather than the
     * placeholder. Exit code is unchanged (still a parse error); only the
     * stderr text becomes faithful. */
    tok_unexpected_error(p, &p->current);
    synchronize(p);
    return NULL;
}

/* ----------------------------------------------------------------
 * IF statement
 * ---------------------------------------------------------------- */
static AstNode *parse_if_statement(Parser *p) {
    consume(p, BTOK_IF, "Expected IF");
    int lineno = p->previous.lineno;

    AstNode *condition = parse_expression(p, PREC_NONE + 1);
    match(p, BTOK_THEN); /* optional — Sinclair BASIC allows IF without THEN */

    /* p_if_then_part (src/zxbc/zxbparser.py:1515-1525): a constant IF
     * condition is always true/false -> W110 at the IF line
     * (p.lineno(1)).  bool(expr.value): nonzero -> True, zero -> False. */
    if (condition && check_is_number(condition)) {
        double cv = 0;
        zxbc_eval_to_num(condition, &cv);
        warn_condition_always(p->cs, lineno, cv != 0.0);
    }

    /* Single-line IF: IF cond THEN stmt [:stmt...] [ELSE stmt [:stmt...]]
     * Note: THEN: is still single-line (: is statement separator) */
    if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF)) {
        /* Skip leading colon(s) after THEN */
        while (match(p, BTOK_CO)) {}

        /* If after THEN and colons we hit a newline, this is either:
         * - Empty single-line IF (no END IF follows), or
         * - Multi-line IF (END IF follows on a subsequent line)
         * We handle both via the continuation check below. */
        AstNode *then_block = make_block_node(p, lineno);
        AstNode *else_block = NULL;
        bool ended = false;

        while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) &&
               !check(p, BTOK_ELSE) && !check(p, BTOK_ENDIF) && !ended) {
            /* Check for END IF on same line */
            if (check(p, BTOK_END)) {
                int save_pos = p->lexer.pos;
                BToken save_cur = p->current;
                advance(p);
                if (check(p, BTOK_IF)) {
                    advance(p);
                    ended = true;
                    break;
                }
                p->current = save_cur;
                p->lexer.pos = save_pos;
            }
            AstNode *stmt = parse_statement(p);
            if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, then_block, stmt);
            if (!match(p, BTOK_CO)) break;
        }
        if (!ended && match(p, BTOK_ENDIF)) ended = true;
        if (!ended && match(p, BTOK_ELSE)) {
            else_block = make_block_node(p, lineno);
            while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !ended) {
                if (check(p, BTOK_END)) {
                    int save_pos = p->lexer.pos;
                    BToken save_cur = p->current;
                    advance(p);
                    if (check(p, BTOK_IF)) { advance(p); ended = true; break; }
                    p->current = save_cur;
                    p->lexer.pos = save_pos;
                }
                if (match(p, BTOK_ENDIF)) { ended = true; break; }
                AstNode *stmt = parse_statement(p);
                if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, else_block, stmt);
                if (!match(p, BTOK_CO)) break;
            }
        }
        /* After single-line IF, check for END IF / ELSE on next line.
         * Only do this if the then-block is empty (i.e. THEN followed by newline
         * with no inline statements). If there were inline statements after THEN:,
         * this is a complete single-line IF — don't look for END IF. */
        if (!ended && then_block->child_count == 0 && !else_block) {
            skip_newlines(p);
            if (check(p, BTOK_ENDIF)) {
                advance(p);
                ended = true;
            } else if (check(p, BTOK_END)) {
                int sp = p->lexer.pos;
                BToken sc = p->current;
                advance(p);
                if (check(p, BTOK_IF)) {
                    advance(p);
                    ended = true;
                } else {
                    p->current = sc;
                    p->lexer.pos = sp;
                }
            } else if (check(p, BTOK_ELSE) || check(p, BTOK_ELSEIF)) {
                /* ELSE/ELSEIF on next line — treat as continuation */
                if (!else_block && match(p, BTOK_ELSE)) {
                    else_block = make_block_node(p, lineno);
                    skip_newlines(p);
                    while (!check(p, BTOK_EOF) && !check(p, BTOK_ENDIF) && !check(p, BTOK_END)) {
                        AstNode *stmt = parse_statement(p);
                        if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, else_block, stmt);
                        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
                    }
                    if (match(p, BTOK_ENDIF)) { ended = true; }
                    else if (check(p, BTOK_END)) {
                        advance(p);
                        if (check(p, BTOK_IF)) { advance(p); ended = true; }
                    }
                }
            }
        }

        AstNode *s = make_sentence_node(p, "IF", lineno);
        if (condition) ast_add_child(p->cs, s, condition);
        ast_add_child(p->cs, s, then_block);
        if (else_block) ast_add_child(p->cs, s, else_block);
        return s;
    }

    /* Multi-line IF: IF cond THEN \n ... [ELSEIF ... | ELSE ...] END IF */
    skip_newlines(p);

    AstNode *then_block = make_block_node(p, lineno);
    while (!check(p, BTOK_EOF) && !check(p, BTOK_ELSE) &&
           !check(p, BTOK_ELSEIF) && !check(p, BTOK_ENDIF) && !check(p, BTOK_END)) {
        AstNode *stmt = parse_statement(p);
        if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, then_block, stmt);
        /* Consume statement separator (newline or :) */
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
    }

    AstNode *else_block = NULL;

    /* ELSEIF chain */
    while (match(p, BTOK_ELSEIF)) {
        int elif_line = p->previous.lineno;
        AstNode *elif_cond = parse_expression(p, PREC_NONE + 1);
        match(p, BTOK_THEN); /* THEN is optional after ELSEIF */
        skip_newlines(p);

        AstNode *elif_body = make_block_node(p, elif_line);
        while (!check(p, BTOK_EOF) && !check(p, BTOK_ELSE) &&
               !check(p, BTOK_ELSEIF) && !check(p, BTOK_ENDIF) && !check(p, BTOK_END)) {
            AstNode *stmt = parse_statement(p);
            if (stmt) ast_add_child(p->cs, elif_body, stmt);
            while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
        }

        /* Wrap ELSEIF as nested IF-ELSE */
        AstNode *nested_if = make_sentence_node(p, "IF", elif_line);
        if (elif_cond) ast_add_child(p->cs, nested_if, elif_cond);
        ast_add_child(p->cs, nested_if, elif_body);

        if (else_block) {
            /* Chain: attach this nested IF as the ELSE (children[2]) of
             * the DEEPEST nested IF so far — NOT as another child of the
             * first one.  Walking children[2] mirrors the ELSE handler
             * below (3rd child == else branch).  Without the walk, a 3rd+
             * ELSEIF was appended as a 4th child of the first nested IF,
             * which visit_IF (n==3 gate) treats as a no-else IF and
             * silently drops every branch from the 2nd ELSEIF onward
             * (optspeed: 11 ELSEIFs collapsed to 1). */
            AstNode *last = else_block;
            while (last->child_count > 2) last = last->children[2];
            ast_add_child(p->cs, last, nested_if);
        } else {
            else_block = nested_if;
        }
    }

    /* ELSE */
    if (match(p, BTOK_ELSE)) {
        skip_newlines(p);
        AstNode *else_body = make_block_node(p, p->previous.lineno);
        while (!check(p, BTOK_EOF) && !check(p, BTOK_ENDIF) && !check(p, BTOK_END)) {
            AstNode *stmt = parse_statement(p);
            if (stmt) ast_add_child(p->cs, else_body, stmt);
            while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
        }
        if (else_block) {
            /* Attach to last ELSEIF */
            AstNode *last = else_block;
            while (last->child_count > 2) last = last->children[2];
            ast_add_child(p->cs, last, else_body);
        } else {
            else_block = else_body;
        }
    }

    /* END IF or ENDIF */
    if (match(p, BTOK_ENDIF)) {
        /* ok */
    } else if (check(p, BTOK_END)) {
        advance(p);
        if (!match(p, BTOK_IF)) {
            parser_error(p, "Expected IF after END");
        }
    } else {
        parser_error(p, "Expected END IF or ENDIF");
    }

    AstNode *s = make_sentence_node(p, "IF", lineno);
    if (condition) ast_add_child(p->cs, s, condition);
    ast_add_child(p->cs, s, then_block);
    if (else_block) ast_add_child(p->cs, s, else_block);
    return s;
}

/* ----------------------------------------------------------------
 * FOR statement
 * ---------------------------------------------------------------- */
static AstNode *parse_for_statement(Parser *p) {
    consume(p, BTOK_FOR, "Expected FOR");
    int lineno = p->previous.lineno;

    /* FOR var = start TO end [STEP step] */
    if (!check(p, BTOK_ID)) {
        parser_error(p, "Expected variable name in FOR");
        return NULL;
    }
    advance(p);
    const char *var_name = p->previous.sval;
    int var_lineno = p->previous.lineno;

    consume(p, BTOK_EQ, "Expected '=' in FOR");
    AstNode *start_expr = parse_expression(p, PREC_NONE + 1);
    consume(p, BTOK_TO, "Expected TO in FOR");
    AstNode *end_expr = parse_expression(p, PREC_NONE + 1);

    AstNode *step_expr = NULL;
    if (match(p, BTOK_STEP)) {
        step_expr = parse_expression(p, PREC_NONE + 1);
    } else {
        /* Python p_step defaults a missing STEP to make_number(1) so the
         * 3-way common_type and the sentence's expr3 always exist
         * (zxbparser.py:1627-1629). */
        step_expr = make_number(p, 1, p->previous.lineno, NULL);
    }

    /* id_type = common_type(common_type(start, end), step)
     * (zxbparser.py:1614). C's check_common_type takes AstNodes and
     * reads only their ->type_; mirror Python's nested type call with a
     * transient type-carrier node (never added to the AST). */
    TypeInfo *id_type = check_common_type(p->cs, start_expr, end_expr);
    if (id_type && step_expr) {
        AstNode *tcarrier = ast_new(p->cs, AST_NUMBER, lineno);
        tcarrier->type_ = id_type;
        id_type = check_common_type(p->cs, tcarrier, step_expr);
    }

    /* Resolve the loop variable (Python access_var with
     * default_type=id_type, zxbparser.py:1615). Keep the existing class
     * diagnostics verbatim — the STDERR surface measures them; error-path
     * control flow is Phase 3's domain, not this parser-fidelity fix. */
    AstNode *var = symboltable_access_id(p->cs->symbol_table, p->cs,
                                          var_name, var_lineno, id_type, CLASS_var);
    if (var) {
        if (var->u.id.class_ == CLASS_const)
            zxbc_error(p->cs, var_lineno, "'%s' is a CONST, not a VAR", var_name);
        else if (var->u.id.class_ == CLASS_function)
            zxbc_error(p->cs, var_lineno, "'%s' is a FUNCTION, not a VAR", var_name);
        else if (var->u.id.class_ == CLASS_sub)
            zxbc_error(p->cs, var_lineno, "Cannot assign a value to '%s'. It's not a variable", var_name);
        if (var->u.id.class_ == CLASS_unknown)
            var->u.id.class_ = CLASS_var;
        var->u.id.accessed = true; /* Python mark_entry_as_accessed */
    }

    /* Push loop info */
    LoopInfo li = { LOOP_FOR, lineno, arena_strdup(&p->cs->arena, var_name) };
    vec_push(p->cs->loop_stack, li);

    /* Parse body */
    skip_newlines(p);
    AstNode *body = make_block_node(p, lineno);
    while (!check(p, BTOK_EOF) && !check(p, BTOK_NEXT)) {
        AstNode *stmt = parse_statement(p);
        if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, body, stmt);
        /* Faithful to the for-body grammar `for_start program_co label_next`
         * (zxbparser.py:1553): the body is a `program_co`, every program_line
         * of which ends in NEWLINE; a body statement must therefore be
         * followed by a statement separator (NEWLINE or CO) before NEXT —
         * a statement that runs directly into NEXT with no separator is a
         * PLY p_error on NEXT (e.g. `FOR i=1 TO 10 PRINT "X" NEXT` ->
         * ":2: Unexpected token 'NEXT' <NEXT>"; verified on the oracle).
         * Only reject the no-separator-at-all case (always a Python fault);
         * the all-CO trailing forms PLY also rejects stay accepted here as
         * residual FN — never over-reject. */
        bool sep = false;
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) sep = true;
        if (sep) continue;
        if (check(p, BTOK_EOF)) continue;
        /* `label_next : label NEXT` (zxbparser.py:1565): a LABEL immediately
         * preceding NEXT is the NEXT's own label, NOT an unterminated body
         * statement — parse_statement consumed it as a label-only sentence
         * (AST_SENTENCE kind "LABEL"); when NEXT follows directly, that is
         * the valid `... program_line\n label NEXT` shape (fornext.bas:
         * `10 FOR…\n20 NEXT`), so do not require a separator here. */
        bool stmt_is_label_only =
            stmt && stmt->tag == AST_SENTENCE && stmt->u.sentence.kind &&
            strcmp(stmt->u.sentence.kind, "LABEL") == 0;
        if (check(p, BTOK_NEXT) && stmt_is_label_only) continue;
        /* No separator and not the label_next case: PLY faults on the
         * current (non-separator) token (NEXT for the bare run-on, or the
         * first token of the would-be second statement). */
        tok_unexpected_error(p, &p->current);
        break;
    }

    /* NEXT [var] */
    consume(p, BTOK_NEXT, "Expected NEXT");
    if (check(p, BTOK_ID)) {
        /* p_next1 (zxbparser.py:1571-1585): when NEXT names a variable it
         * must equal the innermost loop's FOR variable (gl.LOOPS[-1].var),
         * else syntax_error_wrong_for_var(p.lineno(2), LOOPS[-1].var, p3)
         * (errmsg.py:209). The comparison is case-SENSITIVE in Boriel
         * (t_ID keeps t.value's original case; verified on the oracle:
         * `For I … Next i` -> error), so strcmp matches exactly. p.lineno(2)
         * is the NEXT-ID token line in the no-label form. */
        const char *next_var = p->current.sval;
        int next_lineno = p->current.lineno;
        advance(p);
        if (next_var && p->cs->loop_stack.len > 0) {
            const char *forvar =
                p->cs->loop_stack.data[p->cs->loop_stack.len - 1].var_name;
            if (forvar && strcmp(forvar, next_var) != 0)
                err_wrong_for_var(p->cs, next_lineno, forvar, next_var);
        }
    }

    /* Pop loop info */
    if (p->cs->loop_stack.len > 0) {
        vec_pop(p->cs->loop_stack);
    }

    AstNode *s = make_sentence_node(p, "FOR", lineno);
    if (var) {
        /* Python: expr{1,2,3} = make_typecast(variable.type_, …)
         * (zxbparser.py:1620-1622); the FOR sentence carries the
         * resolved symbol entry as child[0] (zxbparser.py:1624). */
        start_expr = make_typecast(p->cs, var->type_, start_expr, var_lineno);
        end_expr   = make_typecast(p->cs, var->type_, end_expr, var_lineno);
        step_expr  = make_typecast(p->cs, var->type_, step_expr, var_lineno);
        ast_add_child(p->cs, s, var);
    } else {
        /* var unresolved (class error already emitted above): preserve
         * the pre-fix fallback shape rather than replicating Python's
         * early-return — error-path control flow is Phase 3's domain. */
        AstNode *fallback = ast_new(p->cs, AST_ID, lineno);
        fallback->u.id.name = arena_strdup(&p->cs->arena, var_name);
        ast_add_child(p->cs, s, fallback);
    }
    if (start_expr) ast_add_child(p->cs, s, start_expr);
    if (end_expr) ast_add_child(p->cs, s, end_expr);
    if (step_expr) ast_add_child(p->cs, s, step_expr);
    ast_add_child(p->cs, s, body);
    return s;
}

/* ----------------------------------------------------------------
 * WHILE statement
 * ---------------------------------------------------------------- */
static AstNode *parse_while_statement(Parser *p) {
    consume(p, BTOK_WHILE, "Expected WHILE");
    int lineno = p->previous.lineno;

    AstNode *condition = parse_expression(p, PREC_NONE + 1);

    LoopInfo li = { LOOP_WHILE, lineno, NULL };
    vec_push(p->cs->loop_stack, li);

    skip_newlines(p);
    AstNode *body = make_block_node(p, lineno);
    while (!check(p, BTOK_EOF) && !check(p, BTOK_WEND)) {
        /* Check for END WHILE */
        if (check(p, BTOK_END)) {
            int save_pos = p->lexer.pos;
            BToken save_cur = p->current;
            advance(p);
            if (check(p, BTOK_WHILE)) {
                advance(p);
                goto while_done;
            }
            p->current = save_cur;
            p->lexer.pos = save_pos;
        }
        AstNode *stmt = parse_statement(p);
        if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, body, stmt);
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
    }

    consume(p, BTOK_WEND, "Expected WEND");
while_done:;

    if (p->cs->loop_stack.len > 0)
        vec_pop(p->cs->loop_stack);

    AstNode *s = make_sentence_node(p, "WHILE", lineno);
    if (condition) ast_add_child(p->cs, s, condition);
    ast_add_child(p->cs, s, body);
    return s;
}

/* ----------------------------------------------------------------
 * DO statement
 * ---------------------------------------------------------------- */
static AstNode *parse_do_statement(Parser *p) {
    consume(p, BTOK_DO, "Expected DO");
    int lineno = p->previous.lineno;

    LoopInfo li = { LOOP_DO, lineno, NULL };
    vec_push(p->cs->loop_stack, li);

    /* DO WHILE cond / DO UNTIL cond */
    const char *kind = "DO_LOOP";
    AstNode *pre_cond = NULL;
    bool is_pretest = false;

    if (match(p, BTOK_WHILE)) {
        pre_cond = parse_expression(p, PREC_NONE + 1);
        kind = "DO_WHILE";
        is_pretest = true;
    } else if (match(p, BTOK_UNTIL)) {
        pre_cond = parse_expression(p, PREC_NONE + 1);
        kind = "DO_UNTIL";
        is_pretest = true;
    }

    /* For the PLAIN DO forms (`do_start program_co label_loop` /
     * `do_start label_loop` / `DO label_loop`, zxbparser.py:1688-1690),
     * `do_start : DO CO | DO NEWLINE` (zxbparser.py:1908-1910) — so after a
     * bare DO the only valid continuations are CO, NEWLINE, or LOOP/a label
     * then LOOP (the `DO label_loop` infinite-loop form). A bare DO that
     * runs straight into a statement (e.g. `DO LET M=0: …`) is a PLY
     * p_error on that token (`Unexpected token 'LET' <LET>`; verified on the
     * oracle). The pre-test `DO WHILE expr` / `DO UNTIL expr` forms have no
     * such separator requirement (`do_while_start : DO WHILE expr`) so this
     * gate applies ONLY to the plain DO. LABEL is allowed (label_loop's
     * `label LOOP`, or a labelled body line). */
    if (!is_pretest && !check(p, BTOK_CO) && !check(p, BTOK_NEWLINE) &&
        !check(p, BTOK_LOOP) && !check(p, BTOK_LABEL) && !check(p, BTOK_EOF)) {
        tok_unexpected_error(p, &p->current);
        if (p->cs->loop_stack.len > 0)
            vec_pop(p->cs->loop_stack);
        return NULL;
    }

    /* `do_start : DO CO | DO NEWLINE` (zxbparser.py:1908-1910) is exactly ONE
     * terminator. The DO-NEWLINE form's separator is consumed by skip_newlines
     * below; the DO-CO form's leading ':' must be consumed symmetrically so an
     * EMPTY same-line body reaches the LOOP terminator cleanly — `DO: LOOP`,
     * `DO: LOOP WHILE 0`, `DO: LOOP UNTIL 0` are valid `do_start label_loop`
     * (zxbparser.py:1689/1840/1710) and Python exits 0. Without consuming it
     * the body loop runs parse_statement on the leftover ':' (-> empty NOP +
     * trailing CO) and the plain-DO no-program-before-trailing-CO rule then
     * faults on LOOP — the PASS->FALSE_POS regression.
     *
     * Consume the do_start CO ONLY when it is immediately followed by LOOP
     * (the same-line empty-body form). This is deliberately narrow: a CO
     * followed by anything else (a body statement, a second CO, a mid-line
     * label, or a NEWLINE) is left for the existing body-loop handling, which
     * already matches Python on every such shape (`DO: LET a=1: LOOP` ->
     * reject; `DO:: LOOP` -> reject; `DO: 20 LOOP` -> reject;
     * `DO:\nLOOP` / `DO:\n20 LOOP` / `DO:\n: LOOP` -> accept). The NEWLINE
     * case stays with skip_newlines so none of those paths shift. */
    if (!is_pretest && check(p, BTOK_CO)) {
        int save_pos = p->lexer.pos;
        BToken save_cur = p->current;
        advance(p);
        if (!check(p, BTOK_LOOP)) {
            /* Not the same-line empty `DO: LOOP` form — restore the CO for
             * the body loop (every non-empty / multi-token shape is handled
             * faithfully there). */
            p->current = save_cur;
            p->lexer.pos = save_pos;
        }
    }

    skip_newlines(p);
    AstNode *body = make_block_node(p, lineno);
    /* Body validity before LOOP. The plain-DO body is `program_co` only
     * (no `do_start co_statements_co label_loop` production) — so a
     * trailing CO-terminated fragment is valid ONLY when a NEWLINE-
     * terminated `program` precedes it; `DO\nLET M=0: LOOP` therefore
     * faults on LOOP (doloopuntilsplitted.bas:3). The pre-test DO
     * WHILE/UNTIL body additionally has a `co_statements_co LOOP`
     * production (zxbparser.py:1864/1882), accepting an all-CO body
     * (`DO WHILE 1: PRINT 1: LOOP` is OK), so for pre-test we only
     * reject a statement running into LOOP with no separator at all.
     * Both: never over-reject — residual rejects PLY also makes stay FN. */
    bool seen_nl_terminated = false;
    while (!check(p, BTOK_EOF) && !check(p, BTOK_LOOP)) {
        AstNode *stmt = parse_statement(p);
        if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, body, stmt);
        bool sep = false, nl = false;
        while (check(p, BTOK_NEWLINE) || check(p, BTOK_CO)) {
            if (check(p, BTOK_NEWLINE)) nl = true;
            advance(p);
            sep = true;
        }
        if (nl) seen_nl_terminated = true;
        if (check(p, BTOK_EOF)) break;
        /* `label_loop : label LOOP` (zxbparser.py:1678): a LABEL immediately
         * preceding LOOP is the LOOP's own label (parse_statement consumed it
         * as a label-only sentence), NOT an unterminated body statement —
         * `DO\n20 LOOP` and `DO\nLET a=1\n30 LOOP` are valid (doloop*.bas).
         * Exempt exactly as the FOR body exempts label_next. */
        bool stmt_is_label_only =
            stmt && stmt->tag == AST_SENTENCE && stmt->u.sentence.kind &&
            strcmp(stmt->u.sentence.kind, "LABEL") == 0;
        if (check(p, BTOK_LOOP) && stmt_is_label_only) break;
        if (check(p, BTOK_LOOP)) {
            /* At the LOOP terminator. The just-parsed statement must be
             * properly closed before LOOP. */
            if (!sep) {
                /* Statement ran directly into LOOP, no separator — always
                 * a PLY fault (both plain and pre-test). */
                tok_unexpected_error(p, &p->current);
                if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
                return NULL;
            }
            if (!is_pretest && !nl && !seen_nl_terminated) {
                /* Plain DO: trailing CO-fragment with no preceding
                 * NEWLINE-terminated program -> fault on LOOP. */
                tok_unexpected_error(p, &p->current);
                if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
                return NULL;
            }
            break;
        }
        if (!sep) {
            /* Two statements with no separator between them -> fault on the
             * first token of the second. */
            tok_unexpected_error(p, &p->current);
            if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
            return NULL;
        }
    }

    consume(p, BTOK_LOOP, "Expected LOOP");
    int loop_line = p->previous.lineno;

    /* LOOP WHILE cond / LOOP UNTIL cond */
    AstNode *post_cond = NULL;
    int cond_kw_line = loop_line;
    if (match(p, BTOK_WHILE)) {
        cond_kw_line = p->previous.lineno;
        post_cond = parse_expression(p, PREC_NONE + 1);
        kind = "LOOP_WHILE";
    } else if (match(p, BTOK_UNTIL)) {
        cond_kw_line = p->previous.lineno;
        post_cond = parse_expression(p, PREC_NONE + 1);
        kind = "LOOP_UNTIL";
    }

    if (p->cs->loop_stack.len > 0)
        vec_pop(p->cs->loop_stack);

    /* p_do_while_loop / p_do_until_loop (src/zxbc/zxbparser.py:1862-1893):
     * a PRE-test loop (DO WHILE <expr> / DO UNTIL <expr> ... LOOP) warns
     * W110 when its condition is a constant number — `if is_number(r):
     * warning_condition_is_always(p.lineno(2), bool(r.value))`. Although the
     * source cites p.lineno(2) (the body / LOOP position), PLY's
     * non-terminal lineno tracking resolves it to the DO line in every case
     * (verified on the oracle: empty body, non-empty body, and a leading-
     * offset DO all report the DO keyword's line, never the body/LOOP line).
     * Reproduce the observed behaviour: emit at the DO line (`lineno`). The
     * pre- and post-test forms are mutually exclusive grammar productions
     * (DO WHILE ... LOOP UNTIL is a syntax error), so pre_cond and post_cond
     * never coexist; emit independently of the post-test block below. */
    if (pre_cond && check_is_number(pre_cond)) {
        double cv = 0;
        zxbc_eval_to_num(pre_cond, &cv);
        warn_condition_always(p->cs, lineno, cv != 0.0);
    }

    /* p_do_loop_until / p_do_loop_while (src/zxbc/zxbparser.py:1708-1730,
     * 1838-1859): a post-test loop warns W110 when its condition is a
     * constant, and W130 when its body is empty.  Python's p.lineno(3):
     * the empty-body form has the bare LOOP as label_loop (token 2), so
     * token 3 is the UNTIL/WHILE keyword; the non-empty form has
     * label_loop (the LOOP keyword) at token 3.  Mirror that: W130 (only
     * fires on the empty body) -> the UNTIL/WHILE keyword line; W110 ->
     * that same keyword line for an empty body, else the LOOP keyword
     * line.  (W110 before W130, matching the Python order.) */
    if (post_cond) {
        bool body_empty = (body->child_count == 0);
        if (check_is_number(post_cond)) {
            double cv = 0;
            zxbc_eval_to_num(post_cond, &cv);
            warn_condition_always(p->cs,
                                  body_empty ? cond_kw_line : loop_line,
                                  cv != 0.0);
        }
        if (body_empty)
            warn_empty_loop(p->cs, cond_kw_line);
    }

    AstNode *s = make_sentence_node(p, kind, lineno);
    if (pre_cond) ast_add_child(p->cs, s, pre_cond);
    ast_add_child(p->cs, s, body);
    if (post_cond) ast_add_child(p->cs, s, post_cond);
    return s;
}

/* ----------------------------------------------------------------
 * DIM / CONST declaration
 * ---------------------------------------------------------------- */
/* const_vector_elem — the per-element const-check for a const-vector element,
 * extracted from parse_array_initializer so the Phase-D PLY const_number_list
 * reduces (51/52) build the byte-for-byte same element node. Faithful port of
 * src/zxbc/zxbparser.py p_const_vector_elem_list:891-897 / p_const_vector_elem_
 * list_list:909-915 (both identical): a non-static element that is a UNARY is
 * make_constexpr-wrapped (delayed const eval); a non-static non-UNARY element is
 * rejected ("Initializer expression is not constant") at `err_lineno`. Returns
 * the (possibly CONSTEXPR-wrapped) element. */
static AstNode *const_vector_elem(Parser *p, AstNode *expr, int err_lineno) {
    if (!check_is_static(expr)) {
        if (expr->tag == AST_UNARY) {
            AstNode *ce = ast_new(p->cs, AST_CONSTEXPR, expr->lineno);
            ast_add_child(p->cs, ce, expr);
            ce->type_ = expr->type_;
            expr = ce;
        } else {
            err_not_constant(p->cs, err_lineno);
        }
    }
    return expr;
}

/* Parse brace-enclosed initializer: {expr, expr, ...} or {{...}, {...}} */
static AstNode *parse_array_initializer(Parser *p) {
    int lineno = p->current.lineno;
    consume(p, BTOK_LBRACE, "Expected '{'");
    AstNode *init = ast_new(p->cs, AST_ARRAYINIT, lineno);
    /* p_const_vector_vector_list (zxbparser.py:929-934): in a list of
     * const_vectors (rows of `{...}`), every row must have the same number
     * of elements as the FIRST row (len(p[3]) != len(p[1][0])), else "All
     * rows must have the same number of elements" at p.lineno(2)=the COMMA,
     * and the const_vector reduces to None — which makes p_arr_decl_initialized
     * (p[8] is None) return WITHOUT running check_bound. We mirror that:
     * track the first row's element count, diagnose a mismatched row, and
     * return NULL so the DIM-level check_bound (the divergent "Mismatched
     * vector size" message) is skipped and no array is declared. */
    int first_row_count = -1;
    bool ragged = false;

    if (!check(p, BTOK_RBRACE)) {
        do {
            if (check(p, BTOK_LBRACE)) {
                int comma_lineno = p->previous.lineno;  /* the COMMA (rows>1) */
                /* Nested initializer for multi-dim arrays */
                AstNode *sub = parse_array_initializer(p);
                if (sub) {
                    if (first_row_count < 0) {
                        first_row_count = sub->child_count;
                    } else if (sub->child_count != first_row_count && !ragged) {
                        zxbc_error(p->cs, comma_lineno,
                                   "All rows must have the same number of elements");
                        ragged = true;
                    }
                    ast_add_child(p->cs, init, sub);
                }
            } else {
                AstNode *expr = parse_expression(p, PREC_NONE + 1);
                if (expr) {
                    /* Faithful port of the const-vector element handling in
                     * src/zxbc/zxbparser.py p_const_vector_elem_list:891-897
                     * (first elem) and p_const_vector_elem_list_list:909-915
                     * (subsequent elems). Both productions are identical:
                     *     if not is_static(e):
                     *         if isinstance(e, sym.UNARY):
                     *             tmp = make_constexpr(p.lineno, e)  # delayed
                     *         else:
                     *             errmsg.syntax_error_not_constant(...)
                     * i.e. a non-static element that is NOT a UNARY ->
                     * "Initializer expression is not constant."  The C
                     * do/while collects BOTH the first and subsequent
                     * elements, so this single per-element if/else mirrors
                     * both Python productions exactly.
                     *
                     * The non-UNARY reject set is precisely Python's, driven
                     * by p_addr_of_id's is_dynamic-conditional CONSTEXPR-wrap
                     * (zxbparser.py:2682-2685, ported at parser.c:981):
                     *   - A bare CLASS_var runtime id (array11's `{ A, .. }`)
                     *     is AST_ID, not static, not UNARY -> rejected.
                     *   - A `@global`/`@function` address is is_dynamic-FALSE
                     *     so p_addr_of_id already CONSTEXPR-wraps it; `@g + 1`
                     *     then make_binary-folds to a CONSTEXPR (is_static
                     *     a,b True, binary.py:113-116) -> static -> ACCEPTED
                     *     (arrlabels2/3/10b: their elems arrive CONSTEXPR/
                     *     NUMBER, never a bare BINARY).
                     *   - A `@local`/`@param` address is is_dynamic-TRUE so
                     *     p_addr_of_id returns a bare ADDRESS UNARY (no
                     *     CONSTEXPR).  Bare `@local` is a UNARY -> CONSTEXPR-
                     *     wrapped below (ACCEPTED, like arrlabels4/5/7/8/9).
                     *     But `@local + 1` is an AST_BINARY whose `@local`
                     *     operand is a non-static UNARY, so make_binary does
                     *     NOT fold it (is_static a,b False) -> it stays a
                     *     bare BINARY: not static, not UNARY -> REJECTED.
                     *     This is the arrlabels11/11b case Python rejects
                     *     (`DIM a(..) AS UInteger => {@a, @a + 1, 3}` inside a
                     *     SUB), matching `:4: Initializer ... not constant.`.
                     * A CLASS_const id is is_static-true via check_is_const
                     * (dim_const0's {xx,xx} stays rc=0 like Python).
                     *
                     * Lineno: Python emits at p.lexer.lineno (first elem) /
                     * p.lineno(2)=COMMA (subsequent) == the const-vector's
                     * line == init->lineno (the '{'); these fixtures are all
                     * single-line DIMs so init->lineno is the faithful value
                     * (array11/arrlabels11/11b -> line 4, matching .err). */
                    /* make_constexpr delayed-const-eval wrap (a non-static
                     * UNARY, e.g. `@label` whose entry is SCOPE.local) /
                     * "not constant" reject (non-static non-UNARY) — shared
                     * with the Phase-D const_number_list reduces. Lineno is
                     * init->lineno (the '{'; these fixtures are single-line). */
                    expr = const_vector_elem(p, expr, init->lineno);
                    ast_add_child(p->cs, init, expr);
                }
            }
        } while (match(p, BTOK_COMMA));
    }
    consume(p, BTOK_RBRACE, "Expected '}'");
    /* Ragged const-vector -> Python's const_vector reduced to None. Return
     * NULL so the caller skips check_bound and the array declaration,
     * exactly as p_arr_decl_initialized's `if ... p[8] is None: return`. */
    if (ragged) return NULL;
    return init;
}

/* ----------------------------------------------------------------
 * make_bound — array-bound semantic validation
 *   Faithful port of src/symbols/bound.py SymbolBOUND.make_node
 *   (invoked via src/zxbc/zxbparser.py:444 make_bound), with the
 *   five rejects in Python's exact precedence order.
 *
 * eval_to_num analogue (src/api/utils.py:173 eval_to_num):
 *   Python computes eval(node.t, {}, {}) where node.t is the node's
 *   string repr.  For a NUMBER that repr is str(value) -> the number.
 *   For a *numeric* named CONST, SymbolBINARY.make_node already FOLDED
 *   any const+number arithmetic so the CONST's .t is the resolved
 *   numeric string (verified: `const B = A + 2` -> B.t == '3').  For a
 *   CONSTEXPR carrying a non-numeric term (e.g. const9's `@dgConnected`
 *   address-of) the repr is f"#{...}" -> eval() SyntaxError -> None.
 *
 *   The C front-end does NOT fold numeric-const arithmetic to a bare
 *   NUMBER (make_typecast keeps a CONST-ref an AST_ID, so make_binary
 *   wraps it in a CONSTEXPR rather than taking the fast NUMBER fold).
 *   To reproduce Python's NET eval_to_num outcome (fold + eval) — and
 *   so NEVER reject what Python accepts — the C analogue numerically
 *   evaluates the static expression tree: a CONSTEXPR/BINARY/UNARY of
 *   numeric leaves resolves to its value (Python folded it to a
 *   number), while a genuinely non-numeric leaf (address-of, label,
 *   string, runtime var) yields "unresolved" exactly as Python's
 *   eval() fails on the `#...`/NameError forms.
 *
 * Returns true and stores *out on success; false == Python's None.
 * ---------------------------------------------------------------- */
static bool zxbc_eval_to_num(const AstNode *n, double *out);

static bool zxbc_eval_binop(const char *op, double l, double r, double *out) {
    /* Mirrors compiler.c fold_numeric for the operators a *static*
     * array-bound CONSTEXPR can contain.  A non-numeric / undefined
     * operator (or div/mod by zero, as Python's eval would ZeroDivision
     * -> not caught by eval_to_num's (NameError,SyntaxError,ValueError)
     * so it would propagate; but a bound never reaches here with /0 in
     * the corpus) yields false == unresolved. */
    if (strcmp(op, "PLUS") == 0)  { *out = l + r; return true; }
    if (strcmp(op, "MINUS") == 0) { *out = l - r; return true; }
    if (strcmp(op, "MULT") == 0 || strcmp(op, "MUL") == 0) { *out = l * r; return true; }
    if (strcmp(op, "DIV") == 0)  { if (r == 0) return false; *out = l / r; return true; }
    if (strcmp(op, "MOD") == 0)  { if (r == 0) return false; *out = fmod(l, r); return true; }
    if (strcmp(op, "POW") == 0)  { *out = pow(l, r); return true; }
    if (strcmp(op, "SHL") == 0)  { *out = (double)((int64_t)l << (int64_t)r); return true; }
    if (strcmp(op, "SHR") == 0)  { *out = (double)((int64_t)l >> (int64_t)r); return true; }
    if (strcmp(op, "BAND") == 0) { *out = (double)((int64_t)l & (int64_t)r); return true; }
    if (strcmp(op, "BOR") == 0)  { *out = (double)((int64_t)l | (int64_t)r); return true; }
    if (strcmp(op, "BXOR") == 0) { *out = (double)((int64_t)l ^ (int64_t)r); return true; }
    if (strcmp(op, "LT") == 0)   { *out = (l <  r) ? 1 : 0; return true; }
    if (strcmp(op, "GT") == 0)   { *out = (l >  r) ? 1 : 0; return true; }
    if (strcmp(op, "EQ") == 0)   { *out = (l == r) ? 1 : 0; return true; }
    if (strcmp(op, "LE") == 0)   { *out = (l <= r) ? 1 : 0; return true; }
    if (strcmp(op, "GE") == 0)   { *out = (l >= r) ? 1 : 0; return true; }
    if (strcmp(op, "NE") == 0)   { *out = (l != r) ? 1 : 0; return true; }
    if (strcmp(op, "AND") == 0)  { *out = ((int64_t)l && (int64_t)r) ? 1 : 0; return true; }
    if (strcmp(op, "OR") == 0)   { *out = ((int64_t)l || (int64_t)r) ? 1 : 0; return true; }
    if (strcmp(op, "XOR") == 0)  { *out = ((!!(int64_t)l) ^ (!!(int64_t)r)) ? 1 : 0; return true; }
    return false;
}

static bool zxbc_eval_to_num(const AstNode *n, double *out) {
    if (!n) return false;
    switch (n->tag) {
    case AST_NUMBER:
        *out = n->u.number.value;
        return true;
    case AST_CONSTEXPR:
        /* Python CONSTEXPR.t == f"#{traverse_const}"; eval() fails on
         * the leading '#'.  But Python only ever has a CONSTEXPR here
         * when traverse_const itself is non-numeric — a numeric one was
         * already folded to a NUMBER upstream.  So recurse the inner
         * expr: numeric => Python's folded value; non-numeric leaf =>
         * unresolved (== Python's `#...` -> None). */
        return n->child_count > 0 && zxbc_eval_to_num(n->children[0], out);
    case AST_ID:
        /* A named CONST: Python's .t is the resolved numeric string
         * (const+number arithmetic already folded).  Resolve via the
         * stored constant value (default_value_expr).  A non-const ID
         * (a DIM var) is not static and is rejected earlier by
         * check_is_static, but defensively yields unresolved here. */
        if (n->u.id.class_ == CLASS_const && n->u.id.default_value_expr)
            return zxbc_eval_to_num(n->u.id.default_value_expr, out);
        return false;
    case AST_BINARY: {
        double l, r;
        if (n->child_count < 2) return false;
        if (!zxbc_eval_to_num(n->children[0], &l)) return false;
        if (!zxbc_eval_to_num(n->children[1], &r)) return false;
        return zxbc_eval_binop(n->u.binary.operator, l, r, out);
    }
    case AST_UNARY: {
        double v;
        const char *op = n->u.unary.operator;
        /* ADDRESS (@name) is non-numeric -> Python eval() fails (this
         * is precisely const9's `@dgConnected` -> Unknown bound). */
        if (op && strcmp(op, "ADDRESS") == 0) return false;
        if (n->child_count < 1) return false;
        if (!zxbc_eval_to_num(n->children[0], &v)) return false;
        if (op && strcmp(op, "MINUS") == 0) { *out = -v; return true; }
        if (op && strcmp(op, "PLUS") == 0)  { *out = v;  return true; }
        if (op && strcmp(op, "NOT") == 0)   { *out = (v == 0) ? 1 : 0; return true; }
        if (op && strcmp(op, "BNOT") == 0)  { *out = (double)(~(int64_t)v); return true; }
        return false;
    }
    default:
        /* STRING, builtins, runtime VAR refs, slices, etc. — Python's
         * eval() raises NameError/SyntaxError/ValueError -> None. */
        return false;
    }
}

/* Faithful port of SymbolARRAYACCESS.offset (symbols/arrayaccess.py
 * :68-91) — the constant byte offset of an array element from the start
 * of the array DATA region when EVERY subscript is a compile-time
 * constant; otherwise "not constant" (Python returns None).
 *
 *   if self.scope == SCOPE.parameter: return None          (:77-78)
 *   offset = 0
 *   for i, b in zip(self.arglist, self.entry.bounds):       (:83)
 *       tmp = i.children[0]                                  (:84)
 *       if is_number(tmp) or is_const(tmp):                  (:85)
 *           offset = offset * b.count + (tmp.value - b.lower)(:86)
 *       else: return None                                    (:88)
 *   offset *= self.type_.size                                (:90)
 *
 * `i` is the C AST_ARGUMENT wrapping the (already BOUND_TYPE-typecast'd)
 * subscript expr at child[0]; `b` is the matching AST_BOUND
 * (child[0]=lower, child[1]=upper; .count == upper-lower+1, .lower==
 * lower — bound.py:33-38), read from entry.ref.bounds == the array ID's
 * arr_boundlist. is_number(tmp)/is_const(tmp) (check.py:293,312): a
 * NUMBER or a CLASS_const id of numeric type — tmp.value resolves via
 * zxbc_eval_to_num (handles NUMBER, named CONST via default_value_expr,
 * and CONSTEXPR-folded forms exactly as the BOUND path does). Python's
 * zip() stops at the shorter sequence (a byref-param array has empty
 * bounds → no iterations → offset stays 0 → const 0); but the
 * SCOPE.parameter early-return already covers that, mirrored here. */
static void compute_arrayaccess_offset(Parser *p, AstNode *acc,
                                       AstNode *entry, AstNode *arglist) {
    (void)p;
    acc->u.arrayaccess.offset = 0;
    acc->u.arrayaccess.is_const = false;

    /* :77-78 — a parameter array is never constant-foldable. */
    if (!entry || entry->u.id.scope == SCOPE_parameter)
        return;

    AstNode *boundlist = entry->u.id.arr_boundlist;
    int ndims = boundlist ? boundlist->child_count : 0;
    int nargs = arglist ? arglist->child_count : 0;
    int n = ndims < nargs ? ndims : nargs;   /* Python zip() shortest */

    long offset = 0;
    for (int k = 0; k < n; k++) {
        AstNode *arg = arglist->children[k];
        AstNode *tmp = (arg && arg->tag == AST_ARGUMENT && arg->child_count > 0)
                           ? arg->children[0]
                           : arg;          /* i.children[0] */
        AstNode *bd = boundlist->children[k];
        if (!tmp || !bd || bd->child_count < 2)
            return;                        /* defensive — treat as dynamic */

        /* is_number(tmp) or is_const(tmp) (check.py:293,312). */
        if (!(check_is_number(tmp) || check_is_const(tmp)))
            return;                        /* :88 — not constant */

        double tv, lo, up;
        if (!zxbc_eval_to_num(tmp, &tv))           return;
        if (!zxbc_eval_to_num(bd->children[0], &lo)) return;
        if (!zxbc_eval_to_num(bd->children[1], &up)) return;

        long count = (long)up - (long)lo + 1;      /* BOUND.count */
        offset = offset * count + ((long)tv - (long)lo);
    }

    /* :90 — offset *= self.type_.size (element size; acc->type_ is the
     * array element type, set to entry->type_ by the caller). */
    offset *= type_size(acc->type_);
    acc->u.arrayaccess.offset = offset;
    acc->u.arrayaccess.is_const = true;
}

/* Port A: faithful src/symbols/bound.py SymbolBOUND.make_node.
 * Returns the (unchanged) AST_BOUND on success, or NULL after emitting
 * the matching Python error.  The bound's children are NOT rewritten —
 * accepted-array AST construction must stay byte-identical (the resolved
 * lower/upper are recomputed on demand for check_bound's .count). */
static AstNode *make_bound(Parser *p, AstNode *bound, int lineno) {
    if (!bound || bound->child_count < 2) return NULL;
    AstNode *lower = bound->children[0];
    AstNode *upper = bound->children[1];

    /* bound.py:43 — if not check.is_static(lower, upper) */
    if (!check_is_static(lower) || !check_is_static(upper)) {
        zxbc_error(p->cs, lineno, "Array bounds must be constants");
        return NULL;
    }

    double lo, up;
    /* bound.py:47-49 — lower_value = eval_to_num(lower.t); if None */
    if (!zxbc_eval_to_num(lower, &lo)) {
        zxbc_error(p->cs, lineno, "Unknown lower bound for array dimension");
        return NULL;
    }
    /* bound.py:52-54 — upper_value = eval_to_num(upper.t); if None */
    if (!zxbc_eval_to_num(upper, &up)) {
        zxbc_error(p->cs, lineno, "Unknown upper bound for array dimension");
        return NULL;
    }
    /* bound.py:57-58 — if lower_value < 0 */
    if (lo < 0) {
        zxbc_error(p->cs, lineno, "Array bounds must be greater than 0");
        return NULL;
    }
    /* bound.py:61-62 — if lower_value > upper_value */
    if (lo > up) {
        zxbc_error(p->cs, lineno,
                   "Lower array bound must be less or equal to upper one");
        return NULL;
    }
    return bound;
}

/* check_bound's BOUND.count == upper - lower + 1 (bound.py:36-38), on
 * the resolved values.  Only called for bounds make_bound accepted. */
static long bound_count(const AstNode *bound) {
    double lo = 0, up = 0;
    if (bound && bound->child_count >= 2) {
        zxbc_eval_to_num(bound->children[0], &lo);
        zxbc_eval_to_num(bound->children[1], &up);
    }
    return (long)up - (long)lo + 1;
}

/* Port B: faithful src/zxbc/zxbparser.py:807-838 p_arr_decl_initialized
 * check_bound closure.  `bounds` are the SUCCESSFULLY-built BOUND nodes
 * (Python's p[4].children — SymbolBOUNDLIST.make_node skips None bounds,
 * so a make_bound-rejected dimension is absent, exactly as here).
 * `remaining` is the const-vector image: an AST_ARRAYINIT == Python's
 * list; anything else == a scalar leaf.  Recurses bounds[start..] vs the
 * nested image, in Python's exact branch order.  Returns false (and has
 * emitted one error) on the first mismatch. */
static bool check_bound_recurse(Parser *p, AstNode *boundlist, int start,
                                AstNode *remaining, int lineno) {
    int nbounds = boundlist ? boundlist->child_count - start : 0;
    bool rem_is_list = (remaining && remaining->tag == AST_ARRAYINIT);

    if (nbounds <= 0) {  /* zxbparser.py:810 — not boundlist */
        if (!rem_is_list)
            return true;  /* :811-812 — return True (OK) */
        zxbc_error(p->cs, lineno,
                   "Unexpected extra vector dimensions. It should be %d",
                   remaining->child_count);
        return false;     /* :814-817 */
    }

    if (!rem_is_list) {   /* :819 — not isinstance(remaining, list) */
        zxbc_error(p->cs, lineno,
                   "Mismatched vector size. Missing %d extra dimension(s)",
                   nbounds);
        return false;     /* :821-824 */
    }

    long want = bound_count(boundlist->children[start]);
    if ((long)remaining->child_count != want) {  /* :826 */
        zxbc_error(p->cs, lineno,
                   "Mismatched vector size. Expected %ld elements, got %d.",
                   want, remaining->child_count);
        return false;     /* :828-830 */
    }

    for (int i = 0; i < remaining->child_count; i++) {  /* :833-835 */
        if (!check_bound_recurse(p, boundlist, start + 1,
                                 remaining->children[i], lineno))
            return false;
    }
    return true;
}

static AstNode *parse_dim_statement(Parser *p) {
    bool is_const = match(p, BTOK_CONST);
    if (!is_const) consume(p, BTOK_DIM, "Expected DIM or CONST");
    int lineno = p->previous.lineno;

    if (!check(p, BTOK_ID) && !check(p, BTOK_ARRAY_ID)) {
        parser_error(p, "Expected variable name");
        return NULL;
    }
    advance(p);
    const char *name = p->previous.sval;

    /* Check for array: DIM name(bounds) */
    if (match(p, BTOK_LP)) {
        /* Array declaration */
        AstNode *bounds = ast_new(p->cs, AST_BOUNDLIST, lineno);
        do {
            AstNode *lower_expr = parse_expression(p, PREC_NONE + 1);
            AstNode *upper_expr = NULL;
            if (match(p, BTOK_TO)) {
                upper_expr = parse_expression(p, PREC_NONE + 1);
            } else {
                /* Single bound: 0 TO expr (or array_base TO expr) */
                upper_expr = lower_expr;
                lower_expr = make_number(p, p->cs->opts.array_base, lineno, NULL);
            }
            AstNode *bound = ast_new(p->cs, AST_BOUND, lineno);
            if (lower_expr) ast_add_child(p->cs, bound, lower_expr);
            if (upper_expr) ast_add_child(p->cs, bound, upper_expr);
            /* Port A — sym.BOUND.make_node (bound.py:40-65) via
             * make_bound (zxbparser.py:442-444).  SymbolBOUNDLIST
             * .make_node (boundlist.py:38-43) skips a None bound, so a
             * rejected dimension is absent from the boundlist — exactly
             * what check_bound (Port B) then walks. */
            if (make_bound(p, bound, lineno))
                ast_add_child(p->cs, bounds, bound);
        } while (match(p, BTOK_COMMA));
        consume(p, BTOK_RP, "Expected ')' after array bounds");

        TypeInfo *type = parse_typedef(p);
        if (!type) {
            /* Check for deprecated suffix ($%&!) to infer type */
            size_t nlen = strlen(name);
            if (nlen > 0 && is_deprecated_suffix(name[nlen - 1])) {
                BasicType bt = suffix_to_type(name[nlen - 1]);
                type = p->cs->symbol_table->basic_types[bt];
            } else {
                type = type_new_ref(p->cs, p->cs->default_type, lineno, true);
                /* Strict mode: error on implicit type in DIM */
                if (p->cs->opts.strict)
                    zxbc_error(p->cs, lineno, "strict mode: missing type declaration for '%s'", name);
            }
        }

        /* DIM array AT expr */
        AstNode *arr_at_expr = NULL;
        if (match(p, BTOK_AT)) {
            arr_at_expr = parse_expression(p, PREC_NONE + 1);
        }

        /* Array initializer: => {...} or = {...} */
        AstNode *init = NULL;
        bool brace_init = false;
        if (match(p, BTOK_RIGHTARROW)) {
            brace_init = true;
            init = parse_array_initializer(p);
        } else if (match(p, BTOK_EQ)) {
            if (check(p, BTOK_LBRACE)) {
                brace_init = true;
                init = parse_array_initializer(p);
            } else {
                init = parse_expression(p, PREC_NONE + 1);
            }
        }

        /* parse_array_initializer returns NULL only for a ragged const
         * vector (the error is already emitted). Python's
         * p_arr_decl_initialized then returns None (p[8] is None) without
         * declaring the array — mirror that here so no array is declared
         * and check_bound is skipped (single faithful error). */
        if (brace_init && !init)
            return NULL;

        /* Port B — p_arr_decl_initialized.check_bound
         * (zxbparser.py:807-838).  Python runs it ONLY for the
         * const-vector-initialized form (`=> {...}` / `= {...}`), i.e.
         * an AST_ARRAYINIT image, and only when bounds/typedef/vector
         * all parsed (p[4]/p[6]/p[8] not None — type is always set
         * here; bounds always exists).  The error lineno is p.lineno(8)
         * == the const-vector's line == init->lineno (set at its '{').
         * On mismatch Python returns before declare_array — but the C
         * declare here is symboltable_declare (further down) and a
         * reject has already flipped error_count, so exit is 1 exactly
         * as Python.  Faithful: do NOT touch the bare-DIM (p_decl_arr)
         * path, which has no check_bound. */
        if (init && init->tag == AST_ARRAYINIT) {
            check_bound_recurse(p, bounds, 0, init, init->lineno);
        }

        /* Check: cannot initialize array of type string */
        if (init && type && type_is_string(type)) {
            zxbc_error(p->cs, lineno, "Cannot initialize array of type string");
        }

        /* AT after initializer is not allowed (Python: either => or AT, not both) */
        if (!arr_at_expr && check(p, BTOK_AT)) {
            if (init) {
                zxbc_error(p->cs, lineno, "Syntax Error. Unexpected token 'AT' <AT>");
                advance(p); /* consume AT */
                parse_expression(p, PREC_NONE + 1); /* consume the address */
            } else {
                advance(p); /* consume AT */
                arr_at_expr = parse_expression(p, PREC_NONE + 1);
            }
        }

        return dim_build_array(p, name, bounds, type, arr_at_expr, init, lineno);
    }

    /* Scalar: collect comma-separated names, then AS type, then AT/= */
    /* DIM a, b, c AS Integer or DIM a AS Integer = 5 */
    const char *names[64];
    int name_count = 0;
    names[name_count++] = name;

    while (match(p, BTOK_COMMA)) {
        if (!check(p, BTOK_ID) && !check(p, BTOK_ARRAY_ID)) {
            parser_error(p, "Expected variable name after ','");
            break;
        }
        advance(p);
        if (name_count < 64) names[name_count++] = p->previous.sval;
    }

    /* p_decl_arr (zxbparser.py:791-799): DIM idlist LP bound_list RP typedef.
     * A single-name array (`DIM a(10)`) took the array path above; reaching
     * a '(' here means the idlist held >1 name (`DIM a, b(10) AS T`), which
     * Python rejects with "Array declaration only allows one variable name
     * at a time" at p.lineno(1)=DIM. Python's grammar still consumes the
     * bound_list + typedef and reduces the production (no cascade), so skip
     * the balanced-paren bounds and the trailing typedef before returning. */
    if (check(p, BTOK_LP)) {
        zxbc_error(p->cs, lineno,
                   "Array declaration only allows one variable name at a time");
        advance(p);  /* ( */
        int depth = 1;
        while (depth > 0 && !check(p, BTOK_EOF) && !check(p, BTOK_NEWLINE) &&
               !check(p, BTOK_CO)) {
            if (check(p, BTOK_LP)) depth++;
            else if (check(p, BTOK_RP)) depth--;
            advance(p);
        }
        parse_typedef(p);  /* consume the trailing AS <type>, if present */
        return NULL;
    }

    TypeInfo *type = parse_typedef(p);
    /* An explicit "AS type" clause yields a non-implicit TYPEREF
     * (parse_type_name -> type_new_ref(..., implicit=false)); the
     * epsilon / inferred typedef is implicit. This mirrors Python's
     * typedef.implicit (zxbparser.py p_type_def vs p_type_def_empty)
     * and gates Port beta's faithful suffix-vs-AS-type check. */
    bool had_as_clause = (type != NULL);

    /* DIM x AS type AT expr (memory-mapped variable) */
    AstNode *at_expr = NULL;
    if (match(p, BTOK_AT)) {
        at_expr = parse_expression(p, PREC_NONE + 1);
    }

    /* Check for initializer: = expr */
    AstNode *init_expr = NULL;
    if (match(p, BTOK_EQ)) {
        if (check(p, BTOK_LBRACE)) {
            init_expr = parse_array_initializer(p);
        } else {
            init_expr = parse_expression(p, PREC_NONE + 1);
        }
    }

    /* Multi-variable DIM is only valid as a bare `DIM a, b AS T`
     * (p_var_decl). The AT and initializer forms each require a single
     * variable; Python errors at p.lineno(1)=DIM and returns (p[0]=None,
     * no declaration), BEFORE the symboltable.declare suffix/strict checks
     * below — so return early here too:
     *   - p_var_decl_at  (zxbparser.py:662-664): `DIM idlist typedef AT expr`
     *     -> "Only one variable at a time can be declared this way".
     *   - p_var_decl_ini (zxbparser.py:693-695): `DIM idlist typedef EQ expr`
     *     -> "Initialized variables must be declared one by one." */
    return dim_build_scalar(p, names, name_count, name, type, had_as_clause,
                            at_expr, init_expr, is_const, lineno);
}

/* Scalar DIM/CONST declaration builder (extracted from parse_dim_statement so
 * the Phase-D PLY var_decl reduce-actions build the byte-for-byte same tree +
 * symbol-table state — C-vs-C identity by construction). Behaviour-preserving
 * extraction: takes the already-parsed pieces (names[], the resolved typedef,
 * whether an explicit AS clause was present, the AT and = initialiser exprs,
 * the CONST flag, and the DIM line) and performs the declare + tree build that
 * was inline. Faithful to p_var_decl / p_var_decl_at / p_var_decl_ini. */
static AstNode *dim_build_scalar(Parser *p, const char **names, int name_count,
                                 const char *name, TypeInfo *type,
                                 bool had_as_clause, AstNode *at_expr,
                                 AstNode *init_expr, bool is_const, int lineno) {
    if (name_count != 1) {
        if (at_expr) {
            zxbc_error(p->cs, lineno,
                       "Only one variable at a time can be declared this way");
            return NULL;
        }
        if (init_expr) {
            zxbc_error(p->cs, lineno,
                       "Initialized variables must be declared one by one.");
            return NULL;
        }
    }

    if (!type) {
        /* Check for deprecated suffix ($%&!) to infer type */
        size_t slen = strlen(name);
        if (slen > 0 && is_deprecated_suffix(name[slen - 1])) {
            BasicType bt = suffix_to_type(name[slen - 1]);
            type = p->cs->symbol_table->basic_types[bt];
        } else if (init_expr && init_expr->type_) {
            /* Implicit type with an initializer: take the initializer's
             * type — Python zxbparser.py:704-705
             * (if typedef.implicit: typedef = TYPEREF(expr.type_, ...)).
             * Without this, CONST/DIM with no AS clause defaulted to
             * float even for string/other initializers. */
            type = type_new_ref(p->cs, init_expr->type_, lineno, true);
        } else {
            type = type_new_ref(p->cs, p->cs->default_type, lineno, true);
        }
    }

    /* Port beta — deprecated-suffix vs explicit-AS-type mismatch.
     * Faithful port of symboltable.py declare (:104-109) + the
     * declare_variable 2nd message (:521-524), reachable on the C DIM
     * path (which calls symboltable_declare, not _declare_variable).
     * Python's declare first error fires iff: the id carries a
     * deprecated suffix AND entry.type_ (the typedef passed) is not
     * None and NOT implicit AND its type != the suffix type. The
     * declare_variable 2nd message then fires under the same
     * non-implicit-typedef + mismatch condition. An explicit "AS type"
     * clause is exactly Python's non-implicit typedef (had_as_clause);
     * a sigil-only DIM ("DIM a%", typedef implicit) or a matching
     * "DIM a% as Integer" must stay accepted (Python does). The two
     * message strings are byte-identical to the existing
     * symboltable_declare_variable port (compiler.c:237/240). */
    if (had_as_clause && type && !type->implicit) {
        size_t nl = strlen(name);
        if (nl > 0 && is_deprecated_suffix(name[nl - 1])) {
            BasicType suffix_bt = suffix_to_type(name[nl - 1]);
            BasicType decl_bt = typeref_basic(type);
            if (decl_bt != TYPE_unknown && decl_bt != suffix_bt) {
                zxbc_error(p->cs, lineno,
                           "expected type %s for '%s', got %s",
                           basictype_to_string(suffix_bt), name,
                           basictype_to_string(decl_bt));
                zxbc_error(p->cs, lineno,
                           "'%s' suffix is for type '%s' but it was declared as '%s'",
                           name, basictype_to_string(suffix_bt),
                           basictype_to_string(decl_bt));
            }
        }
    }

    /* Port gamma — strict-mode implicit-type redirect.
     * Faithful port of declare_variable (symboltable.py:531-532) ->
     * warning_implicit_type (errmsg.py:117-122) -> (under strict)
     * syntax_error_undeclared_type (errmsg.py:271-272). Python emits
     * the strict-mode error whenever the resolved entry type is
     * implicit and not unknown, under config.OPTIONS.strict — i.e.
     * BOTH the no-AS/no-init default-type case (already handled by the
     * removed special block) AND the implicit-typed-initializer case
     * ("DIM a = 5", the gap). A sigil-typed DIM yields a concrete
     * non-implicit type (Python's declare overrides entry.type_ to a
     * non-implicit TYPEREF) and an explicit AS clause is non-implicit
     * — neither trips it (Python accepts both under strict). We do NOT
     * emit the non-strict [W100] implicit-type warning here (that
     * pre-existing W-code-formatting gap is S3.x-owned); Port gamma is
     * confined to exactly Python's strict-mode reject. */
    if (p->cs->opts.strict && type && type->implicit &&
        typeref_basic(type) != TYPE_unknown) {
        for (int i = 0; i < name_count; i++)
            err_undeclared_type(p->cs, lineno, names[i]);
    }

    if (name_count == 1) {
        SymbolClass cls = is_const ? CLASS_const : CLASS_var;
        /* Strip deprecated suffix for symbol table key */
        const char *decl_name = name;
        size_t decl_nlen = strlen(name);
        char decl_stripped[256];
        if (decl_nlen > 0 && decl_nlen < sizeof(decl_stripped) && is_deprecated_suffix(name[decl_nlen - 1])) {
            memcpy(decl_stripped, name, decl_nlen - 1);
            decl_stripped[decl_nlen - 1] = '\0';
            decl_name = decl_stripped;
        }
        AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, decl_name, lineno, cls);
        /* Check for duplicate declaration */
        if (id_node->u.id.declared && id_node->lineno != lineno) {
            zxbc_error(p->cs, lineno, "Variable '%s' already declared at %s:%d",
                       name, p->cs->current_file, id_node->lineno);
        }
        /* Python declare_variable (symboltable.py:501-510): if a prior
         * forward reference (e.g. `@name` via p_addr_of_id -> access_id,
         * default_class=CLASS.unknown) created a CLASS.unknown entry,
         * declare_variable retrieves THE SAME entry and transitions it
         * via to_var/to_const (_id.py:115-141), which PRESERVES the
         * accessed flag (line 132/151). symboltable_declare here returns
         * the shared entry; without the class transition the entry stays
         * CLASS_unknown and the codegen data_ast filter
         * (class_==CLASS_var) drops it — losing the `_a:` DEFB row
         * (and the `_radians EQU _a` alias that traverse_const resolves
         * through it). The accessed/has_address flags set by the prior
         * @name access are already on this entry; we only need the class
         * promotion + declared=true. (Faithful to declare_variable:529.) */
        if (id_node->u.id.class_ == CLASS_unknown)
            id_node->u.id.class_ = cls;
        id_node->u.id.declared = true;
        id_node->lineno = lineno;
        id_node->type_ = type;
        /* Python ID.t delegates to the ref: a global var's VarRef.t is
         * entry.mangled (varref.py:36-37). symboltable_declare (the
         * generic creator) sets .mangled but not .t (only
         * symboltable_declare_variable does); set it here so a later
         * VAR rvalue / LET lvalue reads "_name", not "" — matching the
         * dedicated declarer and Python's VarRef.t. (Non-string scalar
         * DIM/CONST: the '$'-prefixed dynamic-string .t is S5.5+.) */
        if (id_node->t == NULL)
            id_node->t = id_node->u.id.mangled;
        /* Scalar DIM/CONST emits NO node at the statement site — Python
         * p_var_decl / p_var_decl_ini / p_var_decl_at all set p[0]=None
         * (zxbparser.py:652/689/657). The parsed init/AT value is NOT
         * dropped (CLAUDE.md rule 8): S5.3 added the data_ast drain, so
         * the init becomes entry.default_value and AT becomes entry.addr,
         * faithfully recovered by VarTranslator over data_ast. */
        AstNode *deferred_let = NULL;
        if (init_expr) {
            /* p_var_decl_ini (zxbparser.py:700-711):
             *   if not is_static(expr):
             *       if isinstance(expr, sym.UNARY):
             *           expr = make_constexpr(p.lineno(4), expr)
             *   value  = make_typecast(typedef, expr)
             *   defval = value if is_static(expr) and value.type_ != string
             *   declare_variable(..., default_value=defval)
             *
             * The UNARY→CONSTEXPR wrap (zxbparser.py:700-702) is what
             * makes a `DIM p = @a(0,0)` (UNARY ADDRESS of ARRAYACCESS) go
             * static + emit `DEFW (_a.__DATA__ + 0)` at compile time
             * instead of falling through to a delayed LET that emits a
             * runtime __ARRAY call. Mirror the wrap here so the
             * downstream is_static / default_value branch matches Python.
             * arrconst.bas covers exactly this shape. */
            AstNode *expr_for_default = init_expr;
            if (!check_is_static(init_expr) && init_expr->tag == AST_UNARY) {
                AstNode *ce = ast_new(p->cs, AST_CONSTEXPR, lineno);
                ast_add_child(p->cs, ce, init_expr);
                ce->type_ = init_expr->type_;
                ce->t = init_expr->t;
                expr_for_default = ce;
            }
            AstNode *value = make_typecast(p->cs, type, expr_for_default, lineno);
            bool stat = check_is_static(expr_for_default);
            bool is_str = value && type_is_string(value->type_);
            if (stat && !is_str) {
                id_node->u.id.default_value_expr = value;
            } else if (is_const) {
                /* p_var_decl_ini CONST branch (zxbparser.py:712-720):
                 *   if defval is None:                 # i.e. !(static && !string)
                 *       if not is_static_str(value):    # value.token != "STRING"
                 *           errmsg.syntax_error_not_constant(...); return
                 *       defval = value
                 *   declare_const(..., default_value=defval)
                 *
                 * A static-string CONST (`CONST a$ = "x"`) has its STRING
                 * value folded into the entry's default_value — NOT a
                 * deferred LET (consts have no runtime store). is_static_str
                 * is `value.token == "STRING"` (check.py:319-320), so the
                 * folded value must be the bare AST_STRING node; a const of
                 * any other non-static-non-numeric shape is a hard error
                 * (errmsg.py:217-218). Without this fold a string-CONST
                 * reference resolves to an empty ConstRef.t and the LET
                 * store falls through to a temporary (__STORE_STR2/pop de)
                 * instead of the static label load (__STORE_STR/ld de,LBL). */
                if (value && value->tag == AST_STRING) {
                    id_node->u.id.default_value_expr = value;
                } else {
                    err_not_constant(p->cs, lineno);
                    return NULL;
                }
            } else if (value) {
                /* p_var_decl_ini (zxbparser.py:722-728):
                 *   if defval is None:  # delayed initialization
                 *       p[0] = make_sentence("LET",
                 *                  SYMBOL_TABLE.access_var(idlist[0].name,
                 *                                          p.lineno(1)),
                 *                  value)
                 *
                 * The init is non-static (RND/USR/IN/function call) so it
                 * cannot be folded into entry.default_value; emit a LET
                 * statement instead. The optimizer's visit_LET
                 * (optimize.py:320-338, ported to passes/optimizer.c
                 * collect_side_effects) extracts RND/IN/USR/FUNCCALL
                 * side-effects when the lvalue is unused at O>1 so the
                 * RND call (and runtime asm) is preserved in
                 * `DIM a = int(rnd * 4)`.  String-typed init is also
                 * routed here (defval=None when value.type_==string)
                 * faithfully to Python.  CONST keyword has no LET path
                 * (Python errors at :716 if value isn't static_str). */
                AstNode *var = symboltable_access_var(
                                   p->cs->symbol_table, p->cs,
                                   decl_name, lineno, type);
                if (var) {
                    deferred_let = make_sentence_node(p, "LET", lineno);
                    ast_add_child(p->cs, deferred_let, var);
                    ast_add_child(p->cs, deferred_let, value);
                }
            }
        }
        if (at_expr) {
            /* p_var_decl_at (zxbparser.py:672-682) — the three-way branch:
             *
             *   if p[5].token == "CONSTEXPR":          # :672-674
             *       tmp = p[5].expr
             *       entry.addr = tmp                   # NOT typecast,
             *                                          # NOT mark_accessed
             *   elif not is_static(p[5]):              # :675-677
             *       errmsg.syntax_error_address_must_be_constant; return
             *   else:                                  # :679-682
             *       entry.addr = make_typecast(PTR_TYPE, p[5])
             *       mark_entry_as_accessed(entry)
             *       if entry.scope == local: make_static(entry.name)
             *
             * Crucially the CONSTEXPR branch (a `@label` / `@label+1`
             * address — p_addr_of_id:2685 / binary.make_node:113-116 wrap
             * those in CONSTEXPR; the C make_unary/make_binary_node mirror
             * this -> at_expr->tag == AST_CONSTEXPR) does NOT call
             * mark_entry_as_accessed, so an UNUSED `DIM x AT @label`
             * scalar is DCE-dropped at O>1 by VarTranslator.visit_VARDECL
             * exactly like Python (no spurious `_x EQU` line). The prior
             * unconditional accessed=true defeated that DCE gate. */
            TypeInfo *ptr_t =
                p->cs->symbol_table->basic_types[TYPE_uinteger]; /* gl.PTR_TYPE */
            if (at_expr->tag == AST_CONSTEXPR) {
                /* tmp = p[5].expr; entry.addr = tmp (the CONSTEXPR's
                 * inner node — UNARY ADDRESS / BINARY). No typecast, no
                 * accessed marking. VarTranslator resolves it through
                 * the recursive traverse_const (label -> .LABEL._name). */
                AstNode *inner = at_expr->child_count > 0
                                     ? at_expr->children[0]
                                     : at_expr;
                id_node->u.id.addr_expr = inner;
            } else if (!check_is_static(at_expr)) {
                /* elif not is_static(p[5]): address must be constant */
                err_address_must_be_constant(p->cs, lineno);
                return NULL;
            } else {
                /* else: static (NUMBER/CONST) address.
                 * entry.addr = make_typecast(PTR_TYPE, p[5]);
                 * mark_entry_as_accessed(entry).  (Python additionally
                 * does `if entry.scope == local: make_static(name)` here
                 * — like the array p_arr_decl_attr:785-786 path, the C
                 * port does not model make_static; the global corpus
                 * never hits the local branch, and not introducing an
                 * unported mechanism keeps this minimal/faithful.) */
                AstNode *av = make_typecast(p->cs, ptr_t, at_expr, lineno);
                id_node->u.id.addr_expr = av;
                id_node->u.id.accessed = true; /* mark_entry_as_accessed */
            }
        }
        return deferred_let;
    }

    /* Multiple vars. Symbols are registered regardless; a bare multi-var
     * DIM/CONST emits NO node (Python p_var_decl p[0]=None,
     * zxbparser.py:652) — same rule-8 reasoning as the single-name path.
     * (The multi-var grammar attaches no init/AT; the guard is defensive
     * and keeps the prior shape if one is ever present.) */
    SymbolClass cls = is_const ? CLASS_const : CLASS_var;
    AstNode *block = (!at_expr && !init_expr) ? NULL : make_block_node(p, lineno);
    for (int i = 0; i < name_count; i++) {
        /* Strip deprecated suffix for symbol table key */
        const char *mn = names[i];
        size_t ml = strlen(mn);
        char ms[256];
        if (ml > 0 && ml < sizeof(ms) && is_deprecated_suffix(mn[ml - 1])) {
            memcpy(ms, mn, ml - 1);
            ms[ml - 1] = '\0';
            mn = ms;
        }
        AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, mn, lineno, cls);
        id_node->type_ = type;
        if (block) {
            AstNode *decl = ast_new(p->cs, AST_VARDECL, lineno);
            ast_add_child(p->cs, decl, id_node);
            decl->type_ = type;
            ast_add_child(p->cs, block, decl);
        }
    }
    return block;
}

/* Array DIM declaration builder (extracted from parse_dim_statement so the
 * Phase-D array var_decl reduce-actions build the byte-for-byte same tree +
 * symbol-table state — C-vs-C identity by construction). Takes the parsed
 * pieces (name, bounds BOUNDLIST, resolved type, AT-address expr, init image,
 * DIM line). The caller runs check_bound + string-init checks first
 * (order-faithful). Faithful to p_decl_arr / p_arr_decl_attr /
 * p_arr_decl_initialized. Behaviour-preserving extraction. */
static AstNode *dim_build_array(Parser *p, const char *name, AstNode *bounds,
                               TypeInfo *type, AstNode *arr_at_expr,
                               AstNode *init, int lineno) {
        AstNode *decl = ast_new(p->cs, AST_ARRAYDECL, lineno);
        /* Strip deprecated suffix for symbol table key (a$ → a, matching access_id) */
        const char *dim_name = name;
        size_t dim_nlen = strlen(name);
        char dim_stripped[256];
        if (dim_nlen > 0 && dim_nlen < sizeof(dim_stripped) && is_deprecated_suffix(name[dim_nlen - 1])) {
            memcpy(dim_stripped, name, dim_nlen - 1);
            dim_stripped[dim_nlen - 1] = '\0';
            dim_name = dim_stripped;
        }
        AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, dim_name, lineno, CLASS_array);
        /* declare_array reconciliation (symboltable.py:677 -> to_vararray):
         * a pre-existing CLASS_unknown entry (e.g. forward `@a`) is converted
         * to the array here. */
        if (id_node->u.id.class_ == CLASS_unknown)
            id_node->u.id.class_ = CLASS_array;
        id_node->type_ = type;
        id_node->u.id.arr_boundlist = bounds;
        ast_add_child(p->cs, decl, id_node);
        ast_add_child(p->cs, decl, bounds);
        if (arr_at_expr) ast_add_child(p->cs, decl, arr_at_expr);
        if (init) ast_add_child(p->cs, decl, init);
        if (arr_at_expr) id_node->u.id.addr_expr = arr_at_expr;
        decl->type_ = type;
        if (arr_at_expr && id_node->u.id.scope == SCOPE_local) {
            id_node->u.id.scope = SCOPE_global;
        }
        if (id_node->u.id.scope == SCOPE_local) {
            id_node->u.id.arr_init = init;
            bool zb = true;
            for (int bi = 0; bi < bounds->child_count; bi++) {
                AstNode *bd = bounds->children[bi];
                AstNode *lo = (bd && bd->child_count > 0) ? bd->children[0]
                                                          : NULL;
                long lv = 0;
                if (lo && lo->tag == AST_NUMBER) lv = (long)lo->u.number.value;
                else if (lo && lo->tag == AST_CONSTEXPR &&
                         lo->child_count > 0 &&
                         lo->children[0]->tag == AST_NUMBER)
                    lv = (long)lo->children[0]->u.number.value;
                if (lv != 0) { zb = false; break; }
            }
            id_node->u.id.is_zero_based = zb;
        }
        return decl;
}

/* ----------------------------------------------------------------
 * PRINT statement
 * ---------------------------------------------------------------- */
static AstNode *parse_print_statement(Parser *p) {
    consume(p, BTOK_PRINT, "Expected PRINT");
    int lineno = p->previous.lineno;
    p->cs->print_is_used = true;

    AstNode *s = make_sentence_node(p, "PRINT", lineno);

    /* Faithful port of the Python print_list / print_elem grammar
     * (zxbparser.py:1978-2073):
     *
     *   print_list : print_elem                       (p_print_list_elem)
     *              | print_list SC    print_elem       (p_print_list)
     *              | print_list COMMA print_elem       (p_print_list_comma)
     *   print_elem : expr | print_at | print_tab | attr
     *              | BOLD expr | ITALIC expr | <empty>
     *
     * p_print_list_elem builds the PRINT SENTENCE and sets eol=True.
     * p_print_list (SC, i.e. ';')   : eol = (next print_elem is not None);
     *                                 NO separator child is appended;
     *                                 append the next print_elem iff present.
     * p_print_list_comma (',')      : eol = (next print_elem is not None);
     *                                 append a "PRINT_COMMA" SENTENCE child;
     *                                 append the next print_elem iff present.
     * A None print_elem (epsilon) is filtered out of the SENTENCE's
     * children (sentence.py:20) — i.e. it contributes no child.
     *
     * eol default is True (the fresh print_list); a trailing separator
     * with nothing after it (epsilon -> None) flips it False. */
    s->u.sentence.eol = true;

    /* parse_one_print_elem(): consumes a single print_elem and appends it
     * to `s` (the bare expr / PRINT_AT / PRINT_TAB / attr node — Python
     * makes the expr a *direct* child of the PRINT sentence, no wrapper).
     * Returns true if a (non-None) print_elem was consumed, false for the
     * epsilon production (nothing parsed). */
    for (;;) {
        bool produced = false;

        if (match(p, BTOK_AT)) {
            /* print_at : AT expr COMMA expr  (p_print_list_at) */
            AstNode *row = parse_expression(p, PREC_NONE + 1);
            consume(p, BTOK_COMMA, "Expected ',' after AT row");
            AstNode *col = parse_expression(p, PREC_NONE + 1);
            AstNode *at_sent = make_sentence_node(p, "PRINT_AT", lineno);
            if (row) ast_add_child(p->cs, at_sent, row);
            if (col) ast_add_child(p->cs, at_sent, col);
            ast_add_child(p->cs, s, at_sent);
            produced = true;
        } else if (match(p, BTOK_TAB)) {
            /* print_tab : TAB expr  (p_print_list_tab) */
            AstNode *col = parse_expression(p, PREC_NONE + 1);
            AstNode *tab_sent = make_sentence_node(p, "PRINT_TAB", lineno);
            if (col) ast_add_child(p->cs, tab_sent, col);
            ast_add_child(p->cs, s, tab_sent);
            produced = true;
        } else {
            /* attr : OVER|INVERSE|INK|PAPER|BRIGHT|FLASH expr  (p_attr,
             *   zxbparser.py:2055-2057) and BOLD expr | ITALIC expr
             *   (p_print_list_expr, zxbparser.py:2030-2036).
             * S7.1b-iii P-iv — EVERY in-PRINT attr (all 8) builds
             *   make_sentence(lineno, p[1] + "_TMP",
             *       make_typecast(TYPE.ubyte, p[2], lineno))
             * i.e. a SENTENCE whose kind is "<NAME>_TMP" with ONE child =
             * the operand expr wrapped in an unconditional ubyte typecast.
             * make_typecast returning NULL (None expr) collapses to a
             * 0-child SENTENCE — Python's make_sentence filters the None
             * child (sentence.py:20). Same make_typecast(p->cs, ub, …)
             * idiom as the p_print_elem_expr boolean case just below. */
            const char *attr_name = NULL;
            if (match(p, BTOK_INK))          attr_name = "INK_TMP";
            else if (match(p, BTOK_PAPER))   attr_name = "PAPER_TMP";
            else if (match(p, BTOK_BRIGHT))  attr_name = "BRIGHT_TMP";
            else if (match(p, BTOK_FLASH))   attr_name = "FLASH_TMP";
            else if (match(p, BTOK_OVER))    attr_name = "OVER_TMP";
            else if (match(p, BTOK_INVERSE)) attr_name = "INVERSE_TMP";
            else if (match(p, BTOK_BOLD))    attr_name = "BOLD_TMP";
            else if (match(p, BTOK_ITALIC))  attr_name = "ITALIC_TMP";
            if (attr_name) {
                int attr_lineno = p->previous.lineno;
                AstNode *val = parse_expression(p, PREC_NONE + 1);
                TypeInfo *ub =
                    p->cs->symbol_table->basic_types[TYPE_ubyte];
                AstNode *cast = make_typecast(p->cs, ub, val, attr_lineno);
                AstNode *attr_sent =
                    make_sentence_node(p, attr_name, attr_lineno);
                if (cast) ast_add_child(p->cs, attr_sent, cast);
                ast_add_child(p->cs, s, attr_sent);
                produced = true;
            } else if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) &&
                       !check(p, BTOK_CO) && !check(p, BTOK_SC) &&
                       !check(p, BTOK_COMMA)) {
                /* print_elem : expr  (p_print_elem_expr). The BARE expr is
                 * the PRINT sentence's child (no PRINT_ITEM wrapper —
                 * Python has no such node). If the expr is boolean it is
                 * make_typecast'd to ubyte exactly as
                 * p_print_elem_expr does. */
                AstNode *expr = parse_expression(p, PREC_NONE + 1);
                if (expr) {
                    const TypeInfo *et = expr->type_;
                    const TypeInfo *ef = (et && et->final_type)
                                         ? et->final_type : et;
                    if (ef && ef->basic_type == TYPE_boolean) {
                        TypeInfo *ub =
                            p->cs->symbol_table->basic_types[TYPE_ubyte];
                        AstNode *cast = make_typecast(p->cs, ub, expr,
                                                      expr->lineno);
                        if (cast) expr = cast;
                    }
                    ast_add_child(p->cs, s, expr);
                    produced = true;
                }
            }
            /* else: epsilon print_elem -> None (produced stays false). */
        }

        /* Separator handling (p_print_list / p_print_list_comma): the
         * separator's eol = (the print_elem that follows it is not None).
         * `;` (SC) appends NO child; `,` (COMMA) appends a PRINT_COMMA
         * SENTENCE child. Loop continues only while a separator is seen. */
        if (match(p, BTOK_SC)) {
            bool next_present = !check(p, BTOK_NEWLINE) &&
                                !check(p, BTOK_EOF) && !check(p, BTOK_CO);
            s->u.sentence.eol = next_present;
            continue;
        }
        if (match(p, BTOK_COMMA)) {
            bool next_present = !check(p, BTOK_NEWLINE) &&
                                !check(p, BTOK_EOF) && !check(p, BTOK_CO);
            s->u.sentence.eol = next_present;
            AstNode *sep = make_sentence_node(p, "PRINT_COMMA", lineno);
            ast_add_child(p->cs, s, sep);
            continue;
        }

        /* No separator: the print_list is complete. If the first
         * print_elem was epsilon and there is nothing at all (bare
         * `PRINT`), Python still built the SENTENCE with eol=True (the
         * None child filtered out) — that is the s/eol default already.
         *
         * `produced` distinguishes "consumed a real print_elem" from the
         * epsilon production; the SENTENCE/eol bookkeeping above already
         * handled both, so the list simply terminates here. */
        (void)produced;
        break;
    }

    /* D1 — PRINT statement node lineno (zxbparser.py:2038): Python builds
     * the PRINT SENTENCE in p_print_list_elem with `make_sentence(
     * p.lexer.lineno, "PRINT", ...)` — the LEXER's current line at the
     * `print_list : print_elem` reduction, NOT the PRINT keyword's line.
     * For a PRINT terminated by NEWLINE the lexer has already consumed the
     * NEWLINE, so this is source_line+1; for a PRINT followed by a same-line
     * `:` (CO) the lexer is still on that line, so it stays source_line.
     * The C lexer increments lineno before emitting NEWLINE (lexer.c:765),
     * so the lookahead token `p->current` here carries exactly that value:
     * NEWLINE -> source_line+1, CO -> source_line, EOF -> last line. Adopt
     * it so warning_unreachable_code / warning_function_should_return
     * (optimize.py:146 / the FUNCTION-end check), which read node.lineno,
     * report Python's line. The PRINT children keep their own (natural)
     * linenos; only the statement node's is the lexer-position value.
     * Codegen is unaffected — staged byte-identity is GREEN with either
     * value, i.e. the PRINT node lineno is not in the byte stream. */
    s->lineno = p->current.lineno;

    return s;
}

/* ----------------------------------------------------------------
 * FUNCTION / SUB declaration
 * ---------------------------------------------------------------- */

/* S5.7b — PARAMLIST.append_child cumulative offset/size accumulator
 * (paramlist.py:53-58) combined with VarRef.size (varref.py:45-57):
 *
 *   for each param appended left-to-right:
 *       param.ref.offset = self.size
 *       self.size       += param.size
 *
 *   VarRef.size for a SCOPE.parameter:
 *       byref  -> BasicType(gl.PTR_TYPE).size           (== 2 on zx48k)
 *       byval  -> type_.size + (type_.size % PARAM_ALIGN) (PARAM_ALIGN==2)
 *
 * Array params are always byref (parser.c:2486 sets byref=true for
 * `name()`), so they fall in the byref arm → PTR_TYPE.size. gl.PTR_TYPE
 * == uinteger (size 2), gl.PARAM_ALIGN == 2 (src/arch/zx48k/__init__.py).
 *
 * Returns the running total (== SymbolPARAMLIST.size, i.e. the byte count
 * ic_leave pops and the caller pushes), and stamps each ARGUMENT child's
 * cumulative .offset. Pure size/offset bookkeeping over the just-parsed
 * PARAMLIST — no AST shape change, faithful to the Python accumulator. */
static int parser_assign_param_offsets(Parser *p, AstNode *params) {
    (void)p;
    const int PTR_SIZE = 2;    /* BasicType(gl.PTR_TYPE).size, zx48k */
    const int PARAM_ALIGN = 2; /* gl.PARAM_ALIGN, zx48k */
    int size = 0;
    if (!params)
        return 0;
    for (int i = 0; i < params->child_count; i++) {
        AstNode *param = params->children[i];
        if (!param || param->tag != AST_ARGUMENT)
            continue;
        int psz;
        if (param->u.argument.byref || param->u.argument.is_array) {
            psz = PTR_SIZE;
        } else if (param->type_ == NULL) {
            psz = 0; /* varref.py:46-47 — type_ is None -> size 0 */
        } else {
            int ts = type_size(param->type_);
            psz = ts + (ts % PARAM_ALIGN); /* even-size (Float and Byte) */
        }
        param->u.argument.offset = size;
        size += psz;
    }
    return size;
}

static AstNode *parse_sub_or_func_decl(Parser *p, bool is_function, bool is_declare) {
    if (is_function) {
        consume(p, BTOK_FUNCTION, "Expected FUNCTION");
    } else {
        consume(p, BTOK_SUB, "Expected SUB");
    }
    int lineno = p->previous.lineno;

    /* Python's p_function_header_pre sets p[0]=None and returns on a
     * rejected header (type mismatch :2966, parameter mismatch :2976/2988,
     * SUB-with-return-type :2994). p_funcdecl then short-circuits
     * (`if p[1] is None: return`, zxbparser.py:2905) and NEVER calls
     * leave_scope() — so NO W150 unused-parameter warning is emitted for a
     * function whose header was rejected. The C continues parsing the body
     * (by design — see the reject-site comments), so we must remember the
     * rejection and suppress the leave-scope W150 emission to match. */
    bool header_rejected = false;

    /* Optional calling convention */
    Convention conv = CONV_unknown;
    if (match(p, BTOK_FASTCALL)) conv = CONV_fastcall;
    else if (match(p, BTOK_STDCALL)) conv = CONV_stdcall;

    /* Function name (may be a keyword used as identifier) */
    if (!is_name_token(p)) {
        parser_error(p, "Expected function/sub name");
        return NULL;
    }
    const char *func_name_raw = get_name_token(p);
    advance(p);

    /* Strip deprecated suffix for symbol table key (matching access_id) */
    const char *func_name = func_name_raw;
    size_t fn_len = strlen(func_name_raw);
    char fn_stripped[256];
    TypeInfo *fn_suffix_type = NULL;
    if (fn_len > 0 && fn_len < sizeof(fn_stripped) && is_deprecated_suffix(func_name_raw[fn_len - 1])) {
        BasicType bt = suffix_to_type(func_name_raw[fn_len - 1]);
        fn_suffix_type = p->cs->symbol_table->basic_types[bt];
        memcpy(fn_stripped, func_name_raw, fn_len - 1);
        fn_stripped[fn_len - 1] = '\0';
        func_name = fn_stripped;
    }

    /* Parameters */
    AstNode *params = ast_new(p->cs, AST_PARAMLIST, lineno);
    bool had_optional_param = false;
    /* Params declared explicitly ByVal as arrays are REJECTED from the
     * PARAMLIST (zxbparser.py:3089-3093 sets p[0]=None), but Python's
     * declare_param has ALREADY registered the symbol in the function
     * scope — so its leave_scope "[W150] Parameter '<id>' is never used"
     * warning still fires. Collect them here and register them in the
     * body scope below (after enter_scope), WITHOUT adding them to the
     * PARAMLIST that check_call_arguments counts. Tiny count in practice. */
    AstNode *dropped_byval_array[16];
    int dropped_byval_array_n = 0;
    if (match(p, BTOK_LP)) {
        if (!check(p, BTOK_RP)) {
            do {
                bool byref = p->cs->opts.default_byref;
                /* Track whether BYVAL was given explicitly: an array
                 * parameter passed explicitly ByVal is rejected
                 * (zxbparser.py:3084-3094 p_param_byval_definition). */
                bool explicit_byval = false;
                int byval_line = lineno;
                if (match(p, BTOK_BYREF)) byref = true;
                else if (match(p, BTOK_BYVAL)) {
                    byref = false;
                    explicit_byval = true;
                    byval_line = p->previous.lineno;
                }

                if (!is_name_token(p)) {
                    parser_error(p, "Expected parameter name");
                    break;
                }
                const char *param_name = get_name_token(p);
                advance(p);
                int param_line = p->previous.lineno;

                /* Check for array parameter: name() */
                bool is_array = false;
                if (match(p, BTOK_LP)) {
                    consume(p, BTOK_RP, "Expected ')' for array parameter");
                    is_array = true;
                    byref = true; /* arrays always byref */
                }

                /* p_param_byval_definition (zxbparser.py:3089-3093): an
                 * array parameter cannot be passed ByVal — emit the
                 * "Array parameter '<id>' must be passed ByRef" error at
                 * the BYVAL token's line and DROP this param (Python sets
                 * p[0] = None, so the parameter is not appended to the
                 * PARAMLIST). The remaining call-argument checks then see
                 * the reduced parameter count exactly as Python does. */
                if (is_array && explicit_byval) {
                    err_cannot_pass_array_by_value(p->cs, byval_line,
                                                   param_name);
                    /* Consume this param's typedef so the loop resyncs at
                     * the next ',' / ')' (the do-while's match(COMMA)
                     * advances to the following param). The param is
                     * dropped from the PARAMLIST — exactly as Python's
                     * p[0] = None — but a node is still built and remembered
                     * so it can be registered in the body scope, where its
                     * "[W150] never used" warning fires (Python's
                     * declare_param ran before the byval rejection). */
                    TypeInfo *dtype = parse_typedef(p);
                    if (!dtype)
                        dtype = type_new_ref(p->cs, p->cs->default_type,
                                             param_line, true);
                    if (dropped_byval_array_n <
                        (int)(sizeof(dropped_byval_array) /
                              sizeof(dropped_byval_array[0]))) {
                        AstNode *dp = ast_new(p->cs, AST_ARGUMENT,
                                              param_line);
                        dp->u.argument.name =
                            arena_strdup(&p->cs->arena, param_name);
                        dp->u.argument.byref = false;
                        dp->u.argument.is_array = true;
                        dp->type_ = dtype;
                        dropped_byval_array[dropped_byval_array_n++] = dp;
                    }
                    continue;
                }

                TypeInfo *param_type = parse_typedef(p);
                if (!param_type) {
                    param_type = type_new_ref(p->cs, p->cs->default_type, param_line, true);
                    if (p->cs->opts.strict)
                        zxbc_error(p->cs, param_line, "strict mode: missing type declaration for '%s'", param_name);
                }

                /* An array parameter has NO default-value production:
                 * `param_def : singleid LP RP typedef` (zxbparser.py:3108)
                 * vs the scalar `singleid typedef default_arg_value` (:3122).
                 * A '=' after an array param is therefore a grammar error —
                 * PLY raises p_error "Syntax Error. Unexpected token '=' <EQ>"
                 * at the '=' and recovers via `param_decl : LP error RP`
                 * (:3057, param list -> None). Reproduce: diagnose at the '='
                 * line, consume to the closing ')', and drop this param
                 * (err_default_array_arg). */
                if (is_array && check(p, BTOK_EQ)) {
                    zxbc_error(p->cs, p->current.lineno,
                               "Syntax Error. Unexpected token '=' <EQ>");
                    while (!check(p, BTOK_EOF) && !check(p, BTOK_NEWLINE) &&
                           !check(p, BTOK_CO) && !check(p, BTOK_RP))
                        advance(p);
                    break;
                }

                /* Default value */
                AstNode *default_val = NULL;
                if (match(p, BTOK_EQ)) {
                    default_val = parse_expression(p, PREC_NONE + 1);
                    had_optional_param = true;
                } else if (had_optional_param) {
                    zxbc_error(p->cs, param_line,
                               "Can't declare mandatory param '%s' after optional param", param_name);
                }

                /* A deprecated sigil overrides the declared param type at
                 * DECLARE time (SymbolTable.declare, symboltable.py:105-
                 * 123): a$ => string, i% => integer, etc. Python resolves
                 * this inside declare_param BEFORE PARAMLIST.append_child
                 * accumulates `param.size` (paramlist.py:53-58), so the
                 * sigil type must be on the node before
                 * parser_assign_param_offsets runs — otherwise a `s$`
                 * param sizes as the implicit float default (5 → psz 6)
                 * instead of string (2 → psz 2). The body-scope
                 * registration block below repeats this resolution; with
                 * the type already corrected here it is a consistent
                 * no-op. The explicit-vs-sigil conflict diagnostic
                 * (symboltable.py:108-109) is emitted HERE against the
                 * ORIGINAL declared type — once it is overridden below the
                 * body-scope block can no longer see the conflict. */
                {
                    size_t pnl = strlen(param_name);
                    if (pnl > 0 &&
                        is_deprecated_suffix(param_name[pnl - 1])) {
                        BasicType sbt =
                            suffix_to_type(param_name[pnl - 1]);
                        TypeInfo *sti =
                            p->cs->symbol_table->basic_types[sbt];
                        /* SymbolTable.add (symboltable.py:108-109): if the
                         * entry already carries a NON-implicit declared type
                         * that differs from the sigil type, raise
                         * "expected type <sigil> for '<id$>', got <decl>" at
                         * the param's lineno. Python does NOT abort — it
                         * keeps going and adopts the sigil type (line 123).
                         * Mirror exactly: emit against the original
                         * param_type, then override. */
                        if (param_type && !param_type->implicit &&
                            !type_equal(param_type, sti)) {
                            zxbc_error(p->cs, param_line,
                                       "expected type %s for '%s', got %s",
                                       sti->name, param_name,
                                       param_type->name);
                        }
                        param_type = sti;
                    }
                }

                AstNode *param_node = ast_new(p->cs, AST_ARGUMENT, param_line);
                param_node->u.argument.name = arena_strdup(&p->cs->arena, param_name);
                param_node->u.argument.byref = byref;
                param_node->u.argument.is_array = is_array;
                param_node->type_ = param_type;
                /* Port D — p_param_def_type (zxbparser.py:3129):
                 *   default_value = make_typecast(typedef, p[3],
                 *                                  id_.lineno)
                 * Route the parsed default through the already-ported
                 * make_typecast so a string default for a numeric param
                 * raises "Cannot convert string to a value. Use VAL()
                 * function" (compiler.c:596) at the param-name line,
                 * exactly as Python (gates optional_param3's first
                 * error).  make_typecast is a no-op when types match /
                 * node->type_ is NULL, so this never rejects anything
                 * Python's make_node would not. */
                if (default_val)
                    default_val = make_typecast(p->cs, param_type,
                                                default_val, param_line);
                if (default_val) ast_add_child(p->cs, param_node, default_val);
                ast_add_child(p->cs, params, param_node);
            } while (match(p, BTOK_COMMA));
        }
        consume(p, BTOK_RP, "Expected ')' after parameters");
    }

    /* Return type (FUNCTION, or SUB with AS = error but parseable) */
    TypeInfo *ret_type = NULL;
    if (is_function || check(p, BTOK_AS)) {
        ret_type = parse_typedef(p);
        if (!ret_type && is_function) {
            /* Use suffix type if available (e.g. test$ → string) */
            if (fn_suffix_type) {
                ret_type = fn_suffix_type;
            } else {
                ret_type = type_new_ref(p->cs, p->cs->default_type, lineno, true);
                if (p->cs->opts.strict)
                    zxbc_error(p->cs, lineno, "strict mode: missing type declaration for '%s'", func_name);
            }
        }
        /* p_function_def_header (zxbparser.py:2994-2995): a SUB given a
         * non-implicit (explicit `AS <type>`) return type is rejected with
         * "SUBs cannot have a return type definition" at the SUB line. An
         * explicit AS clause yields a non-implicit TYPEREF; a bare SUB has
         * no AS branch (ret_type stays NULL) and never trips this. Python
         * sets p[0]=None but the body still parses, so just diagnose and
         * continue (the SUB keeps parsing; the call site sees 0 params). */
        if (!is_function && ret_type && !ret_type->implicit) {
            zxbc_error(p->cs, lineno,
                       "SUBs cannot have a return type definition");
            header_rejected = true;  /* Python p[0]=None — no leave_scope/W150 */
        }
    }

    /* Declare function/sub in the CURRENT (parent) scope BEFORE entering body scope.
     * This enables recursive calls — the function name is visible from inside.
     *
     * Python declare_func (symboltable.py:727-743) does get_entry(id_) —
     * a search of ALL scopes (symboltable.py:82) — not just the current
     * one. If the entry already exists (e.g. it was forward-CALLED, so
     * access_func implicitly declared it CLASS_unknown and leave_scope
     * relocated it to global), declare_func REUSES that same object and
     * re-mangles it `f"{current_namespace}_{entry.name}"` — a literal
     * '_' join (symboltable.py:741), NOT make_child_namespace's '.'
     * join. Only a genuinely fresh function takes the else branch
     * (symboltable.py:743 -> declare -> make_child_namespace, the '.'
     * form). C symboltable_declare only checks the CURRENT scope, so a
     * relocated-to-global forward entry was missed: a duplicate `_p.r`
     * entry got built instead of reusing the global `_p_r` one
     * (paramstr5). Mirror get_entry's cross-scope search here. */
    SymbolClass cls = is_function ? CLASS_function : CLASS_sub;
    AstNode *pre_existing =
        symboltable_lookup(p->cs->symbol_table, func_name);
    AstNode *id_node;
    if (pre_existing) {
        /* declare_func "entry is not None" branch (symboltable.py:728-
         * 741): reuse the shared object; CLASS_unknown -> to_function;
         * mangled = "{current_namespace}_{name}" (literal '_' join). */
        id_node = pre_existing;
        if (id_node->u.id.class_ == CLASS_unknown)
            id_node->u.id.class_ = cls; /* entry.to_function(class_) */
        const char *ns = p->cs->symbol_table->current_scope->namespace_;
        if (!ns) ns = "";
        size_t nl = strlen(func_name), sl = strlen(ns);
        char *m = arena_alloc(&p->cs->arena, sl + 1 + nl + 1);
        memcpy(m, ns, sl);
        m[sl] = '_';
        memcpy(m + sl + 1, func_name, nl + 1);
        id_node->u.id.mangled = m;
    } else {
        /* declare_func else branch (symboltable.py:743): fresh declare
         * -> make_child_namespace(current_namespace, name) ('.' join);
         * for global scope (ns "") that is "_name", unchanged. */
        id_node = symboltable_declare(p->cs->symbol_table, p->cs,
                                      func_name, lineno, cls);
    }

    /* Check for duplicate definition or class mismatch */
    if (id_node->u.id.declared && id_node->lineno != lineno) {
        if (is_declare) {
            /* Duplicate DECLARE — always an error */
            zxbc_error(p->cs, lineno, "duplicated declaration for %s '%s'",
                       symbolclass_to_string(cls), func_name);
        } else if (id_node->u.id.class_ == CLASS_function || id_node->u.id.class_ == CLASS_sub) {
            if (!id_node->u.id.forwarded) {
                /* Already fully defined — duplicate */
                zxbc_error(p->cs, lineno, "Duplicate function name '%s', previously defined at %d",
                           func_name, id_node->lineno);
            }
        }
    }
    if (id_node->u.id.class_ != CLASS_unknown && id_node->u.id.class_ != cls) {
        zxbc_error(p->cs, lineno, "'%s' is a %s, not a %s", func_name,
                   symbolclass_to_string(id_node->u.id.class_),
                   symbolclass_to_string(cls));
    }

    /* Check type mismatch between forward declaration and full definition.
     * Matches Python zxbparser.py line 2961: if the new type is implicit and
     * the forward declaration already has a known type, keep the declaration's type. */
    if (!is_declare && id_node->u.id.forwarded && ret_type && id_node->type_) {
        bool new_is_implicit = (ret_type->tag == AST_TYPEREF && ret_type->implicit);
        if (new_is_implicit && id_node->type_->basic_type != TYPE_unknown) {
            /* Keep forward declaration's type — don't override with implicit */
            ret_type = id_node->type_;
        } else if (!new_is_implicit && !type_equal(id_node->type_, ret_type)) {
            zxbc_error(p->cs, lineno, "Function '%s' (previously declared at %d) type mismatch",
                       func_name, id_node->lineno);
            header_rejected = true;  /* Python p[0]=None — no leave_scope/W150 */
        }
    }

    id_node->u.id.class_ = cls;
    id_node->u.id.convention = conv;
    id_node->type_ = ret_type;

    /* S5.7b — cumulative param offsets + total params byte size
     * (paramlist.py:53-58 / varref.py:45-57). entry.param_size is what
     * the optimizer's O>1 zero-param/zero-local→fastcall gate checks
     * (optimizer.c:476, optimize.py:314-315) and what ic_leave pops
     * (function_translator.py:169). Stamp it on the shared entry node so
     * both the DECLARE and the definition carry the same value. Python's
     * function_header_pre (zxbparser.py:2957) sets params_size = p[2].size
     * for declare and definition alike. */
    id_node->u.id.param_size = parser_assign_param_offsets(p, params);

    /* S5.10a — stamp the callee PARAMLIST on the shared function ID
     * (Python entry.ref.params, set by function_header_pre for declare
     * and definition alike, zxbparser.py:2957). The definition's
     * PARAMLIST overwrites the declare's on the same entry, exactly as
     * Python's funcref.params does. check_call_arguments reads this
     * stable handle instead of id_node->parent (which a later call
     * node's ast_add_child re-parents). */
    id_node->u.id.params = params;

    if (is_declare) {
        /* Forward declaration — no body to parse */
        id_node->u.id.forwarded = true;

        /* Create a minimal FUNCDECL node with empty body */
        AstNode *decl = ast_new(p->cs, AST_FUNCDECL, lineno);
        /* S5.7b — mark as the C-port-only forward-declaration FUNCDECL.
         * Python's p_funcdeclforward (zxbparser.py:2918-2930) returns None
         * → no node enters the AST/gl.FUNCTIONS for a DECLARE; only the
         * real definition's FUNCDECL is emitted. tr_visit_funcdecl uses
         * this flag to NOT enqueue the declare, so the C pending queue is
         * the exact set Python's is (the definition only). */
        decl->u.funcdecl.is_forward = true;
        AstNode *body = make_block_node(p, lineno);
        ast_add_child(p->cs, decl, id_node);
        ast_add_child(p->cs, decl, params);
        ast_add_child(p->cs, decl, body);
        decl->type_ = ret_type;

        vec_push(p->cs->functions, id_node);
        return decl;
    }

    /* Check parameter mismatch between forward declaration and full definition.
     * Matches Python zxbparser.py lines 2971-2990. */
    if (id_node->u.id.forwarded && id_node->parent && id_node->parent->tag == AST_FUNCDECL
        && id_node->parent->child_count >= 2) {
        AstNode *old_params = id_node->parent->children[1];  /* DECLARE's param list */
        if (old_params && params) {
            if (old_params->child_count != params->child_count) {
                /* Python syntax_error_parameter_mismatch (errmsg.py:255):
                 * "Function '%s' (previously declared at %d) parameter
                 * mismatch" — use the existing faithful helper, not the
                 * divergent inline literal. */
                err_parameter_mismatch(p->cs, lineno, func_name, id_node->lineno);
                header_rejected = true;  /* Python p[0]=None — no leave_scope/W150 */
            } else {
                for (int pi = 0; pi < old_params->child_count; pi++) {
                    AstNode *a = old_params->children[pi];
                    AstNode *b = params->children[pi];
                    if (a && b) {
                        /* Check type and byref match */
                        bool type_mismatch = false;
                        if (a->type_ && b->type_ && !type_equal(a->type_, b->type_)) {
                            type_mismatch = true;
                        }
                        bool byref_mismatch = (a->u.argument.byref != b->u.argument.byref);
                        if (type_mismatch || byref_mismatch) {
                            /* Python syntax_error_parameter_mismatch
                             * (errmsg.py:255) — existing faithful helper. */
                            err_parameter_mismatch(p->cs, lineno, func_name, id_node->lineno);
                            header_rejected = true;  /* Python p[0]=None — no leave_scope/W150 */
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Full definition — clear forwarded flag if this was previously declared */
    id_node->u.id.forwarded = false;

    /* Enter function body scope. Python p_function_def (zxbparser.py:
     * 3025) calls enter_scope(name) with p[3] — the RAW function name
     * (suffix NOT stripped; only declare() strips it for entry.name/
     * .mangled). The namespace prefix therefore uses func_name_raw. */
    symboltable_enter_scope(p->cs->symbol_table, p->cs, func_name_raw);

    /* Register parameters in the function scope (so body can reference them).
     * Strip deprecated suffixes ($%&!) so lookups match (Python strips on get_entry). */
    for (int i = 0; i < params->child_count; i++) {
        AstNode *param = params->children[i];
        const char *pname = param->u.argument.name;
        /* Strip deprecated suffix if present; remember it for type inference */
        size_t plen = strlen(pname);
        char stripped[256];
        char psuffix = '\0';
        if (plen > 0 && plen < sizeof(stripped) && is_deprecated_suffix(pname[plen - 1])) {
            psuffix = pname[plen - 1];
            memcpy(stripped, pname, plen - 1);
            stripped[plen - 1] = '\0';
            pname = stripped;
        }
        /* A deprecated sigil overrides the declared param type (Python
         * SymbolTable.declare, symboltable.py:102-117): a$ => string,
         * i% => integer, etc. The DIM and function-name paths already do
         * this; the param-registration path previously did not, leaving
         * sigil params at the float default. */
        TypeInfo *ptype = param->type_;
        if (psuffix != '\0') {
            TypeInfo *suffix_ti =
                p->cs->symbol_table->basic_types[suffix_to_type(psuffix)];
            if (param->type_ && !param->type_->implicit &&
                !type_equal(param->type_, suffix_ti)) {
                zxbc_error(p->cs, param->lineno,
                           "expected type %s for '%s', got %s",
                           suffix_ti->name, param->u.argument.name,
                           param->type_->name);
            }
            ptype = suffix_ti;
            param->type_ = suffix_ti; /* keep the PARAMLIST node consistent */
        }
        SymbolClass pcls = param->u.argument.is_array ? CLASS_array : CLASS_var;
        AstNode *sym = symboltable_declare(p->cs->symbol_table, p->cs, pname, param->lineno, pcls);
        if (sym) {
            sym->type_ = ptype;
            sym->u.id.declared = true;
            /* S5.7d — Python's declare_param sets entry.scope =
             * SCOPE.parameter (symboltable.py:652). The C generic
             * symboltable_declare stamps SCOPE_local (level>0); correct it
             * so compute_offsets skips it (offset comes from the PARAMLIST,
             * not the local frame) and the :58-116 walk's param-skip and
             * visit_VAR's +offset (vs local -offset) fire faithfully. */
            sym->u.id.scope = SCOPE_parameter;
            /* In Python the param symbol IS the PARAMLIST child, so
             * PARAMLIST.append_child's `param.ref.offset = self.size`
             * (paramlist.py:53-58) lands on the very symbol visit_VAR
             * reads. The C port builds separate ARGUMENT + body-scope-ID
             * nodes, so copy the cumulative offset (already computed by
             * parser_assign_param_offsets into u.argument.offset) onto the
             * body symbol. byref propagates too: entry_size uses it
             * (varref.py:52-53 -> PTR) and visit_VAR's `*` indirection. */
            sym->u.id.offset = param->u.argument.offset;
            sym->u.id.offset_set = true;
            sym->u.id.byref = param->u.argument.byref;
        }
    }

    /* Register byval-array-rejected params in the body scope so their
     * "[W150] Parameter '<id>' is never used" warning fires at
     * leave_scope (Python declare_param ran before the byval rejection,
     * so the symbol exists in scope even though it was dropped from the
     * PARAMLIST). They are NOT in `params`, so check_call_arguments still
     * sees the reduced parameter count. Suffix handling mirrors the loop
     * above; these are scope-only entries (no PARAMLIST offset). */
    for (int i = 0; i < dropped_byval_array_n; i++) {
        AstNode *dp = dropped_byval_array[i];
        const char *pname = dp->u.argument.name;
        size_t plen = strlen(pname);
        char stripped[256];
        if (plen > 0 && plen < sizeof(stripped) &&
            is_deprecated_suffix(pname[plen - 1])) {
            memcpy(stripped, pname, plen - 1);
            stripped[plen - 1] = '\0';
            pname = stripped;
        }
        AstNode *sym = symboltable_declare(p->cs->symbol_table, p->cs,
                                           pname, dp->lineno, CLASS_array);
        if (sym) {
            sym->type_ = dp->type_;
            sym->u.id.declared = true;
            sym->u.id.scope = SCOPE_parameter;
            sym->u.id.byref = false;
        }
    }

    /* Note: function name is NOT re-declared in the body scope.
     * The parent scope CLASS_function entry is visible via scope chain lookup,
     * enabling recursive calls. Return value assignment (funcname = expr) works
     * because Python's LET handler recognizes CLASS_function as valid LHS. */

    /* Push function level (for GOSUB check and other function-scope tracking) */
    vec_push(p->cs->function_level, id_node);

    /* The function header must be terminated by a statement separator:
     * `function_header : function_header_pre CO | function_header_pre NEWLINE`
     * (zxbparser.py:2934-2935). A header that runs directly into the body
     * (or END) with no CO/NEWLINE is a PLY p_error on that token — e.g.
     * `FUNCTION f() END FUNCTION` on one line -> ":2: Unexpected token
     * 'END' <END>" (verified on the oracle, both FUNCTION and SUB). The
     * header is fully parsed at this point (params + return typedef); the
     * only valid continuations are CO or NEWLINE. */
    if (!check(p, BTOK_CO) && !check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF)) {
        tok_unexpected_error(p, &p->current);
        /* Pop the function level we just pushed and restore the parent
         * scope so the outer parser stays balanced. symboltable_exit_scope
         * does NOT emit W150 (those come later, on the local_symbol_table
         * capture which we skip), matching Python: the one-line header
         * error emits only the p_error line, no unused-param warnings
         * (verified on the oracle). had_error is set -> parse rejected. */
        if (p->cs->function_level.len > 0)
            vec_pop(p->cs->function_level);
        symboltable_exit_scope(p->cs->symbol_table);
        return NULL;
    }
    /* Parse body */
    skip_newlines(p);
    /* p_function_header_pre (src/zxbc/zxbparser.py:3002-3004): a FASTCALL
     * SUB/FUNCTION declared with more than one parameter -> W160.
     * Python's lineno is p.lineno(3) — the implicit-typedef position,
     * which for a header with no explicit return type resolves (via PLY's
     * epsilon-production lookahead) to the first token after the header
     * NEWLINE, i.e. the line parsing resumes on (p->current here, after
     * skip_newlines).  Uses the suffix-stripped entry name and the
     * SUB/FUNCTION class, matching FUNCTION_LEVEL[-1].class_. */
    if (conv == CONV_fastcall && params->child_count > 1) {
        warn_fastcall_n_params(p->cs, p->current.lineno,
                               is_function ? "FUNCTION" : "SUB",
                               func_name, params->child_count);
    }
    AstNode *body = make_block_node(p, lineno);
    while (!check(p, BTOK_EOF)) {
        /* Check for END FUNCTION / END SUB */
        if (check(p, BTOK_END)) {
            int save_pos = p->lexer.pos;
            BToken save_cur = p->current;
            advance(p);
            if ((is_function && check(p, BTOK_FUNCTION)) ||
                (!is_function && check(p, BTOK_SUB))) {
                advance(p);
                break;
            }
            /* Not END FUNCTION/SUB — parse as regular END statement */
            p->current = save_cur;
            p->lexer.pos = save_pos;
        }

        AstNode *stmt = parse_statement(p);
        /* Python builds the function body via program_co/make_block, which
         * skips null/NOP children (block.py:43-51 / is_null). The loop
         * previously appended NOPs (e.g. the empty statement from an inline
         * `FUNCTION f(): END FUNCTION` `:` separator), giving the body a
         * spurious NOP child the grammar/PLY-engine path does not — the only
         * divergence on def_func_inline_ok/lvalue02. Skip NOPs so the body
         * matches Python (and the engine) exactly; a NOP emits nothing, so
         * output is unchanged (byte-clean). */
        if (stmt && stmt->tag != AST_NOP) ast_add_child(p->cs, body, stmt);
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
    }

    /* Pop function level */
    if (p->cs->function_level.len > 0)
        vec_pop(p->cs->function_level);

    /* S5.7d — capture the body scope's insertion-ordered entries onto the
     * function ID (Python func.ref.local_symbol_table = current_scope,
     * zxbparser.py:2910, taken BEFORE leave_scope pops the table) and run
     * compute_offsets (symboltable.py:283) to assign per-local +/- IX
     * offsets and the total locals_size (the ic_enter operand). The
     * forward-DECLARE path returns early above and never reaches here, so
     * (like Python's leave_scope(show_warnings=False) discarding the
     * return at zxbparser.py:2929) a declare computes no sizes. */
    /* SymbolTable.leave_scope (symboltable.py:268-277): every parameter
     * that was never accessed in the body is force-marked accessed —
     * "Parameters must always be present even if not used!". This is
     * load-bearing for codegen: visit_LETARRAY / visit_ARRAYLOAD /
     * emit_var_assign all gate on `entry.accessed` at O>1
     * (optimize.py:309/322/344, translator.py:330/980); without this a
     * write-only array/scalar PARAMETER (e.g. `q` in
     * FUNCTION test(q() as UInteger): q(3)=7) is pruned and its whole
     * body vanishes at the default O2. Mark BEFORE exit_scope so the
     * shared symbol nodes the translator later reads carry it; CRITICALLY
     * also BEFORE compute_offsets, because Python orders it identically
     * (symboltable.py:270-283: param accessed-mark on line 274 precedes
     * compute_offsets on line 283) and compute_offsets' Scope.values(
     * filter_by_opt=True) drops the un-accessed at O>1 — otherwise a
     * never-read parameter would lose its frame slot.
     *
     * W150 (Parameter): leave_scope's param loop also emits the
     * unused-parameter warning (symboltable.py:276-277): for an unaccessed
     * param, `if show_warnings and not v.byref: warning_not_used(v.lineno,
     * v.name, kind="Parameter")`. THIS path is the real definition
     * (p_funcdecl, zxbparser.py:2911 — leave_scope() with the default
     * show_warnings=True); the forward-DECLARE path (p_funcdeclforward,
     * :2929, show_warnings=False) returns early above and never reaches
     * here, so emitting here is faithful. byref params are skipped (they
     * can return a value, so are always treated as used). warn_not_used
     * itself gates on optimization_level>0 (errmsg.py:157). Emit BEFORE
     * force-marking accessed (the warning predicates on the original
     * unaccessed state, which the loop condition captures). */
    {
        Scope_ *bs = p->cs->symbol_table->current_scope;
        for (int si = 0; si < bs->ordered_count; si++) {
            AstNode *e = bs->ordered[si];
            if (e && e->tag == AST_ID &&
                e->u.id.scope == SCOPE_parameter && !e->u.id.accessed) {
                /* header_rejected: Python returned before leave_scope, so
                 * no W150 here (declare1/2/3, type-mismatch, SUB-ret-type).
                 * The force-mark-accessed still runs (codegen-silent). */
                if (!header_rejected && !e->u.id.byref)
                    warn_not_used(p->cs, e->lineno,
                                  e->u.id.name ? e->u.id.name : "",
                                  "Parameter");
                e->u.id.accessed = true;
            }
        }
    }

    /* S5.7d — capture the body scope's insertion-ordered entries onto the
     * function ID (Python func.ref.local_symbol_table = current_scope,
     * zxbparser.py:2910, taken BEFORE leave_scope pops the table) and run
     * compute_offsets (symboltable.py:283) to assign per-local +/- IX
     * offsets and the total locals_size (the ic_enter operand). The
     * forward-DECLARE path returns early above and never reaches here, so
     * (like Python's leave_scope(show_warnings=False) discarding the
     * return at zxbparser.py:2929) a declare computes no sizes. */
    {
        Scope_ *body_scope = p->cs->symbol_table->current_scope;

        /* Python order (zxbparser.py:2910-2911): func.ref.local_symbol_table
         * = current_scope (BY REFERENCE), then leave_scope() — which FIRST
         * relocates every CLASS.unknown entry to the global scope (deleting
         * it from current_scope), and only THEN compute_offsets. Because the
         * local table is the same live Scope object, the promoted entries are
         * already gone from it when the FunctionTranslator iterates
         * values(). The C snapshots leaving->ordered, so it must snapshot
         * AFTER exit_scope's promotion-compaction — otherwise a forward
         * `@global` inside the body lingers in local_entries (dimconst2d).
         * exit_scope only flips current_scope to the parent; body_scope still
         * points at the (arena-owned) leaving scope, now with the promoted
         * entries compacted out of ->ordered. */
        symboltable_exit_scope(p->cs->symbol_table);

        int oc = body_scope->ordered_count;
        if (oc > 0) {
            AstNode **le = arena_alloc(&p->cs->arena,
                                       (size_t)oc * sizeof(AstNode *));
            memcpy(le, body_scope->ordered,
                   (size_t)oc * sizeof(AstNode *));
            id_node->u.id.local_entries = le;
            id_node->u.id.local_entries_count = oc;
        }
        id_node->u.id.local_size =
            symboltable_compute_offsets(p->cs->symbol_table, body_scope,
                                        p->cs->opts.optimization_level);
    }

    /* Create function declaration node (id_node was declared before entering scope) */
    AstNode *decl = ast_new(p->cs, AST_FUNCDECL, lineno);

    ast_add_child(p->cs, decl, id_node);
    ast_add_child(p->cs, decl, params);
    ast_add_child(p->cs, decl, body);
    decl->type_ = ret_type;

    /* Python p_funcdecl (zxbparser.py:2913): `p[0].entry.ref.body = p[2]`
     * — the function body is stamped onto the SHARED function-entry's
     * ref. In the C AST the faithful analogue is the dedicated
     * id_node->u.id.body handle (same reason u.id.params exists: a later
     * call node's ast_add_child re-parents id_node, so id->parent is
     * unreliable; the entry-owned field is stable). FunctionGraph's
     * transitive accessed-cascade — Python _get_calls_from_children /
     * filter_inorder descends a CALL's child[0] FUNCTION entry into its
     * .ref.body to reach nested calls (optimize.py:164-184) — needs this
     * link to walk from a callee ID into its body. The definition's body
     * wins over any forward DECLARE's empty block, mirroring Python where
     * the definition's p[2] replaces the forward funcref body. */
    id_node->u.id.body = body;

    vec_push(p->cs->functions, id_node);

    return decl;
}

/* ----------------------------------------------------------------
 * Program parsing
 * ---------------------------------------------------------------- */

void parser_init(Parser *p, CompilerState *cs, const char *input) {
    memset(p, 0, sizeof(*p));
    p->cs = cs;
    blexer_init(&p->lexer, cs, input);
    p->had_error = false;
    p->panic_mode = false;
    tokdump_init(); /* Phase A token-parity dump (inert unless ZXBC_TOKDUMP set) */
    advance(p); /* prime the first token */
}

AstNode *parser_parse(Parser *p) {
    AstNode *program = make_block_node(p, 1);

    /* enable_break: track last CHKBREAK lineno to suppress duplicates on
     * the same source line — faithful to zxbparser.py:101 last_brk_linenum
     * gating in make_break (zxbparser.py:461-471).  CHKBREAK is emitted
     * AFTER each program_line that contains real code (Python's
     * p_program: `make_block(p[1], p[2], make_break(p.lineno(2), p[2]))`,
     * gated by `not is_null(p[2])`).  In the C recursive-descent parser
     * a program_line corresponds to one or more statements terminated
     * by NEWLINE (BTOK_CO between statements on the SAME line never
     * starts a new program_line; that's why Python's make_break runs
     * per program_line, not per statement).  A label-only statement
     * (Python's `label_line_co : label %prec CO` route) does NOT
     * trigger CHKBREAK in Python — verified empirically against
     * break_label0.bas where the label on a line of its own has no
     * CHKBREAK before/at it; only the next non-label program_line
     * does.  Detected by AST_SENTENCE kind=="LABEL". */
    int last_brk_linenum = 0;

    while (!check(p, BTOK_EOF)) {
        /* Skip blank lines */
        if (match(p, BTOK_NEWLINE)) continue;

        int stmt_lineno = p->current.lineno;
        AstNode *stmt = parse_statement(p);
        if (stmt && stmt->tag != AST_NOP) {
            /* Python's SymbolBLOCK.append (block.py:44-51) flattens a
             * child BLOCK into its parent and skips null children — so
             * Python's top-level `program` is one flat BLOCK of
             * statements with no nesting.  The C parser builds
             * compound statements (e.g. `label: stmt` on the same
             * line, parser.c:1395-1404) as an AST_BLOCK; without
             * flattening here, the program ends up with nested BLOCKs
             * that hide the "LABEL immediately followed by CHKBREAK"
             * pattern from OptimizerVisitor.visit_BLOCK
             * (api/optimize.py:113-121 → passes/unreachable.c uc_visit_
             * block).  Flatten ONLY at the top program level (where
             * Python flattens unconditionally via make_block / BLOCK.
             * append); compound statements inside FOR / IF / SUB etc.
             * still keep their own BLOCKs because visit_BLOCK descends
             * recursively.  CHKBREAK-suppression of label-only and
             * decl-only program_lines (`10 DIM a as ubyte` → BLOCK[
             * LABEL_10] alone) depends on this flat shape. */
            if (stmt->tag == AST_BLOCK) {
                for (int i = 0; i < stmt->child_count; i++) {
                    AstNode *gc = stmt->children[i];
                    if (gc && gc->tag != AST_NOP)
                        ast_add_child(p->cs, program, gc);
                }
            } else {
                ast_add_child(p->cs, program, stmt);
            }
        }

        /* Consume end-of-statement terminators; remember whether we
         * crossed a NEWLINE (program_line boundary) vs just a CO
         * (statement-on-same-line separator). */
        bool crossed_newline = false;
        bool sep_consumed = false;
        while (check(p, BTOK_NEWLINE) || check(p, BTOK_CO)) {
            if (check(p, BTOK_NEWLINE)) crossed_newline = true;
            advance(p);
            sep_consumed = true;
        }
        if (check(p, BTOK_EOF)) crossed_newline = true;

        /* Top-level statement separator requirement. Python's program is a
         * sequence of `program_line`s, each `statements NEWLINE` / etc.
         * (zxbparser.py:564-573), and `statements : statement | statements_co
         * statement` requires a CO between statements on one line
         * (zxbparser.py:596-603). Two statements with no NEWLINE/CO between
         * them is a PLY p_error on the first token of the second statement —
         * e.g. `LET a = 0 b = 1` -> ":2: Unexpected token 'b' <ID>", and
         * `LET a = BIN a = a + 1` (BIN with no digits lexes to NUMBER 0, the
         * trailing `a` re-lexed as ID) -> ":2: Unexpected token 'a' <ID>"
         * (bin02.bas; both verified on the oracle). Only fault when a real
         * statement was parsed, no separator followed, we are not at EOF,
         * and no earlier error is being recovered (panic_mode) — never
         * over-reject; valid one-liners use CO and are unaffected.
         *
         * EXEMPT a label-only sentence: `label_line : label statements`
         * (zxbparser.py:617) lets a (line-number or named) label be followed
         * by a statement on the same line with NO separator, e.g.
         * `25 END` / `30 GOTO 50` (opt3_gotogosub.bas). parse_statement
         * returns the bare label sentence (END/etc. are block terminators it
         * declines to absorb), and the following statement is taken on the
         * next top-level iteration — the pre-existing accepted shape. */
        bool top_stmt_label_only =
            stmt && stmt->tag == AST_SENTENCE && stmt->u.sentence.kind &&
            strcmp(stmt->u.sentence.kind, "LABEL") == 0;
        /* EXEMPT block-structured statements (IF/FOR/WHILE/DO…). In a valid
         * program these are always their own program_line and end in a
         * NEWLINE-terminated terminator, so a missing separator after one can
         * only arise from the block parser's own internal recovery of a
         * desync it ALREADY diagnoses (e.g. `IF a<0 THEN:` — the colon-after-
         * THEN form returns an empty single-line IF at the next line's first
         * token; Python faults on `IF` at the END IF line, ifthencoendif.bas,
         * which the END-statement path already reports). Re-faulting on that
         * leftover token here would double-report. The leaf-statement desync
         * (bin02 `LET a=0` then `a`) is unaffected — LET is not a block. */
        bool top_stmt_block = false;
        if (stmt && stmt->tag == AST_SENTENCE && stmt->u.sentence.kind) {
            const char *k = stmt->u.sentence.kind;
            top_stmt_block =
                strcmp(k, "IF") == 0 || strcmp(k, "FOR") == 0 ||
                strcmp(k, "WHILE") == 0 || strncmp(k, "DO_", 3) == 0 ||
                strncmp(k, "LOOP_", 5) == 0;
        }
        if (!sep_consumed && !check(p, BTOK_EOF) && !p->panic_mode &&
            stmt && stmt->tag != AST_NOP && !top_stmt_label_only &&
            !top_stmt_block) {
            tok_unexpected_error(p, &p->current);
        }

        /* Emit CHKBREAK sentence after program_line if enable_break is
         * on and the line carried a real (non-label-only) statement.
         * Faithful to make_break (zxbparser.py:461-471):
         *   - skip if !OPTIONS.enable_break
         *   - skip if lineno == last_brk_linenum (same-line dedupe)
         *   - skip if is_null(p[2]) (statement absent/NOP)
         * Adds: skip label-only statements (LABEL-sentence shape) so
         *   break_label0.bas matches Python (label-only line carries
         *   no CHKBREAK; the next non-label line's CHKBREAK fires).
         * Adds: skip non-statement contributions (#pragma, #require,
         *   #init, #line — all return AST_NOP, already filtered above). */
        bool stmt_real = stmt && stmt->tag != AST_NOP;
        bool stmt_is_label_only =
            stmt_real && stmt->tag == AST_SENTENCE && stmt->u.sentence.kind &&
            strcmp(stmt->u.sentence.kind, "LABEL") == 0;
        if (crossed_newline && p->cs->opts.enable_break && stmt_real &&
            !stmt_is_label_only && stmt_lineno != last_brk_linenum) {
            /* make_sentence(lineno, "CHKBREAK",
             *               make_number(lineno, lineno, TYPE.uinteger))
             * (zxbparser.py:471). */
            TypeInfo *uint_t =
                p->cs->symbol_table->basic_types[TYPE_uinteger];
            AstNode *brk = make_sentence_node(p, "CHKBREAK", stmt_lineno);
            ast_add_child(p->cs, brk,
                make_number(p, stmt_lineno, stmt_lineno, uint_t));
            ast_add_child(p->cs, program, brk);
            last_brk_linenum = stmt_lineno;
        }

        if (p->panic_mode) synchronize(p);
    }

    /* Append implicit END */
    AstNode *end = make_sentence_node(p, "END", p->current.lineno);
    end->u.sentence.sentinel = true;
    ast_add_child(p->cs, end,
        make_number(p, 0, p->current.lineno, p->cs->symbol_table->basic_types[TYPE_uinteger]));
    ast_add_child(p->cs, program, end);

    p->cs->ast = program;
    return p->had_error ? NULL : program;
}

/* ================================================================
 * PHASE D — PLY engine integration (PARALLEL parser, behind a flag).
 *
 * This wires the authoritative LALR(1) engine (csrc/zxbc/plyparser/) to the
 * EXISTING C AST representation: each reduce-action builds the byte-for-byte
 * same AstNode the recursive-descent parser above builds, per the real action
 * bodies in src/zxbc/zxbparser.py. It is ADDITIVE — the production entry
 * points (parser_init/parser_parse) are unchanged; this runs only when
 * explicitly invoked by the Phase-D AST-compare harness (plyparse_program),
 * so production output stays byte-identical until the Phase E swap.
 *
 * Validation vehicle: build the program two ways (recursive-descent vs PLY
 * engine) and deep-compare the ASTs (pd_ast_equal). A production whose action
 * is not yet ported sets *pd_unwired so the harness reports the file as
 * "not-yet-wired" rather than silently producing a wrong tree.
 * ================================================================ */
#include "plyparser/ply_engine.h"

/* Per-parse context threaded through the engine callbacks as `ud`. */
typedef struct PdCtx {
    Parser *p;             /* the production Parser (lexer + CompilerState) */
    bool unwired;          /* set when a reduce hits an unported production */
    int unwired_prod;      /* first such production number (for reporting) */
    int last_lineno;       /* lexer.lineno tracker (== p.lexer.lineno) */
    bool in_preproc;       /* lex-adapter preproc-state tracker */
    bool emit_errors;      /* Phase C-full: when true, pd_error emits the real
                            * p_error message+line (for the error-validation
                            * harness). When false (the astcmp meter), pd_error
                            * only flags unwired so a p_error file classifies as
                            * UNWIRED (not a DIFF) — the valid-corpus meter is
                            * unchanged. */
    bool p_error_fired;    /* set whenever pd_error fired (either mode). */
    BToken last_btoken;    /* the BToken pd_lex last returned — pd_error renders
                            * its p.value via the production's tok_ply_value
                            * (correct for punctuation/keyword/ID/NUMBER). */
    bool last_btoken_set;
} PdCtx;

/* A non-AST value carried on the parse stack for productions whose Python
 * action returns a plain tuple/string rather than a Symbol — e.g. `lexpr`
 * returns the Id(name, lineno) NamedTuple (zxbparser.py:109,1119-1134). */
typedef struct PdId {
    const char *name;
    int lineno;
} PdId;

static PdId *pd_new_id(Parser *p, const char *name, int lineno) {
    PdId *id = arena_alloc(&p->cs->arena, sizeof(PdId));
    id->name = name;
    id->lineno = lineno;
    return id;
}

/* Cross-reduce state for a function/sub definition: built at the
 * `function_def` reduce (which declares the func in the parent scope, enters
 * the body scope, and pushes FUNCTION_LEVEL — Python p_function_def's exact
 * timing), threaded through header_pre/header/body, consumed at `p_funcdecl`
 * (which leaves the scope, computes offsets, pops FUNCTION_LEVEL, and builds
 * the FUNCDECL). `rejected` marks a not-yet-ported sub-form (pre-existing/
 * forwarded entry, etc.) so the engine flags UNWIRED rather than mis-building;
 * the scope is still entered/left so the parse stays balanced. */
typedef struct PdFuncDef {
    AstNode *id_node;      /* the declared FUNCTION/SUB entry */
    const char *name_raw;  /* raw name (suffix not stripped) — enter_scope arg */
    int lineno;
    bool is_function;
    Convention conv;
    TypeInfo *ret_type;
    TypeInfo *suffix_type; /* sigil-derived return type (test$ -> string), or NULL */
    AstNode *params;       /* PARAMLIST */
    bool rejected;         /* unported sub-form hit */
} PdFuncDef;

/* A single parameter carried by param_def/param_definition: the ARGUMENT node
 * that goes in the PARAMLIST PLUS the body-scope symbol that param_def
 * registered (Python declare_param makes ONE symbol that is both; the C
 * production parser keeps them as separate nodes — parser.c:7397 ARGUMENT vs
 * :7656 body symbol — so we carry both and copy the offset across at
 * function_header_pre, exactly as the production does). */
typedef struct PdParam {
    AstNode *arg;          /* AST_ARGUMENT (PARAMLIST child) */
    AstNode *body_sym;     /* body-scope symbol (for offset/byref copy) */
} PdParam;

/* The else-part value (else_part_inline / else_part nonterminals) — Python
 * carries a list `[else_, endif]` or `[else_, endif, extra]` that every
 * consumer flattens via make_block(*list). Carry the elements; the IF/elseif
 * consumers build the block. (zxbparser.py:1470-1511). */
typedef struct PdElse {
    AstNode *e[3];
    int n;
} PdElse;

/* The substr value (the `substr` non-terminal) — Python carries a tuple
 * `(lower, upper)` of uinteger-pre-cast bounds (zxbparser.py:2600-2638). The
 * string-slice-consuming reduces (288/289/etc.) pass these to
 * make_strslice_node. `raw_lower`/`raw_upper` are the bounds BEFORE the
 * uinteger pre-cast — the array-substr WRITE path (302/303) needs them raw
 * because the C production builds the array-substr inner STRSLICE bounds at
 * their natural type (build_array_substr_lvalue, not the cast read path). */
typedef struct PdSubstr {
    AstNode *lower;
    AstNode *upper;
    AstNode *raw_lower;
    AstNode *raw_upper;
} PdSubstr;

/* The elseif_expr value — Python carries a tuple `(label_, cond_)`
 * (zxbparser.py:1431-1442). */
typedef struct PdElseif {
    AstNode *label;
    AstNode *cond;
    int lineno;
} PdElseif;

/* A growable Id list — the value of the `idlist` nonterminal (Python: a
 * Python list of Id tuples). */
typedef struct PdIdList {
    PdId **ids;
    int count;
    int cap;
} PdIdList;

static PdIdList *pd_new_idlist(Parser *p, PdId *first) {
    PdIdList *l = arena_alloc(&p->cs->arena, sizeof(PdIdList));
    l->cap = 4;
    l->ids = arena_alloc(&p->cs->arena, sizeof(PdId *) * l->cap);
    l->ids[0] = first;
    l->count = 1;
    return l;
}

static void pd_idlist_append(Parser *p, PdIdList *l, PdId *id) {
    if (l->count == l->cap) {
        int nc = l->cap * 2;
        PdId **ni = arena_alloc(&p->cs->arena, sizeof(PdId *) * nc);
        memcpy(ni, l->ids, sizeof(PdId *) * l->count);
        l->ids = ni;
        l->cap = nc;
    }
    l->ids[l->count++] = id;
}

/* is_null (api/check.py:is_null): None, NOP, or a BLOCK that is recursively
 * all-null. */
static bool pd_is_null(const AstNode *n) {
    if (n == NULL) return true;
    if (n->tag == AST_NOP) return true;
    if (n->tag == AST_BLOCK) {
        for (int i = 0; i < n->child_count; i++)
            if (!pd_is_null(n->children[i])) return false;
        return true;
    }
    return false;
}

static void pd_block_append(Parser *p, AstNode *blk, AstNode *arg);

/* IF/loop body builder matching the PRODUCTION parser's then-block shape: its
 * recursive-descent loop appends each top-level body statement via ast_add_child
 * WITHOUT recursively flattening — so a labelled body line (`20 a=a+1`, a
 * label_line compound BLOCK[LABEL,stmt]) stays ONE nested child, not flattened
 * into siblings. So flatten the body BLOCK ONE level (its direct program_line/
 * statement children added as-is), then append the endif per make_block (flatten
 * a BLOCK endif, skip a null/NOP). (pd_make_block2's recursive flatten would
 * over-flatten the label compound — the ifthen divergence.) */
static AstNode *pd_if_body(Parser *p, AstNode *stat, AstNode *endif) {
    AstNode *blk = make_block_node(p, p->current.lineno);
    if (stat && stat->tag == AST_BLOCK) {
        for (int i = 0; i < stat->child_count; i++) {
            AstNode *ch = stat->children[i];
            if (!pd_is_null(ch)) ast_add_child(p->cs, blk, ch);
        }
    } else if (!pd_is_null(stat)) {
        ast_add_child(p->cs, blk, stat);
    }
    /* endif: make_block appends it flattening a BLOCK / skipping a null. */
    pd_block_append(p, blk, endif);
    return blk;
}

/* Does a block carry a top-level NOP child (a leading/internal `:` in a
 * co_statements_co body)? The PRODUCTION's IF body loop appends raw statements
 * INCLUDING such NOPs, while make_block (the engine path) filters them — a
 * byte-clean nesting divergence (Phase-E-reconcile). Used to defer those
 * else/elseif bodies rather than emit a wrong (NOP-dropped) tree. */
static bool pd_block_has_nop(const AstNode *n) {
    if (!n || n->tag != AST_BLOCK) return false;
    for (int i = 0; i < n->child_count; i++)
        if (n->children[i] && n->children[i]->tag == AST_NOP) return true;
    return false;
}

/* Does a (possibly-block) node contain a LABEL sentence at top level? Used to
 * defer IF/loop bodies whose label_line nesting via program_co differs from
 * the production parser's flat then-block (resolved precisely later). */
static bool pd_block_has_label(const AstNode *n) {
    if (!n) return false;
    if (n->tag == AST_SENTENCE && n->u.sentence.kind &&
        strcmp(n->u.sentence.kind, "LABEL") == 0)
        return true;
    if (n->tag == AST_BLOCK) {
        for (int i = 0; i < n->child_count; i++)
            if (pd_block_has_label(n->children[i])) return true;
    }
    return false;
}

/* make_block / SymbolBLOCK.append (symbols/block.py): build a BLOCK, append
 * each arg skipping nulls and FLATTENING child BLOCKs. */
static void pd_block_append(Parser *p, AstNode *blk, AstNode *arg) {
    if (pd_is_null(arg)) return;
    if (arg->tag == AST_BLOCK) {
        for (int i = 0; i < arg->child_count; i++)
            pd_block_append(p, blk, arg->children[i]);
    } else {
        ast_add_child(p->cs, blk, arg);
    }
}

static AstNode *pd_make_block2(Parser *p, AstNode *a, AstNode *b) {
    AstNode *blk = make_block_node(p, p->current.lineno);
    pd_block_append(p, blk, a);
    if (b) pd_block_append(p, blk, b);
    return blk;
}

/* make_block(*args) over an N-element list (the else_part list flatten). */
static AstNode *pd_make_block_n(Parser *p, AstNode **args, int n) {
    AstNode *blk = make_block_node(p, p->current.lineno);
    for (int i = 0; i < n; i++) pd_block_append(p, blk, args[i]);
    return blk;
}

/* Binary op with parse_infix's exact error recovery: make_binary_node, and on
 * NULL (a type error already reported) fall back to the LEFT operand — matching
 * the production parser's parse_infix `if (!result) return left;` (parser.c:
 * 3522), the byte-clean swap baseline the engine must equal. */
static AstNode *pd_binary(Parser *p, const char *op, AstNode *left,
                          AstNode *right, int lineno) {
    AstNode *result = make_binary_node(p->cs, op, left, right, lineno, NULL);
    if (!result) return left;  /* parse_infix recovery */
    return result;
}

/* Single-arg parenless builtin (`KW bexpr`): build the AST_BUILTIN(fname) with
 * the pre-built arg as child[0], then make_builtin_node (the extracted node-
 * build core does the per-builtin fold/type). */
static AstNode *pd_builtin1(Parser *p, const char *fname, BTokenType kw,
                           AstNode *arg, int lineno) {
    if (!arg) return NULL;
    AstNode *n = ast_new(p->cs, AST_BUILTIN, lineno);
    n->u.builtin.fname = arena_strdup(&p->cs->arena, fname);
    ast_add_child(p->cs, n, arg);
    return make_builtin_node(p, n, arg, kw, lineno);
}

/* Compute the SAVE/LOAD/VERIFY `... DATA [id]` start/length pair, faithfully
 * mirroring the production parser's DATA-form branch (parser.c:4597-4699 /
 * Python p_save_data + p_load_data, zxbparser.py:2257-2354). With an `id`:
 * start = UNARY[ADDRESS, access_id(id)] (uinteger), length = the array memsize
 * or the scalar type size. Without an id: the .core.ZXBASIC_USER_DATA[_LEN]
 * root-global labels' ADDRESS. Returns false (→ p[0]=None) if the id is
 * unresolvable. */
static bool loadsave_data_startlen(Parser *p, const char *id_name, int id_ln,
                                   int data_ln, AstNode **start, AstNode **length) {
    TypeInfo *uint_t = p->cs->symbol_table->basic_types[TYPE_uinteger];
    if (id_name) {
        AstNode *entry = symboltable_access_id(p->cs->symbol_table, p->cs,
                                               id_name, id_ln, NULL, CLASS_var);
        if (!entry) return false;  /* p[0] = None */
        entry->u.id.accessed = true;
        AstNode *s = make_unary_node(p->cs, "ADDRESS", entry, id_ln);
        if (s) s->type_ = uint_t;
        AstNode *l;
        if (entry->u.id.class_ == CLASS_array) {
            int memsize = (entry->type_ ? type_size(entry->type_) : 0);
            if (entry->u.id.arr_boundlist) {
                AstNode *bl = entry->u.id.arr_boundlist;
                for (int bi = 0; bi < bl->child_count; bi++) {
                    AstNode *b = bl->children[bi];
                    if (b && b->child_count >= 2 &&
                        b->children[0]->tag == AST_NUMBER &&
                        b->children[1]->tag == AST_NUMBER) {
                        int lo = (int)b->children[0]->u.number.value;
                        int hi = (int)b->children[1]->u.number.value;
                        memsize *= (hi - lo + 1);
                    }
                }
            }
            l = make_number(p, memsize, id_ln, NULL);
        } else {
            l = make_number(p, entry->type_ ? type_size(entry->type_) : 0,
                            id_ln, NULL);
        }
        *start = s; *length = l;
        return true;
    }
    /* No-id form: the .core.ZXBASIC_USER_DATA[_LEN] root-global labels. */
    const char *names[2] = { ".core.ZXBASIC_USER_DATA",
                             ".core.ZXBASIC_USER_DATA_LEN" };
    AstNode *out2[2];
    for (int k = 0; k < 2; k++) {
        AstNode *lbl = symboltable_access_label(p->cs->symbol_table, p->cs,
                                                names[k], data_ln);
        if (lbl) {
            char *m = arena_strdup(&p->cs->arena, names[k]);
            lbl->u.id.mangled = m;
            lbl->t = m;  /* LabelRef.t == parent.mangled */
            lbl->u.id.class_ = CLASS_label;
            lbl->u.id.scope = SCOPE_global;
            lbl->u.id.declared = true;
            lbl->u.id.accessed = true;
            lbl->type_ = uint_t;
        }
        out2[k] = make_unary_node(p->cs, "ADDRESS", lbl, data_ln);
        if (out2[k]) out2[k]->type_ = uint_t;
    }
    *start = out2[0]; *length = out2[1];
    return true;
}

/* Build the SAVE/LOAD/VERIFY sentence: SENTENCE(kind, str_expr, start, length)
 * (parser.c:4770-4774). NULL children are dropped by ast_add_child. */
static AstNode *make_loadsave_node(Parser *p, const char *kind, int ln,
                                   AstNode *str_expr, AstNode *start,
                                   AstNode *length) {
    AstNode *s = make_sentence_node(p, kind, ln);
    if (str_expr) ast_add_child(p->cs, s, str_expr);
    if (start)    ast_add_child(p->cs, s, start);
    if (length)   ast_add_child(p->cs, s, length);
    return s;
}

/* Snapshot a substr bound's RAW (pre-uinteger-cast) value: a constant NUMBER
 * is copied to a fresh auto-typed node (so a later make_typecast on the
 * original does not mutate this snapshot); any other node is returned as-is
 * (make_typecast wraps non-constants, never mutating them). Used to keep the
 * array-substr WRITE bounds at their natural type while the read path casts. */
static AstNode *pd_clone_bound(Parser *p, AstNode *n) {
    if (n && n->tag == AST_NUMBER)
        return make_number(p, n->u.number.value, n->lineno, n->type_);
    return n;
}

/* No-arg builtin (RND/INKEY): build AST_BUILTIN(fname) with no child and the
 * given result type — matching parse_builtin_func's RND/INKEY node-build
 * (parser.c:1642-1655). */
static AstNode *pd_builtin0(Parser *p, const char *fname, BasicType bt,
                           int lineno) {
    AstNode *n = ast_new(p->cs, AST_BUILTIN, lineno);
    n->u.builtin.fname = arena_strdup(&p->cs->arena, fname);
    n->type_ = p->cs->symbol_table->basic_types[bt];
    return n;
}

/* make_sub_call (SymbolCALL.make_node, symbols/call.py:90-112) — the
 * parenless/with-args statement-level sub call. access_func resolves/auto-
 * declares the callee (child[0]); arglist is child[1]; FUNCCALL->CALL retag;
 * register in function_calls for the pending-call check. Faithful to the
 * production parser's bare-ID node-build (parser.c:5256-5367) but taking a
 * pre-built arglist (the LALR `arguments`/`arg_list` reduce supplies it),
 * which matches Python's reduce order (args reduce before statement:ID). */
static AstNode *pd_sub_call(Parser *p, const char *name, int lineno,
                           AstNode *arglist) {
    AstNode *entry = symboltable_lookup(p->cs->symbol_table, name);
    if (entry && entry->u.id.class_ == CLASS_var)
        zxbc_error(p->cs, lineno, "'%s' is a VAR, not a FUNCTION", name);
    else if (entry && entry->u.id.class_ == CLASS_const)
        zxbc_error(p->cs, lineno, "'%s' is a CONST, not a FUNCTION", name);

    AstNode *s = ast_new(p->cs, AST_FUNCCALL, lineno);
    AstNode *id_node =
        symboltable_access_func(p->cs->symbol_table, p->cs, name, lineno, NULL);
    if (id_node) {
        s->type_ = id_node->type_;
    } else {
        id_node = ast_new(p->cs, AST_ID, lineno);
        id_node->u.id.name = arena_strdup(&p->cs->arena, name);
        s->type_ = p->cs->default_type;
    }
    ast_add_child(p->cs, s, id_node);
    if (!arglist) arglist = ast_new(p->cs, AST_ARGLIST, lineno);
    ast_add_child(p->cs, s, arglist);

    if (id_node->tag == AST_ID &&
        id_node->u.id.class_ != CLASS_var && id_node->u.id.class_ != CLASS_const) {
        if (p->cs->current_file)
            s->u.call.filename = arena_strdup(&p->cs->arena, p->cs->current_file);
        s->u.call.callee_inline =
            (id_node->u.id.params != NULL && !id_node->u.id.forwarded);
        vec_push(p->cs->function_calls, s);
        inline_check_call_arguments(p, s);
    }
    if (s->tag == AST_FUNCCALL) s->tag = AST_CALL;
    return s;
}

/* ---- lex adapter: BToken -> PlySym, applying the Phase-A-documented
 * translations (NEWLINE lineno -1; preproc NUMBER->INTEGER / STRC->STRING;
 * ERROR carries the offending char). ID/ARRAY_ID resolution is whatever the
 * lexer returns when driven here — with the engine mutating the SAME symbol
 * table at PLY's reduce timing, this is the faithful stream (validated in the
 * harness). ---- */
static bool pd_lex(void *ud, PlySym *out) {
    PdCtx *c = ud;
    Parser *p = c->p;
    BToken t;
    for (;;) {
        t = blexer_next(&p->lexer);
        /* Unlike the production advance(), the engine MUST see ERROR tokens
         * (PLY's parser does) — do not skip them. */
        break;
    }
    c->last_lineno = p->lexer.lineno;
    c->last_btoken = t;          /* for pd_error's p.value rendering */
    c->last_btoken_set = true;
    if (t.type == BTOK_EOF)
        return false; /* $end */

    memset(out, 0, sizeof(*out));
    const char *name = btok_name(t.type);
    int id = ply_term_id(name);
    out->lineno = t.lineno;

    bool was_preproc = c->in_preproc;
    if (t.type == BTOK__PRAGMA || t.type == BTOK__INIT ||
        t.type == BTOK__REQUIRE || t.type == BTOK__LINE ||
        t.type == BTOK__PUSH || t.type == BTOK__POP)
        c->in_preproc = true;
    else if (t.type == BTOK_NEWLINE)
        c->in_preproc = false;

    if (t.type == BTOK_NEWLINE) {
        out->type = id;
        out->lineno = t.lineno - 1; /* PLY captures pre-increment lineno */
        out->sval = "\n";
        return true;
    }
    if (was_preproc && t.type == BTOK_NUMBER) {
        out->type = ply_term_id("INTEGER");
        out->sval = t.sval;       /* raw digit string */
        out->num = t.numval;
        return true;
    }
    if (was_preproc && t.type == BTOK_STRC) {
        out->type = ply_term_id("STRING");
        out->sval = t.sval;
        return true;
    }
    out->type = id;
    out->num = t.numval;
    out->sval = t.sval;
    return true;
}

/* p[N] accessor: the value of RHS symbol N (1-based, as in PLY p[1..len]).
 * For a nonterminal this is the built AstNode; for a leaf-terminal the action
 * reads .num/.sval directly off rhs[N-1]. */
#define PD_NODE(i)   ((AstNode *)rhs[(i) - 1].value)
#define PD_LINENO(i) (rhs[(i) - 1].lineno)
#define PD_NUM(i)    (rhs[(i) - 1].num)
#define PD_SVAL(i)   (rhs[(i) - 1].sval)

/* ---- reduce-action dispatch. Returns false on a SyntaxError-raising action
 * (none wired yet). Sets *out to p[0]. ---- */
static bool pd_action(void *ud, int prodno, PlySym *rhs, int len,
                      void **out, int *out_lineno) {
    PdCtx *c = ud;
    Parser *p = c->p;
    SymbolTable *st = p->cs->symbol_table;
    AstNode *r = NULL;
    int rln = (len > 0) ? rhs[0].lineno : p->lexer.lineno;

    switch (prodno) {
    /* ---- program spine ---- */
    case 1: { /* start : program
               * p_start (zxbparser.py:505): ast = p[1]; if ast is a BLOCK and
               * its last child is not an ender, append a sentinel END(0). For
               * Phase-D comparability with parser_parse's finalisation we
               * reproduce the implicit-END append + the top-level flatten.
               * (Symbol-table label/class checks, data_ast, pending-call
               * checks — Phase E.) */
        AstNode *prog = PD_NODE(1);
        AstNode *flat = make_block_node(p, prog ? prog->lineno : p->lexer.lineno);
        if (prog) pd_block_append(p, flat, prog);
        /* implicit END sentinel (parser_parse tail / p_start make_sentence
         * END make_number(0) sentinel=True). */
        AstNode *end = make_sentence_node(p, "END", p->lexer.lineno);
        end->u.sentence.sentinel = true;
        ast_add_child(p->cs, end,
            make_number(p, 0, p->lexer.lineno, st->basic_types[TYPE_uinteger]));
        ast_add_child(p->cs, flat, end);
        r = flat;
        break;
    }
    case 2: /* program : program_line */
        r = pd_make_block2(p, PD_NODE(1), NULL);
        break;
    case 3: /* program : program program_line */
        r = pd_make_block2(p, PD_NODE(1), PD_NODE(2));
        break;
    case 4: case 5: case 6: case 7: case 8: case 9:
        /* program_line : X NEWLINE  -> p[1] */
        r = PD_NODE(1);
        break;
    case 10: /* program_line : NEWLINE -> NOP */
        r = make_nop(p);
        break;
    case 11: case 12: /* co_statements_co : co_statements[_co] CO -> p[1] */
        r = PD_NODE(1);
        break;
    case 13: /* co_statements_co : CO -> NOP */
        r = make_nop(p);
        break;
    case 14: /* co_statements : co_statements_co statement */
        r = pd_make_block2(p, PD_NODE(1), PD_NODE(2));
        break;
    case 15: case 16: /* statements_co : statements[_co] CO -> p[1] */
        r = PD_NODE(1);
        break;
    case 17: /* statements : statement */
        r = pd_make_block2(p, PD_NODE(1), NULL);
        break;
    case 18: /* statements : statements_co statement */
        r = pd_make_block2(p, PD_NODE(1), PD_NODE(2));
        break;
    case 19: /* statement : var_decl */
        r = PD_NODE(1);
        break;

    /* ---- labels ----
     * label : LABEL (20, p_label) -> label_define (declare + SENTENCE LABEL).
     * The LABEL terminal value is the source text (sval) or, for a numeric
     * label, the integer rendered as a string. */
    case 20: {
        const char *lt = PD_SVAL(1);
        char lbuf[32];
        if (!lt) { snprintf(lbuf, sizeof(lbuf), "%d", (int)PD_NUM(1)); lt = lbuf; }
        r = label_define(p, lt, PD_LINENO(1));
        break;
    }
    case 21: case 22: /* label_line : label statements | label co_statements
                       * (p_program_line_label) -> make_block(label, p[2]) */
        r = pd_make_block2(p, PD_NODE(1), PD_NODE(2));
        break;
    case 23: /* label_line : label_line_co -> p[1] */
        r = PD_NODE(1);
        break;
    case 24: case 25: /* label_line_co : label statements_co|co_statements_co
                       * (p_label_line_co, len 3) -> make_block(label, p[2]) */
        r = pd_make_block2(p, PD_NODE(1), PD_NODE(2));
        break;
    case 26: /* label_line_co : label (len 2) -> the label */
        r = PD_NODE(1);
        break;
    case 27: /* program_co : program -> p[1] */
        r = PD_NODE(1);
        break;
    case 28: case 29: case 30: /* program_co : program X -> make_block(p[1],p[2]) */
        r = pd_make_block2(p, PD_NODE(1), PD_NODE(2));
        break;

    /* ---- function/sub declaration subsystem (no-param fresh case first) ----
     * convention : <empty> | STDCALL | FASTCALL (334/335/336, p_convention). */
    case 334: /* <empty> -> stdcall (p_convention) */
    case 335: /* STDCALL  -> stdcall */
        *out = (void *)(intptr_t)CONV_stdcall;
        *out_lineno = p->lexer.lineno;
        return true;
    case 336: /* FASTCALL -> fastcall (p_convention2) */
        *out = (void *)(intptr_t)CONV_fastcall;
        *out_lineno = PD_LINENO(1);
        return true;

    case 332: case 333: { /* function_def : FUNCTION|SUB convention ID
                           * (p_function_def): declare in parent scope, then
                           * enter_scope + FUNCTION_LEVEL.push. Scope is entered
                           * HERE — before params/body reduce — exactly as
                           * Python. Only the FRESH (no pre-existing entry) case
                           * is wired; pre-existing/forwarded entries are
                           * flagged rejected (UNWIRED) and deferred. */
        bool is_func = (prodno == 332);
        Convention conv = (Convention)(intptr_t)rhs[1].value;
        const char *name_raw = PD_SVAL(3);
        int ln = PD_LINENO(3);
        SymbolClass cls = is_func ? CLASS_function : CLASS_sub;
        /* suffix-stripped key (matching the production parser) */
        const char *fname = name_raw;
        char fbuf[256];
        size_t fl = strlen(name_raw);
        if (fl > 0 && fl < sizeof(fbuf) && is_deprecated_suffix(name_raw[fl - 1])) {
            memcpy(fbuf, name_raw, fl - 1); fbuf[fl - 1] = '\0'; fname = fbuf;
        }
        PdFuncDef *fd = arena_alloc(&p->cs->arena, sizeof(PdFuncDef));
        memset(fd, 0, sizeof(*fd));
        fd->name_raw = arena_strdup(&p->cs->arena, name_raw);
        fd->lineno = ln; fd->is_function = is_func; fd->conv = conv;
        /* Sigil-typed function names (`test$`) derive the return type from
         * the suffix (production parser.c:7276-7279 fn_suffix_type), used at
         * function_header_pre (330) when there is no explicit `AS` typedef.
         * (sigilfunc, param0/2/3.) */
        if (fl > 0 && is_deprecated_suffix(name_raw[fl - 1])) {
            BasicType sbt = suffix_to_type(name_raw[fl - 1]);
            fd->suffix_type = p->cs->symbol_table->basic_types[sbt];
        }
        AstNode *pre = symboltable_lookup(p->cs->symbol_table, fname);
        AstNode *id;
        if (pre) {
            /* declare_func reuse branch (symboltable.py:728-741 / production
             * parser.c:7471-7512): reuse the shared object; CLASS_unknown ->
             * to_function; re-mangle "{current_namespace}_{name}" (literal '_'
             * join). Then the duplicate / class-mismatch checks. */
            id = pre;
            if (id->u.id.class_ == CLASS_unknown)
                id->u.id.class_ = cls;
            const char *ns = p->cs->symbol_table->current_scope->namespace_;
            if (!ns) ns = "";
            size_t nl = strlen(fname), sl = strlen(ns);
            char *m = arena_alloc(&p->cs->arena, sl + 1 + nl + 1);
            memcpy(m, ns, sl); m[sl] = '_'; memcpy(m + sl + 1, fname, nl + 1);
            id->u.id.mangled = m;
            /* duplicate definition (a fully-defined, non-forwarded entry). */
            if (id->u.id.declared && id->lineno != ln &&
                (id->u.id.class_ == CLASS_function || id->u.id.class_ == CLASS_sub) &&
                !id->u.id.forwarded) {
                zxbc_error(p->cs, ln,
                           "Duplicate function name '%s', previously defined at %d",
                           fname, id->lineno);
            }
            /* class mismatch (e.g. SUB vs FUNCTION reuse). */
            if (id->u.id.class_ != CLASS_unknown && id->u.id.class_ != cls) {
                zxbc_error(p->cs, ln, "'%s' is a %s, not a %s", fname,
                           symbolclass_to_string(id->u.id.class_),
                           symbolclass_to_string(cls));
            }
        } else {
            id = symboltable_declare(p->cs->symbol_table, p->cs, fname, ln, cls);
        }
        id->u.id.class_ = cls;
        id->u.id.declared = true;
        id->u.id.convention = conv;
        id->lineno = ln;
        fd->id_node = id;
        symboltable_enter_scope(p->cs->symbol_table, p->cs, fd->name_raw);
        vec_push(p->cs->function_level, fd->id_node);
        *out = fd;
        *out_lineno = ln;
        return true;
    }

    case 330: { /* function_header_pre : function_def param_decl typedef
                 * (p_function_header_pre). No-param: param_decl is an empty
                 * PARAMLIST; typedef NULL (no AS) or the return type. */
        PdFuncDef *fd = (PdFuncDef *)rhs[0].value;
        AstNode *params = (AstNode *)rhs[1].value;
        TypeInfo *td = (TypeInfo *)rhs[2].value;
        fd->params = params;
        /* return type: explicit `AS <type>` (td) wins; else a FUNCTION with a
         * sigil-suffixed name takes the sigil type (production parser.c:
         * 7520-7528 fn_suffix_type); else a FUNCTION with no AS gets the
         * implicit default; a SUB stays NULL. */
        if (td) {
            fd->ret_type = td;
        } else if (fd->is_function && fd->suffix_type) {
            fd->ret_type = fd->suffix_type;
        } else if (fd->is_function) {
            fd->ret_type = type_new_ref(p->cs, p->cs->default_type, fd->lineno, true);
        }
        if (fd->id_node) {
            /* p_function_header_pre type reconciliation (zxbparser.py:2961):
             *   if not typedef.implicit or entry.type_ is None or
             *      entry.type_ == unknown:  entry.type_ = typedef
             * An EXPLICIT `AS` type always wins; otherwise a FORWARDED entry's
             * EXISTING type (e.g. a `declare function test$(...)` sigil-derived
             * string) is preserved against an implicit default at the no-sigil
             * definition (param2). NARROWED to forwarded entries only — a fresh
             * function always takes its computed ret_type (the common case is
             * untouched; broadening to all entries regressed CALL types). */
            AstNode *e = fd->id_node;
            bool ret_is_explicit = (td && !td->implicit) ||
                                   (fd->is_function && fd->suffix_type && !td);
            bool entry_type_set = e->type_ &&
                !(e->type_->final_type &&
                  e->type_->final_type->basic_type == TYPE_unknown);
            if (e->u.id.forwarded && entry_type_set && !ret_is_explicit)
                fd->ret_type = e->type_;       /* keep forwarded type */
            else
                e->type_ = fd->ret_type;
            fd->id_node->u.id.params = params;
            fd->id_node->u.id.param_size = parser_assign_param_offsets(p, params);
        }
        /* After parser_assign_param_offsets stamps each ARGUMENT's cumulative
         * offset, copy offset+byref onto the matching body-scope symbol — the
         * production's dual-node reconciliation (parser.c:7675-7677). The body
         * symbols were registered (by name) at the param_def reduces; find
         * them in the current (body) scope. */
        if (params) {
            for (int i = 0; i < params->child_count; i++) {
                AstNode *arg = params->children[i];
                if (!arg || arg->tag != AST_ARGUMENT) continue;
                /* The body symbol is keyed under the suffix-stripped name
                 * (declare strips the sigil); strip here too so a sigil
                 * param (`s$`) reconciles. */
                const char *an = arg->u.argument.name;
                size_t anl = an ? strlen(an) : 0;
                char anbuf[256];
                if (anl > 0 && anl < sizeof(anbuf) &&
                    is_deprecated_suffix(an[anl - 1])) {
                    memcpy(anbuf, an, anl - 1); anbuf[anl - 1] = '\0'; an = anbuf;
                }
                AstNode *bsym = symboltable_lookup(p->cs->symbol_table, an);
                if (bsym) {
                    bsym->u.id.offset = arg->u.argument.offset;
                    bsym->u.id.offset_set = true;
                    bsym->u.id.byref = arg->u.argument.byref;
                }
            }
        }
        *out = fd;
        *out_lineno = fd->lineno;
        return true;
    }
    case 326: case 327: /* function_header : function_header_pre CO|NEWLINE */
        *out = rhs[0].value;
        *out_lineno = PD_LINENO(1);
        return true;

    /* ---- error-token productions (Phase C-full, p[0]=None) ----
     * These fire only AFTER the engine synthesised an `error` token during
     * recovery — pd_error already emitted the message. The action just yields
     * None (zxbparser.py p_function_header_error / p_function_error /
     * p_param_decl_errpr). Return NULL without flagging unwired (the p_error
     * path already marked the file in astcmp mode). */
    case 328: case 329: /* function_header : function_def error CO|NEWLINE */
    case 331:           /* function_declaration : function_header program_co END error */
    case 340:           /* param_decl : LP error RP */
        r = NULL;
        break;

    case 324: { /* function_declaration : function_header function_body
                 * (p_funcdecl): leave scope, compute offsets, pop
                 * FUNCTION_LEVEL, build FUNCDECL. */
        /* If an upstream production already flagged UNWIRED (e.g. the
         * function_header error-token productions 328/329, which take the
         * default NOP path), rhs[0].value is a NOP AstNode, NOT a PdFuncDef —
         * type-punning it would dereference garbage (let_expr_type_crash).
         * The file is already flagged UNWIRED; bail to NOP without touching it. */
        if (c->unwired) { r = make_nop(p); break; }
        PdFuncDef *fd = (PdFuncDef *)rhs[0].value;
        AstNode *body = (AstNode *)rhs[1].value;
        if (!body) body = make_block_node(p, fd->lineno);
        /* leave_scope param-accessed marking (symboltable.py:268-277, the OLD
         * parser's parser.c:7866-7882): BEFORE exit_scope, force-mark every
         * unaccessed PARAMETER accessed ("Parameters must always be present
         * even if not used") and emit W150 ("Parameter '<id>' is never used")
         * for a non-byref unaccessed param — UNLESS the header was rejected
         * (Python returned before leave_scope). Without this, an unused string
         * param is dropped by the O>1 Scope.values(filter_by_opt) filter and
         * its stdcall MEM_FREE teardown is skipped (paramstr3/4/5, pararray*,
         * byrefbyref, subparam, …). */
        {
            Scope_ *bs = p->cs->symbol_table->current_scope;
            for (int si = 0; si < bs->ordered_count; si++) {
                AstNode *e = bs->ordered[si];
                if (e && e->tag == AST_ID &&
                    e->u.id.scope == SCOPE_parameter && !e->u.id.accessed) {
                    if (!fd->rejected && !e->u.id.byref)
                        warn_not_used(p->cs, e->lineno,
                                      e->u.id.name ? e->u.id.name : "",
                                      "Parameter");
                    e->u.id.accessed = true;
                }
            }
        }
        /* leave scope: snapshot local_entries AFTER exit_scope compaction,
         * then compute_offsets (production parser.c:7841-7869). */
        Scope_ *body_scope = p->cs->symbol_table->current_scope;
        symboltable_exit_scope(p->cs->symbol_table);
        if (p->cs->function_level.len > 0)
            vec_pop(p->cs->function_level);
        AstNode *id_node = fd->id_node;
        if (id_node) {
            int oc = body_scope->ordered_count;
            if (oc > 0) {
                AstNode **le = arena_alloc(&p->cs->arena, (size_t)oc * sizeof(AstNode *));
                memcpy(le, body_scope->ordered, (size_t)oc * sizeof(AstNode *));
                id_node->u.id.local_entries = le;
                id_node->u.id.local_entries_count = oc;
            }
            id_node->u.id.local_size = symboltable_compute_offsets(
                p->cs->symbol_table, body_scope, p->cs->opts.optimization_level);
        }
        if (fd->rejected) { r = make_nop(p); break; }
        /* Empty bodies now match: the production body loop skips NOPs
         * (parser.c:7770), so an empty/inline-`:` function body is 0-child in
         * both paths (== Python's make_block()). */
        /* p_funcdecl (zxbparser.py:2914): entry.ref.forwarded = False — the
         * definition un-forwards a previously-DECLAREd entry. */
        id_node->u.id.forwarded = false;
        AstNode *decl = ast_new(p->cs, AST_FUNCDECL, fd->lineno);
        ast_add_child(p->cs, decl, id_node);
        ast_add_child(p->cs, decl, fd->params);
        ast_add_child(p->cs, decl, body);
        decl->type_ = fd->ret_type;
        id_node->u.id.body = body;
        vec_push(p->cs->functions, id_node);
        r = decl;
        break;
    }
    case 325: { /* function_declaration : DECLARE function_header_pre
                 * (p_funcdeclforward): mark forwarded, leave scope (no
                 * warnings) + pop FUNCTION_LEVEL. function_def already
                 * entered the scope. The production builds an is_forward
                 * FUNCDECL (empty body) and pushes functions (parser.c:
                 * 7552-7572); Python returns None but the C-vs-C compare is
                 * against the production, so match it. */
        if (c->unwired) { r = make_nop(p); break; }
        PdFuncDef *fd = (PdFuncDef *)rhs[1].value;
        symboltable_exit_scope(p->cs->symbol_table);
        if (p->cs->function_level.len > 0)
            vec_pop(p->cs->function_level);
        if (fd->rejected) { r = make_nop(p); break; }
        AstNode *id_node = fd->id_node;
        id_node->u.id.forwarded = true;
        AstNode *decl = ast_new(p->cs, AST_FUNCDECL, fd->lineno);
        decl->u.funcdecl.is_forward = true;
        AstNode *body = make_block_node(p, fd->lineno);
        ast_add_child(p->cs, decl, id_node);
        ast_add_child(p->cs, decl, fd->params);
        ast_add_child(p->cs, decl, body);
        decl->type_ = fd->ret_type;
        vec_push(p->cs->functions, id_node);
        r = decl;
        break;
    }
    case 55: /* statement : function_declaration (p_staement_func_decl) */
        r = PD_NODE(1);
        break;

    /* ---- IF subsystem (no-else forms first) ----
     * if_then_part : IF expr then (129) -> the condition expr. (The
     * always-true/false warning is stderr-only and the production parser does
     * not emit it, so omit it here to match the C-vs-C baseline; Phase C.) */
    case 129:
        r = PD_NODE(2); /* the condition; NULL propagates */
        break;
    case 132: case 133: /* then : <empty> | THEN -> no value */
        *out = NULL;
        *out_lineno = (len > 0) ? PD_LINENO(1) : p->lexer.lineno;
        return true;
    case 95: case 97: /* endif : END IF | ENDIF -> NOP (p_endif) */
        r = make_nop(p);
        break;
    case 96: case 98: /* endif : label END IF | label ENDIF -> the label */
        r = PD_NODE(1);
        break;
    case 90: case 92: case 93: { /* statement : if_then_part NEWLINE
                       * program_co|statements_co|co_statements_co endif
                       * (p_if_sentence): SENTENCE("IF", cond, pd_if_body(stat,
                       * endif)). pd_if_body one-level-flattens to match the
                       * production (body NOPs now skipped both sides — batch 18).
                       * Only a LABEL in the body still diverges (program_co/
                       * label_line flatten vs production's nested compound,
                       * byte-clean — ifthen); defer that -> UNWIRED. */
        AstNode *body = pd_if_body(p, PD_NODE(3), PD_NODE(4));
        if (pd_block_has_label(body)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "IF", PD_LINENO(2));
        ast_add_child(p->cs, r, PD_NODE(1));
        ast_add_child(p->cs, r, body);
        break;
    }
    case 91: /* statement : if_then_part NEWLINE endif (empty body) */
        r = make_sentence_node(p, "IF", PD_LINENO(2));
        ast_add_child(p->cs, r, PD_NODE(1));
        ast_add_child(p->cs, r, pd_if_body(p, NULL, PD_NODE(3)));
        break;
    case 94: /* statement : if_then_part NEWLINE label statements_co endif
              * (len==6, label body) — label always present, defer. */
        c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        r = make_nop(p);
        break;
    case 100: case 101: { /* statement : if_then_part statements_co|
                          * co_statements_co endif (same-line). */
        AstNode *body = pd_if_body(p, PD_NODE(2), PD_NODE(3));
        if (pd_block_has_label(body)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "IF", PD_LINENO(1));
        ast_add_child(p->cs, r, PD_NODE(1));
        ast_add_child(p->cs, r, body);
        break;
    }
    case 102: case 103: case 104: case 105: {
        /* if_inline : if_then_part statements|co_statements_co|statements_co|
         * co_statements (p_if_inline). SENTENCE("IF", cond, pd_if_body(stat)).
         * No endif. Label-in-body deferred (nesting reason). */
        AstNode *body = pd_if_body(p, PD_NODE(2), NULL);
        if (pd_block_has_label(body)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "IF", PD_LINENO(1));
        ast_add_child(p->cs, r, PD_NODE(1));
        ast_add_child(p->cs, r, body);
        break;
    }
    case 99: /* statement : if_inline (no else) -> the IF SENTENCE */
        r = PD_NODE(1);
        break;

    /* ---- IF else / elseif subsystem ----
     * Match the PRODUCTION parser's else/elseif tree shapes (parse_if_statement,
     * parser.c:5572-5658), which coincide with Python's make_block flattening
     * for the no-label cases. LABEL-bearing else/elseif forms carry the
     * label_line/program_co nesting divergence (Phase-E-reconcile, byte-clean)
     * and are deferred -> UNWIRED, like the simple IF body cases.
     *
     * else_part_inline / else_part carry a list [else_, endif] (Python:
     * zxbparser.py:1470-1511). Build it as a PdElse element list; the IF/elseif
     * consumers flatten it via make_block(*list). */
    case 113: case 114: { /* ELSE NEWLINE program_co|statements_co endif
                           * (p[2]=="\n", len==5) -> [p[3], p[4]]. */
        PdElse *pe = arena_alloc(&p->cs->arena, sizeof(PdElse));
        pe->e[0] = PD_NODE(3); pe->e[1] = PD_NODE(4); pe->n = 2;
        *out = pe; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 115: /* ELSE NEWLINE co_statements_co endif: a leading `:` in the
               * co_statements_co else body is a NOP the PRODUCTION's else loop
               * KEEPS but the engine's make_block drops (co_statements case 14
               * filters it before this reduce sees it) — a byte-clean nesting
               * divergence (Phase-E-reconcile). The engine matches Python here;
               * defer to hold DIFF 0 (ifthensntcoelsecocoendif). */
    case 120: case 122: case 123: /* same co_statements_co else-body class */
        c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        *out = NULL; *out_lineno = PD_LINENO(1);
        return true;
    case 116: { /* ELSE NEWLINE endif (len==4) -> [make_nop(), p[3]] */
        PdElse *pe = arena_alloc(&p->cs->arena, sizeof(PdElse));
        pe->e[0] = make_nop(p); pe->e[1] = PD_NODE(3); pe->n = 2;
        *out = pe; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 117: case 118: /* ELSE NEWLINE label ... endif -> HAS LABEL, defer */
        c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        *out = NULL; *out_lineno = PD_LINENO(1);
        return true;
    case 119: { /* ELSE statements_co endif (p[2] != "\n") -> [p[2], p[3]]. */
        PdElse *pe = arena_alloc(&p->cs->arena, sizeof(PdElse));
        pe->e[0] = PD_NODE(2); pe->e[1] = PD_NODE(3); pe->n = 2;
        *out = pe; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 121: case 124: { /* p_else_part: ELSE statements_co|statements
                           * -> [p[2], make_nop()]. (co_statements forms
                           * 122/123 deferred above — leading-`:` NOP class.) */
        PdElse *pe = arena_alloc(&p->cs->arena, sizeof(PdElse));
        pe->e[0] = PD_NODE(2); pe->e[1] = make_nop(p); pe->n = 2;
        *out = pe; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 125: /* else_part : else_part_inline (p_else_part_is_inline) -> pass */
        *out = rhs[0].value; *out_lineno = PD_LINENO(1);
        return true;
    case 126: case 127: case 128: /* else_part : label ELSE ... -> HAS LABEL */
        c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        *out = NULL; *out_lineno = PD_LINENO(1);
        return true;

    /* elseif_expr : ELSEIF expr then (108) | label ELSEIF expr then (109).
     * Carry (label_, cond_). The label form (109) is deferred. */
    case 108: { /* ELSEIF expr then -> (NOP, cond) */
        PdElseif *pe = arena_alloc(&p->cs->arena, sizeof(PdElseif));
        pe->label = make_nop(p); pe->cond = PD_NODE(2); pe->lineno = PD_LINENO(1);
        *out = pe; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 109: /* label ELSEIF expr then -> HAS LABEL, defer */
        c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        *out = NULL; *out_lineno = PD_LINENO(1);
        return true;

    /* elseiflist : elseif_expr program_co endif (110)
     *            | elseif_expr program_co else_part (111)
     *            | elseif_expr program_co elseiflist (112)
     * (p_elseif_list / p_elseif_elseiflist). Build make_block(label_,
     * IF(cond_, then_, else_)) — label_ NOP so the block unwraps to the IF. */
    case 110: case 111: case 112: {
        PdElseif *pe = (PdElseif *)rhs[0].value;
        if (!pe) { /* upstream label-elseif deferred */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        AstNode *then_ = PD_NODE(2);    /* program_co */
        AstNode *else_node = NULL;
        if (prodno == 110) {            /* ... endif: then_=make_block(then_,endif) */
            AstNode *endif = PD_NODE(3);
            then_ = pd_if_body(p, then_, endif);
            else_node = NULL;
        } else if (prodno == 111) {     /* ... else_part: else_=make_block(*list) */
            PdElse *el = (PdElse *)rhs[2].value;
            if (!el || pd_block_has_nop(el->e[0])) { /* deferred (label / leading-`:` NOP) */
                c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
                r = make_nop(p); break;
            }
            then_ = pd_if_body(p, then_, NULL);
            else_node = pd_make_block_n(p, el->e, el->n);
        } else {                        /* 112: ... elseiflist (nested) */
            AstNode *nested = PD_NODE(3);
            if (!nested || nested->tag == AST_NOP) {
                c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
                r = make_nop(p); break;
            }
            then_ = pd_if_body(p, then_, NULL);
            else_node = nested;
        }
        AstNode *nif = make_sentence_node(p, "IF", pe->lineno);
        if (pe->cond) ast_add_child(p->cs, nif, pe->cond);
        ast_add_child(p->cs, nif, then_);
        if (else_node) ast_add_child(p->cs, nif, else_node);
        if (pd_block_has_label(then_) || pd_block_has_label(else_node)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = nif; /* make_block(NOP, IF) unwraps to the IF sentence */
        break;
    }

    /* statement : if_then_part NEWLINE program_co elseiflist (106)
     *           | if_then_part NEWLINE elseiflist (107)  (p_if_elseif)
     * IF(cond_, stats_, eliflist). */
    case 106: case 107: {
        AstNode *cond_ = PD_NODE(1);
        AstNode *stats_, *eliflist;
        /* The production always builds a (possibly-empty) BLOCK for the then-
         * part (parse_if_statement:5575), NOT Python's make_nop — match it. */
        if (prodno == 106) { stats_ = pd_if_body(p, PD_NODE(3), NULL); eliflist = PD_NODE(4); }
        else               { stats_ = make_block_node(p, PD_LINENO(2)); eliflist = PD_NODE(3); }
        if (!eliflist || eliflist->tag == AST_NOP) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        if (pd_block_has_label(stats_)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "IF", PD_LINENO(2));
        if (cond_) ast_add_child(p->cs, r, cond_);
        ast_add_child(p->cs, r, stats_);
        ast_add_child(p->cs, r, eliflist);
        break;
    }

    /* statement : if_then_part NEWLINE program_co else_part (131, p_if_else).
     * IF(cond_, then_, make_block(else_, endif)) where else_=p[4][0],
     * endif=p[4][1]. */
    case 131: {
        AstNode *cond_ = PD_NODE(1);
        AstNode *then_ = pd_if_body(p, PD_NODE(3), NULL);
        PdElse *el = (PdElse *)rhs[3].value;
        if (!el || pd_block_has_nop(el->e[0])) { /* deferred (label / leading-`:` NOP) */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        AstNode *else_block = pd_make_block2(p, el->e[0], el->n > 1 ? el->e[1] : NULL);
        if (pd_block_has_label(then_) || pd_block_has_label(else_block)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "IF", PD_LINENO(2));
        if (cond_) ast_add_child(p->cs, r, cond_);
        ast_add_child(p->cs, r, then_);
        ast_add_child(p->cs, r, else_block);
        break;
    }

    /* statement : if_inline else_part_inline (130, p_if_inline).
     * p[1].append_child(make_block(p[2][0], p[2][1])); p[0] = p[1]. */
    case 130: {
        AstNode *ifn = PD_NODE(1);
        PdElse *el = (PdElse *)rhs[1].value;
        if (!ifn || ifn->tag == AST_NOP || !el || pd_block_has_nop(el->e[0])) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        AstNode *else_block = pd_make_block2(p, el->e[0], el->n > 1 ? el->e[1] : NULL);
        if (pd_block_has_label(else_block) || pd_block_has_label(ifn)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        ast_add_child(p->cs, ifn, else_block);
        r = ifn;
        break;
    }

    /* ---- FOR subsystem ----
     * step : <empty> (142) -> make_number(1); STEP expr (143) -> expr. */
    case 142:
        r = make_number(p, 1, p->lexer.lineno, NULL);
        break;
    case 143:
        r = PD_NODE(2);
        break;
    case 141: { /* for_start : FOR ID EQ expr TO expr step (p_for_sentence_start)
                 * push loop_stack; resolve var (common_type); build
                 * SENTENCE("FOR", var, e1, e2, e3). Faithful to parser.c:
                 * 5662-5773 (var resolution + typecasts + FOR sentence). The
                 * body is appended at for_sentence. (always-loop warnings are
                 * stderr-only / the production omits them — Phase C.) */
        const char *var_name = PD_SVAL(2);
        int var_lineno = PD_LINENO(2);
        int for_ln = PD_LINENO(1);
        AstNode *start_expr = PD_NODE(4), *end_expr = PD_NODE(6), *step_expr = PD_NODE(7);
        TypeInfo *id_type = check_common_type(p->cs, start_expr, end_expr);
        if (id_type && step_expr) {
            AstNode *tc = ast_new(p->cs, AST_NUMBER, for_ln);
            tc->type_ = id_type;
            id_type = check_common_type(p->cs, tc, step_expr);
        }
        AstNode *var = symboltable_access_id(p->cs->symbol_table, p->cs,
                                             var_name, var_lineno, id_type, CLASS_var);
        if (var) {
            if (var->u.id.class_ == CLASS_const)
                zxbc_error(p->cs, var_lineno, "'%s' is a CONST, not a VAR", var_name);
            else if (var->u.id.class_ == CLASS_function)
                zxbc_error(p->cs, var_lineno, "'%s' is a FUNCTION, not a VAR", var_name);
            else if (var->u.id.class_ == CLASS_sub)
                zxbc_error(p->cs, var_lineno, "Cannot assign a value to '%s'. It's not a variable", var_name);
            if (var->u.id.class_ == CLASS_unknown) var->u.id.class_ = CLASS_var;
            var->u.id.accessed = true;
        }
        LoopInfo li = { LOOP_FOR, for_ln, arena_strdup(&p->cs->arena, var_name) };
        vec_push(p->cs->loop_stack, li);
        AstNode *s = make_sentence_node(p, "FOR", for_ln);
        if (var) {
            start_expr = make_typecast(p->cs, var->type_, start_expr, var_lineno);
            end_expr = make_typecast(p->cs, var->type_, end_expr, var_lineno);
            step_expr = make_typecast(p->cs, var->type_, step_expr, var_lineno);
            ast_add_child(p->cs, s, var);
        } else {
            AstNode *fb = ast_new(p->cs, AST_ID, for_ln);
            fb->u.id.name = arena_strdup(&p->cs->arena, var_name);
            ast_add_child(p->cs, s, fb);
        }
        if (start_expr) ast_add_child(p->cs, s, start_expr);
        if (end_expr) ast_add_child(p->cs, s, end_expr);
        if (step_expr) ast_add_child(p->cs, s, step_expr);
        r = s;
        break;
    }
    case 137: case 139: /* label_next : label NEXT | label NEXT ID -> the label
                         * (+ NEXT-var check for 139). */
        if (prodno == 139 && p->cs->loop_stack.len > 0) {
            const char *nv = PD_SVAL(3);
            const char *fv = p->cs->loop_stack.data[p->cs->loop_stack.len - 1].var_name;
            if (nv && fv && strcmp(fv, nv) != 0)
                err_wrong_for_var(p->cs, PD_LINENO(3), fv, nv);
        }
        r = PD_NODE(1);
        break;
    case 138: /* label_next : NEXT -> NOP */
        r = make_nop(p);
        break;
    case 140: /* label_next : NEXT ID -> NOP (+ NEXT-var check) */
        if (p->cs->loop_stack.len > 0) {
            const char *nv = PD_SVAL(2);
            const char *fv = p->cs->loop_stack.data[p->cs->loop_stack.len - 1].var_name;
            if (nv && fv && strcmp(fv, nv) != 0)
                err_wrong_for_var(p->cs, PD_LINENO(2), fv, nv);
        }
        r = make_nop(p);
        break;
    case 134: case 135: case 136: { /* statement : for_start
                       * program_co|co_statements_co|program label_next
                       * (p_for_sentence): append body make_block(p[2], p[3]);
                       * pop loop_stack. */
        AstNode *forsent = PD_NODE(1);
        AstNode *body = pd_if_body(p, PD_NODE(2), PD_NODE(3));
        if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
        if (!forsent || forsent->tag != AST_SENTENCE) { r = forsent; break; }
        /* Body NOPs now skipped in the production loop too (batch 18); only the
         * label-compound nesting still diverges — defer that. */
        if (pd_block_has_label(body)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        ast_add_child(p->cs, forsent, body);
        r = forsent;
        break;
    }

    /* ---- WHILE subsystem ----
     * while_start : WHILE expr (181) -> the cond; push loop_stack(WHILE).
     * Carry the WHILE-keyword line as the result lineno (p_while_sentence uses
     * p.lineno(1) == while_start's line == WHILE token). */
    case 181: {
        LoopInfo li = { LOOP_WHILE, PD_LINENO(1), NULL };
        vec_push(p->cs->loop_stack, li);
        *out = rhs[1].value;       /* the cond expr */
        *out_lineno = PD_LINENO(1); /* WHILE token line */
        return true;
    }
    case 175: case 176: /* label_end_while : label WEND | label END WHILE -> label */
        r = PD_NODE(1);
        break;
    case 177: case 178: /* label_end_while : WEND | END WHILE -> NULL */
        *out = NULL;
        *out_lineno = PD_LINENO(1);
        return true;
    case 179: case 180: { /* statement : while_start co_statements_co|program_co
                          * label_end_while (p_while_sentence): pop loop_stack;
                          * make_sentence("WHILE", cond, make_block(body, lew)).
                          * (always-cond warning omitted, as production.) */
        AstNode *cond = PD_NODE(1);
        AstNode *body = pd_if_body(p, PD_NODE(2), PD_NODE(3));
        if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
        if (pd_block_has_label(body)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "WHILE", PD_LINENO(1));
        ast_add_child(p->cs, r, cond);
        ast_add_child(p->cs, r, body);
        break;
    }

    /* ---- DO loop subsystem ----
     * NOTE: the C-vs-C compare is against the PRODUCTION parser, whose DO node
     * kinds + child order DIFFER from Python's (production: DO_LOOP[body] /
     * LOOP_UNTIL[body,cond] / LOOP_WHILE[body,cond] / DO_WHILE[cond,body] /
     * DO_UNTIL[cond,body]; Python uses DO_LOOP/DO_UNTIL/DO_WHILE/WHILE_DO/
     * UNTIL_DO with cond-first). Match the production (parser.c:6017-6020). */
    case 173: case 174: { /* do_start : DO CO | DO NEWLINE -> push loop_stack(DO) */
        LoopInfo li = { LOOP_DO, PD_LINENO(1), NULL };
        vec_push(p->cs->loop_stack, li);
        *out = NULL; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 171: case 172: { /* do_while_start: DO WHILE expr | do_until_start:
                          * DO UNTIL expr -> cond; push loop_stack(DO). */
        LoopInfo li = { LOOP_DO, PD_LINENO(1), NULL };
        vec_push(p->cs->loop_stack, li);
        *out = rhs[2].value; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 149: /* label_loop : label LOOP -> the label */
        r = PD_NODE(1);
        break;
    case 150: /* label_loop : LOOP -> NOP */
        r = make_nop(p);
        break;
    case 151: case 152: case 153: { /* statement : do_start program_co label_loop
                       * | do_start label_loop | DO label_loop (p_do_loop):
                       * "DO_LOOP"[body]. For 153 (bare DO) push here. */
        int doln = PD_LINENO(1);
        AstNode *body;
        if (prodno == 151) body = pd_if_body(p, PD_NODE(2), PD_NODE(3));
        else body = pd_if_body(p, PD_NODE(2), NULL);
        if (prodno == 153) { LoopInfo li = {LOOP_DO, doln, NULL}; vec_push(p->cs->loop_stack, li); }
        if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
        if (pd_block_has_label(body)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, "DO_LOOP", doln);
        ast_add_child(p->cs, r, body);
        break;
    }
    case 154: case 155: case 156: /* do...LOOP UNTIL -> "LOOP_UNTIL"[body,cond] */
    case 162: case 163: case 164: { /* do...LOOP WHILE -> "LOOP_WHILE"[body,cond] */
        bool is_until = (prodno <= 156);
        int doln = PD_LINENO(1);
        bool bare = (prodno == 156 || prodno == 164);
        bool has_prog = (prodno == 154 || prodno == 162);
        AstNode *body, *cond;
        if (has_prog) { body = pd_if_body(p, PD_NODE(2), PD_NODE(3)); cond = PD_NODE(5); }
        else { body = pd_if_body(p, PD_NODE(2), NULL); cond = PD_NODE(4); }
        if (bare) { LoopInfo li = {LOOP_DO, doln, NULL}; vec_push(p->cs->loop_stack, li); }
        if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
        /* NULL cond == a semantic error in the post-test expr (do_crash:
         * `LOOP WHILE A=""`); error-path AST shapes diverge — defer. */
        if (pd_block_has_label(body) || !cond) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, is_until ? "LOOP_UNTIL" : "LOOP_WHILE", doln);
        ast_add_child(p->cs, r, body);
        ast_add_child(p->cs, r, cond);
        break;
    }
    case 165: case 166: case 167: /* DO WHILE...LOOP -> "DO_WHILE"[cond,body] */
    case 168: case 169: case 170: { /* DO UNTIL...LOOP -> "DO_UNTIL"[cond,body] */
        bool is_until = (prodno >= 168);
        int doln = PD_LINENO(1);
        AstNode *cond = PD_NODE(1); /* do_while_start/do_until_start value */
        bool has_body = (prodno == 165 || prodno == 166 || prodno == 168 || prodno == 169);
        AstNode *body = has_body ? pd_if_body(p, PD_NODE(2), NULL) : make_block_node(p, doln);
        if (p->cs->loop_stack.len > 0) vec_pop(p->cs->loop_stack);
        if (pd_block_has_label(body) || !cond) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        r = make_sentence_node(p, is_until ? "DO_UNTIL" : "DO_WHILE", doln);
        ast_add_child(p->cs, r, cond);
        ast_add_child(p->cs, r, body);
        break;
    }

    /* RETURN (214 p_return) | RETURN expr (215 p_return_expr). Faithful to
     * parser.c:3763-3848 (which itself ports the Python actions). */
    case 214: { /* statement : RETURN */
        AstNode *func = (p->cs->function_level.len > 0)
                ? p->cs->function_level.data[p->cs->function_level.len - 1] : NULL;
        int ln = PD_LINENO(1);
        if (!func) { r = make_sentence_node(p, "RETURN", ln); break; }
        if (func->u.id.class_ != CLASS_sub) {
            zxbc_error(p->cs, ln, "Syntax Error: Function must RETURN a value.");
            r = NULL; break;
        }
        r = make_sentence_node(p, "RETURN", ln);
        ast_add_child(p->cs, r, func);
        break;
    }
    case 215: { /* statement : RETURN expr */
        AstNode *func = (p->cs->function_level.len > 0)
                ? p->cs->function_level.data[p->cs->function_level.len - 1] : NULL;
        int ln = PD_LINENO(1);
        AstNode *expr = PD_NODE(2);
        if (!func) {
            zxbc_error(p->cs, ln, "Syntax Error: Returning value out of FUNCTION");
            r = NULL; break;
        }
        if (func->u.id.class_ == CLASS_unknown) { r = NULL; break; }
        if (func->u.id.class_ != CLASS_function) {
            zxbc_error(p->cs, ln, "Syntax Error: SUBs cannot return a value");
            r = NULL; break;
        }
        if (func->type_ == NULL) { r = NULL; break; }
        if (expr) {
            bool es = type_is_string(expr->type_), fs = type_is_string(func->type_);
            if (!es && fs) {
                zxbc_error(p->cs, expr->lineno,
                           "Type Error: Function must return a string, not a numeric value");
                r = NULL; break;
            }
            if (es && !fs) {
                zxbc_error(p->cs, expr->lineno,
                           "Type Error: Function must return a numeric value, not a string");
                r = NULL; break;
            }
        }
        AstNode *cast = make_typecast(p->cs, func->type_, expr, ln);
        r = make_sentence_node(p, "RETURN", ln);
        ast_add_child(p->cs, r, func);
        if (cast) ast_add_child(p->cs, r, cast);
        break;
    }

    /* param_decl : <empty> | LP RP (337/338, p_param_decl_none) -> empty
     * PARAMLIST. */
    case 337: case 338:
        r = ast_new(p->cs, AST_PARAMLIST, p->lexer.lineno);
        break;
    case 339: /* param_decl : LP param_decl_list RP -> the PARAMLIST */
        r = PD_NODE(2);
        break;

    /* ---- parameters (scalar, no byval/array/sigil-conflict yet) ----
     * param_def : singleid typedef default_arg_value (347, p_param_def_type):
     * build the ARGUMENT node AND register the body-scope symbol (the scope is
     * already active — function_def entered it). Faithful to the production's
     * dual-node model (parser.c:7397 ARGUMENT + :7656 body symbol). Array
     * params (348) and sigil-typed params are deferred -> UNWIRED. */
    case 348: /* default_arg_value : <empty> -> NULL */
        *out = NULL;
        *out_lineno = p->lexer.lineno;
        return true;
    case 349: /* default_arg_value : EQ expr -> the expr */
        *out = rhs[1].value;
        *out_lineno = PD_LINENO(1);
        return true;
    case 346: { /* param_def : singleid LP RP typedef (p_param_def_array):
                 * array parameter. make_param_decl(id, typeref, is_array=True).
                 * Build ARGUMENT(is_array=true) + body symbol CLASS_array.
                 * (typeref NULL -> p[0]=None in Python; here type defaults like
                 * the scalar path. Sigil-named array params deferred.) */
        PdId *id = (PdId *)rhs[0].value;
        TypeInfo *td = (TypeInfo *)rhs[3].value;  /* typedef after LP RP */
        const char *pname = id->name;
        size_t pnl = strlen(pname);
        if (pnl > 0 && is_deprecated_suffix(pname[pnl - 1])) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        if (p->cs->opts.strict && td && td->implicit)
            err_undeclared_type(p->cs, id->lineno, pname);
        TypeInfo *ptype = td;
        if (!ptype) ptype = type_new_ref(p->cs, p->cs->default_type, id->lineno, true);
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, id->lineno);
        arg->u.argument.name = arena_strdup(&p->cs->arena, pname);
        arg->u.argument.is_array = true;
        arg->type_ = ptype;
        AstNode *bsym = symboltable_declare(p->cs->symbol_table, p->cs,
                                            pname, id->lineno, CLASS_array);
        if (bsym) {
            bsym->type_ = ptype;
            bsym->u.id.declared = true;
            bsym->u.id.scope = SCOPE_parameter;
        }
        PdParam *pp = arena_alloc(&p->cs->arena, sizeof(PdParam));
        pp->arg = arg; pp->body_sym = bsym;
        *out = pp;
        *out_lineno = id->lineno;
        return true;
    }
    case 347: {
        PdId *id = (PdId *)rhs[0].value;
        TypeInfo *td = (TypeInfo *)rhs[1].value;   /* typedef (NULL = implicit) */
        AstNode *defval = (AstNode *)rhs[2].value; /* default_arg_value */
        const char *pname = id->name;
        /* check_type_is_explicit (strict-only) */
        if (p->cs->opts.strict && td && td->implicit)
            err_undeclared_type(p->cs, id->lineno, pname);
        TypeInfo *ptype = td;
        if (!ptype)
            ptype = type_new_ref(p->cs, p->cs->default_type, id->lineno, true);
        /* A deprecated sigil ($/%/&/!) overrides the declared param type
         * (SymbolTable.declare, symboltable.py:102-123), matching the
         * production parser (parser.c:7401-7441): a$ => string, i% =>
         * integer, etc. If the param carries a non-implicit explicit type
         * differing from the sigil, emit the conflict diagnostic against the
         * ORIGINAL type, then adopt the sigil type (Python does NOT abort).
         * Resolving it HERE puts the corrected type on the ARGUMENT node
         * before parser_assign_param_offsets (at function_header_pre, 330)
         * sizes it. The body-scope symbol is registered under the STRIPPED
         * name (declare strips the suffix for entry.name); case 330's body-
         * symbol lookup also strips, so they reconcile. */
        size_t pnl = strlen(pname);
        const char *bname = pname;       /* body-scope key (suffix stripped) */
        char bbuf[256];
        if (pnl > 0 && pnl < sizeof(bbuf) && is_deprecated_suffix(pname[pnl - 1])) {
            BasicType sbt = suffix_to_type(pname[pnl - 1]);
            TypeInfo *sti = p->cs->symbol_table->basic_types[sbt];
            if (ptype && !ptype->implicit && !type_equal(ptype, sti))
                zxbc_error(p->cs, id->lineno, "expected type %s for '%s', got %s",
                           sti->name, pname, ptype->name);
            ptype = sti;
            memcpy(bbuf, pname, pnl - 1); bbuf[pnl - 1] = '\0'; bname = bbuf;
        }
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, id->lineno);
        arg->u.argument.name = arena_strdup(&p->cs->arena, pname);
        arg->u.argument.is_array = false;
        arg->type_ = ptype;
        if (defval) {
            defval = make_typecast(p->cs, ptype, defval, id->lineno);
            if (defval) ast_add_child(p->cs, arg, defval);
        }
        /* Register the body-scope symbol (declare_param) under the stripped name. */
        AstNode *bsym = symboltable_declare(p->cs->symbol_table, p->cs,
                                            bname, id->lineno, CLASS_var);
        if (bsym) {
            bsym->type_ = ptype;
            bsym->u.id.declared = true;
            bsym->u.id.scope = SCOPE_parameter;
        }
        PdParam *pp = arena_alloc(&p->cs->arena, sizeof(PdParam));
        pp->arg = arg; pp->body_sym = bsym;
        *out = pp;
        *out_lineno = id->lineno;
        return true;
    }
    case 345: { /* param_definition : param_def (p_param_definition) ->
                 * array param byref=True; else byref=default_byref. */
        PdParam *pp = (PdParam *)rhs[0].value;
        if (pp && pp->arg)
            pp->arg->u.argument.byref = pp->arg->u.argument.is_array
                                            ? true : p->cs->opts.default_byref;
        *out = pp; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 343: { /* param_definition : BYREF param_def -> byref = true */
        PdParam *pp = (PdParam *)rhs[1].value;
        if (pp && pp->arg) pp->arg->u.argument.byref = true;
        *out = pp; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 344: { /* param_definition : BYVAL param_def -> byref = false
                 * (array-byval rejection deferred -> UNWIRED via the array
                 * param being UNWIRED already). */
        PdParam *pp = (PdParam *)rhs[1].value;
        if (pp && pp->arg) pp->arg->u.argument.byref = false;
        *out = pp; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 341: { /* param_decl_list : param_definition -> PARAMLIST[arg] */
        PdParam *pp = (PdParam *)rhs[0].value;
        AstNode *pl = ast_new(p->cs, AST_PARAMLIST, PD_LINENO(1));
        if (pp && pp->arg) ast_add_child(p->cs, pl, pp->arg);
        r = pl;
        break;
    }
    case 342: { /* param_decl_list : param_decl_list COMMA param_definition
                 * mandatory-after-optional check (p_param_decl_list2). */
        AstNode *pl = (AstNode *)rhs[0].value;
        PdParam *pp = (PdParam *)rhs[2].value;
        if (pl && pp && pp->arg) {
            int n = pl->child_count;
            AstNode *prev = n > 0 ? pl->children[n - 1] : NULL;
            bool cur_has_default = (pp->arg->child_count > 0);
            bool prev_has_default = (prev && prev->child_count > 0);
            if (!cur_has_default && prev_has_default) {
                zxbc_error(p->cs, pp->arg->lineno,
                           "Can't declare mandatory param '%s' after optional param '%s'",
                           pp->arg->u.argument.name,
                           prev ? prev->u.argument.name : "");
            }
            ast_add_child(p->cs, pl, pp->arg);
        }
        r = pl;
        break;
    }

    /* function_body : ... END FUNCTION|SUB (350-357, p_function_body):
     * class-match check + return the body block. */
    case 350: case 351: case 352: case 353:
    case 354: case 355: case 356: case 357: {
        bool end_func = (prodno % 2 == 0); /* 350/352/354/356 END FUNCTION */
        /* class-match: FUNCTION_LEVEL[-1].class_ vs END FUNCTION/SUB */
        if (p->cs->function_level.len > 0) {
            AstNode *top = p->cs->function_level.data[p->cs->function_level.len - 1];
            bool is_func_class = (top->u.id.class_ == CLASS_function);
            int end_ln = PD_LINENO(len); /* END's line ~ last token */
            if (is_func_class != end_func) {
                zxbc_error(p->cs, end_ln,
                           "Unexpected token 'END %s'. Should be 'END %s'",
                           end_func ? "FUNCTION" : "SUB",
                           is_func_class ? "FUNCTION" : "SUB");
            }
        }
        /* body block: bare END -> empty BLOCK; else p[1]. */
        if (prodno == 356 || prodno == 357)
            r = make_block_node(p, p->lexer.lineno);
        else
            r = PD_NODE(1);
        break;
    }

    /* ---- DIM / var_decl declaration subsystem ----
     * The scalar var_decl forms reuse dim_build_scalar (extracted from the
     * production parser's parse_dim_statement) so the tree + symbol-table
     * state are byte-identical to the production parser by construction.
     * Supporting productions: singleid/idlist (Id list), type/typedef. */
    case 35: case 36: /* singleid : ID | ARRAY_ID -> Id(name,lineno) */
        *out = pd_new_id(p, PD_SVAL(1), PD_LINENO(1));
        *out_lineno = PD_LINENO(1);
        return true;
    case 37: /* idlist : singleid -> [singleid] */
        *out = pd_new_idlist(p, (PdId *)rhs[0].value);
        *out_lineno = PD_LINENO(1);
        return true;
    case 38: { /* idlist : idlist COMMA singleid -> append */
        PdIdList *l = (PdIdList *)rhs[0].value;
        pd_idlist_append(p, l, (PdId *)rhs[2].value);
        *out = l;
        *out_lineno = PD_LINENO(1);
        return true;
    }
    case 360: case 361: case 362: case 363: case 364:
    case 365: case 366: case 367: case 368: { /* type : <basic-type> */
        BasicType bt =
            (prodno == 360) ? TYPE_byte :
            (prodno == 361) ? TYPE_ubyte :
            (prodno == 362) ? TYPE_integer :
            (prodno == 363) ? TYPE_uinteger :
            (prodno == 364) ? TYPE_long :
            (prodno == 365) ? TYPE_ulong :
            (prodno == 366) ? TYPE_fixed :
            (prodno == 367) ? TYPE_float : TYPE_string;
        TypeInfo *t = type_new_ref(p->cs, st->basic_types[bt], PD_LINENO(1), false);
        *out = t;
        *out_lineno = PD_LINENO(1);
        return true;
    }
    case 247: case 248: case 249: case 250: case 251:
    case 252: case 253: case 254: { /* numbertype : <basic numeric type>
                                     * (p_numbertype) -> make_type. */
        BasicType bt =
            (prodno == 247) ? TYPE_byte :
            (prodno == 248) ? TYPE_ubyte :
            (prodno == 249) ? TYPE_integer :
            (prodno == 250) ? TYPE_uinteger :
            (prodno == 251) ? TYPE_long :
            (prodno == 252) ? TYPE_ulong :
            (prodno == 253) ? TYPE_fixed : TYPE_float;
        TypeInfo *t = type_new_ref(p->cs, st->basic_types[bt], PD_LINENO(1), false);
        *out = t;
        *out_lineno = PD_LINENO(1);
        return true;
    }
    case 358: /* typedef : <empty> — NULL so dim_build_scalar infers (matches
               * the production parser, where parse_typedef returns NULL for no
               * AS clause). had_as_clause is derived as (typedef != NULL). */
        *out = NULL;
        *out_lineno = p->lexer.lineno;
        return true;
    case 359: /* typedef : AS type -> the type (non-implicit) */
        *out = rhs[1].value;
        *out_lineno = PD_LINENO(2);
        return true;

    case 31: { /* var_decl : DIM idlist typedef (p_var_decl) */
        PdIdList *l = (PdIdList *)rhs[1].value;
        TypeInfo *type = (TypeInfo *)rhs[2].value;
        const char *names[64];
        int n = l->count > 64 ? 64 : l->count;
        for (int i = 0; i < n; i++) names[i] = l->ids[i]->name;
        r = dim_build_scalar(p, names, n, names[0], type, type != NULL,
                             NULL, NULL, false, PD_LINENO(1));
        break;
    }
    case 32: { /* var_decl : DIM idlist typedef AT expr (p_var_decl_at) */
        PdIdList *l = (PdIdList *)rhs[1].value;
        TypeInfo *type = (TypeInfo *)rhs[2].value;
        const char *names[64];
        int n = l->count > 64 ? 64 : l->count;
        for (int i = 0; i < n; i++) names[i] = l->ids[i]->name;
        r = dim_build_scalar(p, names, n, names[0], type, type != NULL,
                             PD_NODE(5), NULL, false, PD_LINENO(1));
        break;
    }
    case 33: case 34: { /* var_decl : DIM|CONST idlist typedef EQ expr */
        PdIdList *l = (PdIdList *)rhs[1].value;
        TypeInfo *type = (TypeInfo *)rhs[2].value;
        const char *names[64];
        int n = l->count > 64 ? 64 : l->count;
        for (int i = 0; i < n; i++) names[i] = l->ids[i]->name;
        r = dim_build_scalar(p, names, n, names[0], type, type != NULL,
                             NULL, PD_NODE(5), prodno == 34, PD_LINENO(1));
        break;
    }

    /* ---- array DIM ----
     * bound : expr (p_bound) -> make_bound(make_number(array_base), p[1]);
     * bound : expr TO expr (p_bound_to_bound) -> make_bound(p[1], p[3]).
     * The C make_bound takes a pre-built BOUND node ([lower,upper]) and
     * validates it (returns NULL on a bad/non-constant bound). To match the
     * production parser, an invalid bound is dropped from the boundlist
     * (make_bound_list skips a NULL). */
    case 47: { /* bound : expr */
        AstNode *bd = ast_new(p->cs, AST_BOUND, PD_LINENO(1));
        AstNode *lo = make_number(p, p->cs->opts.array_base, PD_LINENO(1), NULL);
        ast_add_child(p->cs, bd, lo);
        ast_add_child(p->cs, bd, PD_NODE(1));
        *out = make_bound(p, bd, p->lexer.lineno); /* NULL if invalid */
        *out_lineno = PD_LINENO(1);
        return true;
    }
    case 48: { /* bound : expr TO expr */
        AstNode *bd = ast_new(p->cs, AST_BOUND, PD_LINENO(2));
        ast_add_child(p->cs, bd, PD_NODE(1));
        ast_add_child(p->cs, bd, PD_NODE(3));
        *out = make_bound(p, bd, PD_LINENO(2));
        *out_lineno = PD_LINENO(2);
        return true;
    }
    case 45: { /* bound_list : bound -> BOUNDLIST[bound] (skip NULL) */
        AstNode *bl = ast_new(p->cs, AST_BOUNDLIST, PD_LINENO(1));
        if (rhs[0].value) ast_add_child(p->cs, bl, (AstNode *)rhs[0].value);
        r = bl;
        break;
    }
    case 46: { /* bound_list : bound_list COMMA bound -> append (skip NULL) */
        AstNode *bl = (AstNode *)rhs[0].value;
        if (rhs[2].value) ast_add_child(p->cs, bl, (AstNode *)rhs[2].value);
        r = bl;
        break;
    }
    case 42: { /* var_arr_decl : DIM idlist LP bound_list RP typedef
                * (p_decl_arr): declare the array; the production parser
                * returns the ARRAYDECL node as the statement (parse_dim_
                * statement array path), so match that for the C-vs-C compare. */
        PdIdList *l = (PdIdList *)rhs[1].value;
        AstNode *bounds = (AstNode *)rhs[3].value;
        TypeInfo *type = (TypeInfo *)rhs[5].value;
        int ln = PD_LINENO(1);
        if (l->count != 1) {
            zxbc_error(p->cs, ln,
                       "Array declaration only allows one variable name at a time");
            r = NULL;
            break;
        }
        /* typedef NULL (no AS) -> infer from suffix / default, mirroring the
         * production array path (parse_typedef NULL -> the !type block). */
        const char *aname = l->ids[0]->name;
        if (!type) {
            size_t nl = strlen(aname);
            if (nl > 0 && is_deprecated_suffix(aname[nl - 1]))
                type = st->basic_types[suffix_to_type(aname[nl - 1])];
            else {
                type = type_new_ref(p->cs, p->cs->default_type, ln, true);
                if (p->cs->opts.strict)
                    zxbc_error(p->cs, ln,
                               "strict mode: missing type declaration for '%s'", aname);
            }
        }
        r = dim_build_array(p, aname, bounds, type, NULL, NULL, ln);
        break;
    }
    case 41: { /* var_arr_decl_addr : var_arr_decl AT expr (p_arr_decl_attr):
                * the array was already declared + built (ARRAYDECL) by
                * var_arr_decl (42); apply the AT address. The C PRODUCTION
                * array-AT path does NOT replicate Python's CONSTEXPR/is_static
                * address validation — it attaches the raw expr via
                * dim_build_array (parser.c:6515-6577 -> 6995-7001): add the AT
                * expr as a child, set id->addr_expr, and flip a local array's
                * scope to global (make_static is not modeled — parser.c:6925).
                * Reproduce that exact addendum on the already-built node for
                * C-vs-C identity. */
        AstNode *decl = (AstNode *)rhs[0].value;
        AstNode *at_expr = (AstNode *)rhs[2].value;
        if (!decl || !at_expr) {
            /* p_arr_decl_attr: arr_decl or expr None -> p[0] = None. */
            r = NULL; break;
        }
        ast_add_child(p->cs, decl, at_expr);
        AstNode *aid = decl->child_count > 0 ? decl->children[0] : NULL;
        if (aid && aid->tag == AST_ID) {
            aid->u.id.addr_expr = at_expr;
            if (aid->u.id.scope == SCOPE_local)
                aid->u.id.scope = SCOPE_global;
        }
        r = decl;
        break;
    }
    case 39: case 40: /* var_decl : var_arr_decl | var_arr_decl_addr */
        r = PD_NODE(1);
        break;

    /* ---- preproc_line subsystem (369-373) — side-effect-only, no AST node
     * (Python actions return None). Faithful to zxbparser.py:3227-3245. ---- */
    case 369: /* preproc_line : _INIT ID (p_preproc_line_init): INITS.add(p[2]) */
        vec_push(p->cs->inits, arena_strdup(&p->cs->arena, PD_SVAL(2)));
        r = NULL;
        break;
    case 370: /* preproc_line : _REQUIRE STRING (p_preproc_line_require):
               * REQUIRES.add(p[2]). The adapter delivers the preproc STRING
               * already quote-stripped via sval. */
        vec_push(p->cs->requires, arena_strdup(&p->cs->arena, PD_SVAL(2)));
        r = NULL;
        break;
    case 371: case 372: case 373: { /* preproc_line : _PRAGMA ID EQ ID|STRING|
                                     * INTEGER (p_preproc_line_pragma_option):
                                     * setattr(OPTIONS, p[2], p[4]). p[2]=NAME,
                                     * p[4]=value text (str in all 3 alts). */
        const char *pname = PD_SVAL(2);
        const char *pval = PD_SVAL(4);
        apply_pragma_option(p, pname, PD_LINENO(2), pval);
        /* `enable_break = true` makes the production insert CHKBREAK sentences
         * per program_line (the make_break spine) — not yet ported to the
         * engine program-line cases. Flag UNWIRED so those files stay honestly
         * gated (not DIFF) until the CHKBREAK spine is wired. */
        if (pname && strcmp(pname, "enable_break") == 0 && p->cs->opts.enable_break) {
            c->unwired = true;
            if (c->unwired_prod == 0) c->unwired_prod = prodno;
        }
        r = NULL;
        break;
    }
    case 374: case 375: /* preproc_line : _PRAGMA _PUSH|_POP LP ID RP
                         * (p_preproc_pragma_push / _pop). Python pushes/pops
                         * the OPTIONS stack; the C PRODUCTION treats these as a
                         * no-op line-skip (parser.c:4880-4906 — no OPTIONS
                         * stack mutation), producing a NOP statement (no AST).
                         * Match the production C-vs-C: no side effect, no node. */
        r = NULL;
        break;

    /* ---- const_vector subsystem (array initialisers `=> {…}` / `= {…}`) ----
     * Every Python "list" is represented as an AST_ARRAYINIT node, exactly the
     * structure parse_array_initializer builds, so check_bound_recurse +
     * dim_build_array (shared with the production) operate identically. */
    case 51: { /* const_number_list : expr (p_const_vector_elem_list) — start a
                * flat element list with the per-element const-check. */
        AstNode *lst = ast_new(p->cs, AST_ARRAYINIT, PD_LINENO(1));
        AstNode *e = PD_NODE(1);
        if (e) ast_add_child(p->cs, lst, const_vector_elem(p, e, p->lexer.lineno));
        r = lst;
        break;
    }
    case 52: { /* const_number_list : const_number_list COMMA expr
                * (p_const_vector_elem_list_list) — append the next element.
                * Error lineno is p.lineno(2) = the COMMA. */
        AstNode *lst = (AstNode *)rhs[0].value;
        AstNode *e = PD_NODE(3);
        if (lst && e) ast_add_child(p->cs, lst, const_vector_elem(p, e, PD_LINENO(2)));
        r = lst;
        break;
    }
    case 49: case 50: /* const_vector : LBRACE const_vector_list|const_number_list
                       * RBRACE (p_const_vector) -> p[2] (the inner list). */
        r = (AstNode *)rhs[1].value;
        break;
    case 53: { /* const_vector_list : const_vector (p_const_vector_list) ->
                * [p[1]] — a one-row list whose single child is the row. */
        AstNode *rows = ast_new(p->cs, AST_ARRAYINIT, PD_LINENO(1));
        if (rhs[0].value) ast_add_child(p->cs, rows, (AstNode *)rhs[0].value);
        r = rows;
        break;
    }
    case 54: { /* const_vector_list : const_vector_list COMMA const_vector
                * (p_const_vector_vector_list): every row must have the same
                * element count as the first; on mismatch emit at the COMMA and
                * reduce to None (NULL) — matching parse_array_initializer's
                * ragged path. */
        AstNode *rows = (AstNode *)rhs[0].value;
        AstNode *row = (AstNode *)rhs[2].value;
        if (!rows || !row) { r = NULL; break; }
        int first_count = (rows->child_count > 0) ? rows->children[0]->child_count : -1;
        if (row->child_count != first_count) {
            zxbc_error(p->cs, PD_LINENO(2),
                       "All rows must have the same number of elements");
            r = NULL;  /* p[0] = None */
            break;
        }
        ast_add_child(p->cs, rows, row);
        r = rows;
        break;
    }
    case 43: case 44: { /* var_decl : DIM idlist LP bound_list RP typedef
                         * (RIGHTARROW|EQ) const_vector (p_arr_decl_initialized).
                         * idlist[1] bound_list[3] typedef[5] const_vector[7]. */
        PdIdList *l = (PdIdList *)rhs[1].value;
        AstNode *bounds = (AstNode *)rhs[3].value;
        TypeInfo *type = (TypeInfo *)rhs[5].value;
        AstNode *init = (AstNode *)rhs[7].value;
        int ln = PD_LINENO(1);
        /* p[8] is None (ragged const_vector) -> return without declaring or
         * check_bound (zxbparser.py:840-842). */
        if (!init) { r = make_nop(p); break; }
        if (l->count != 1) {
            zxbc_error(p->cs, ln,
                       "Array declaration only allows one variable name at a time");
            r = make_nop(p);
            break;
        }
        const char *aname = l->ids[0]->name;
        if (!type) {
            size_t nl = strlen(aname);
            if (nl > 0 && is_deprecated_suffix(aname[nl - 1]))
                type = st->basic_types[suffix_to_type(aname[nl - 1])];
            else {
                type = type_new_ref(p->cs, p->cs->default_type, ln, true);
                if (p->cs->opts.strict)
                    zxbc_error(p->cs, ln,
                               "strict mode: missing type declaration for '%s'", aname);
            }
        }
        /* check_bound (zxbparser.py:844-845) on the const-vector image. The
         * error lineno is p.lineno(8) == the const-vector's line; the
         * production parser uses init->lineno (the '{'), so match that. */
        check_bound_recurse(p, bounds, 0, init, init->lineno);
        /* Cannot initialize array of type string (zxbparser.py:854-856). */
        if (type && type_is_string(type))
            zxbc_error(p->cs, ln, "Cannot initialize array of type string");
        r = dim_build_array(p, aname, bounds, type, NULL, init, ln);
        break;
    }

    /* ---- assignment family ----
     * lexpr : ID EQ | LET ID EQ  (p_lexpr, zxbparser.py:1119): returns the
     * Id(name,lineno) and pre-accesses the lvalue (access_id, NO default
     * type) BEFORE the RHS — the LALR reduce order gives exactly Python's
     * timing. */
    case 74: { /* lexpr : ID EQ */
        const char *name = PD_SVAL(1);
        int ln = PD_LINENO(1);
        symboltable_access_id(p->cs->symbol_table, p->cs, name, ln, NULL, CLASS_unknown);
        *out = pd_new_id(p, name, ln);
        *out_lineno = ln;
        return true;
    }
    case 75: { /* lexpr : LET ID EQ */
        const char *name = PD_SVAL(2);
        int ln = PD_LINENO(2);
        symboltable_access_id(p->cs->symbol_table, p->cs, name, ln, NULL, CLASS_unknown);
        *out = pd_new_id(p, name, ln);
        *out_lineno = ln;
        return true;
    }
    /* ---- statement : ID [arg_list|arguments] (p_statement_call, 70/71/72) ----
     * ID arg_list (70) / ID arguments (71) -> make_sub_call. Bare ID (72): if
     * the entry is label/unknown -> a label reference (make_label); else a
     * make_sub_call with an empty arglist. Faithful to parser.c:5167-5367. */
    case 70: /* statement : ID arg_list */
        r = pd_sub_call(p, PD_SVAL(1), PD_LINENO(1), (AstNode *)rhs[1].value);
        break;
    case 71: /* statement : ID arguments -> arglist wrap of `arguments` */
        r = pd_sub_call(p, PD_SVAL(1), PD_LINENO(1), (AstNode *)rhs[1].value);
        break;
    case 72: { /* statement : ID (bare) */
        const char *name = PD_SVAL(1);
        int ln = PD_LINENO(1);
        AstNode *e = symboltable_lookup(p->cs->symbol_table, name);
        if (e && (e->u.id.class_ == CLASS_label || e->u.id.class_ == CLASS_unknown)) {
            /* label reference (make_label). Promote CLASS_unknown via
             * access_label; reject a re-declared label. (parser.c:5189-5252) */
            if (e->u.id.class_ == CLASS_unknown) {
                AstNode *ln_e = symboltable_access_label(p->cs->symbol_table, p->cs, name, ln);
                if (ln_e && ln_e->u.id.class_ == CLASS_label) {
                    ln_e->u.id.declared = true;
                    ln_e->type_ = p->cs->symbol_table->basic_types[TYPE_uinteger];
                    label_capture_scope_owner(p->cs, ln_e);
                }
            } else if (e->u.id.class_ == CLASS_label && e->u.id.declared) {
                zxbc_error(p->cs, ln, "Label '%s' already used at %s:%d",
                           name, p->cs->current_file, e->lineno);
            }
            AstNode *ls = make_sentence_node(p, "LABEL", ln);
            AstNode *lid = ast_new(p->cs, AST_ID, ln);
            lid->u.id.name = arena_strdup(&p->cs->arena, name);
            lid->u.id.class_ = CLASS_label;
            ast_add_child(p->cs, ls, lid);
            hashmap_set(&p->cs->data_labels, name,
                        p->cs->data_ptr_current ? p->cs->data_ptr_current : "");
            r = ls;
        } else {
            r = pd_sub_call(p, name, ln, NULL);
        }
        break;
    }

    case 73: { /* statement : lexpr expr (p_assignment, zxbparser.py:1084).
                * Faithful to parser.c:5121-5160: re-access the lvalue with
                * default_type = RHS.type_ and default_class var, class fixups,
                * typecast RHS to lvalue type, make_sentence("LET", var, expr). */
        PdId *lx = (PdId *)rhs[0].value;
        AstNode *expr = PD_NODE(2);
        int ln = lx->lineno;
        TypeInfo *rhs_type = expr ? expr->type_ : NULL;
        AstNode *var = symboltable_access_id(p->cs->symbol_table, p->cs,
                                             lx->name, ln, rhs_type, CLASS_var);
        if (var) {
            if (var->u.id.class_ == CLASS_const ||
                var->u.id.class_ == CLASS_sub ||
                var->u.id.class_ == CLASS_function) {
                err_cannot_assign(p->cs, ln, lx->name);
            }
            if (var->u.id.class_ == CLASS_unknown)
                var->u.id.class_ = CLASS_var;
        } else {
            var = ast_new(p->cs, AST_ID, ln);
            var->u.id.name = arena_strdup(&p->cs->arena, lx->name);
            var->u.id.class_ = CLASS_unknown;
        }
        expr = make_typecast(p->cs, var->type_, expr, ln);
        r = make_sentence_node(p, "LET", ln);
        ast_add_child(p->cs, r, var);
        if (expr) ast_add_child(p->cs, r, expr);
        break;
    }

    /* ---- array assignment (p_arr_assignment, 78/79) ----
     * statement : ARRAY_ID arg_list EQ expr | LET ARRAY_ID arg_list EQ expr.
     * Match the PRODUCTION parser's LET-statement array path (parser.c:4911-
     * 4991): the ARRAYACCESS lvalue via make_call_node(expr_context=false),
     * then SENTENCE("LETARRAY", arr, RAW-expr) — NO type_ on the sentence, the
     * expr added WITHOUT a typecast (the C production does not typecast here,
     * unlike Python's p_arr_assignment; C-vs-C matches the production). The
     * string-array element-substr-assign special case (bounds+1 == len) is a
     * separate production path -> defer to UNWIRED. */
    case 76: case 77: { /* statement : [LET] ARRAY_ID EQ ARRAY_ID
                         * (p_array_copy). Match the production (parser.c:
                         * 5089-5130): access both arrays, element-type check,
                         * mark accessed, build ARRAYCOPY[larray, rarray]. The
                         * memsize/count Python checks are warnings the C
                         * production skips. */
        int li = (prodno == 77) ? 1 : 0;   /* LHS ARRAY_ID 0-based index */
        int ri = li + 2;                    /* RHS ARRAY_ID after EQ */
        const char *lname = (const char *)rhs[li].sval;
        const char *rname = (const char *)rhs[ri].sval;
        int lln = rhs[li].lineno, rln = rhs[ri].lineno;
        AstNode *larray = symboltable_access_id(p->cs->symbol_table, p->cs,
                                                lname, lln, NULL, CLASS_array);
        AstNode *rarray = symboltable_access_id(p->cs->symbol_table, p->cs,
                                                rname, rln, NULL, CLASS_array);
        /* The production returns make_nop on the None/type-error paths
         * (parser.c:5116/5121) — match it (a dropped statement vs a NOP can
         * shift the parent BLOCK shape on error files). */
        if (!larray || !rarray) { r = make_nop(p); break; }
        if (larray->type_ && rarray->type_ &&
            !type_equal(larray->type_, rarray->type_)) {
            zxbc_error(p->cs, lln, "Arrays must have the same element type");
            r = make_nop(p); break;
        }
        larray->u.id.accessed = true;
        rarray->u.id.accessed = true;
        /* ARRAYCOPY lineno = the LHS ARRAY_ID line (the production's `ln`). */
        r = make_sentence_node(p, "ARRAYCOPY", lln);
        ast_add_child(p->cs, r, larray);
        ast_add_child(p->cs, r, rarray);
        break;
    }
    case 78: case 79: {
        int i = (prodno == 79) ? 2 : 1;
        const char *aname = (const char *)rhs[i - 1].sval;
        int aln = rhs[i - 1].lineno;
        AstNode *arglist = (AstNode *)rhs[i].value;
        AstNode *aexpr = (AstNode *)rhs[i + 2].value; /* expr at RHS pos i+3 (0-based i+2) */
        if (!arglist || !aexpr) { r = NULL; break; }
        /* Strip a deprecated suffix ($/%/&) for the symbol-table key — a
         * string array `a$(...)` is stored under `a`. */
        const char *alook = aname;
        char abuf[256];
        size_t anl = aname ? strlen(aname) : 0;
        if (anl > 0 && anl < sizeof(abuf) && is_deprecated_suffix(aname[anl - 1])) {
            memcpy(abuf, aname, anl - 1); abuf[anl - 1] = '\0'; alook = abuf;
        }
        AstNode *aentry = symboltable_lookup(p->cs->symbol_table, alook);
        /* Production 78/79 is `ARRAY_ID arg_list EQ expr` ONLY — the chained /
         * TO-substr write forms reduce via SEPARATE productions (80-83,
         * 302-313, p_let_arr_substr*), never here, so there is NEVER a postfix
         * `(` group and NEVER a `TO`-subscript on this reduce.
         *
         * (1) STRING-array `bounds+1 == nargs` substr-in-args sub-case
         *     (p_arr_assignment string branch, zxbparser.py:1199-1205): a
         *     string array where the arglist carries exactly one more
         *     subscript than the declared dim count. Python pops the last
         *     subscript as the (lower==upper) substring index and routes to
         *     make_array_substr_assign → the C production builds it via
         *     parse_call_or_array's single-paren-group substr detection
         *     (parser.c:2615-2653, build_array_substr_lvalue) yielding the
         *     Shape-A STRSLICE[ARRAYACCESS, STRSLICE[lo,hi]] lvalue, then
         *     LETARRAY[lv, RAW expr] (let_array_substr13, sys_letarrsubstr0..2).
         * (2) Otherwise (numeric full-element write, OR a string full-element
         *     write `a$(b) = s` where bounds == nargs): make_call_node
         *     (expr_context=false) → ARRAYACCESS → LETARRAY[arr, RAW expr].
         *
         * RAW expr (no typecast) in both cases, matching the production's
         * LET-statement path (parser.c:5008-5010) — a documented byte-clean
         * divergence from Python's typecast; C-vs-C is what astcmp validates. */
        AstNode *arr = NULL;
        if (aentry && aentry->tag == AST_ID &&
            aentry->u.id.class_ == CLASS_array &&
            type_is_string(aentry->type_)) {
            int ndecl = aentry->u.id.arr_boundlist
                            ? aentry->u.id.arr_boundlist->child_count : 0;
            if (ndecl > 0 && ndecl + 1 == arglist->child_count) {
                /* Pop the last subscript as the substring index. */
                AstNode *last = arglist->children[arglist->child_count - 1];
                AstNode *idx = (last && last->tag == AST_ARGUMENT &&
                                last->child_count > 0) ? last->children[0] : last;
                arr = build_array_substr_lvalue(p, aname, aln,
                                                arglist->children,
                                                arglist->child_count - 1,
                                                idx, idx);
                if (!arr) { r = NULL; break; }  /* error already emitted */
            }
        }
        if (!arr)
            arr = make_call_node(p, aname, aln, arglist, false, false, false);
        if (arr && arr->tag != AST_ARRAYACCESS && arr->tag != AST_STRSLICE) {
            /* An unexpected non-array shape goes through other production
             * paths in the C parser — defer. (arr == NULL is the semantic-
             * error path; the production STILL builds LETARRAY[expr] with the
             * NULL lvalue dropped by ast_add_child — match it, do not defer:
             * let_array_wrong_dims / sn_crash.) */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        if (aentry && aentry->u.id.class_ == CLASS_array && aentry->u.id.addr_expr)
            aentry->u.id.accessed = true;
        r = make_sentence_node(p, "LETARRAY", aln);
        ast_add_child(p->cs, r, arr);  /* no-op when NULL (error path) */
        ast_add_child(p->cs, r, aexpr); /* RAW expr, matching the production */
        break;
    }
    case 82: case 83: { /* statement : [LET] ID substr EQ expr (p_str_assign).
                         * The C PRODUCTION builds this as the LET-statement
                         * STRSLICE-lvalue path: STRSLICE[id, lower, upper]
                         * (the WRITE substr keeps the uinteger pre-cast bounds,
                         * NO base-subtract — parser.c:2705-2814 with
                         * expr_context=false) wrapped in LETARRAY[lv, RAW expr]
                         * (parser.c:5055-5057). Python builds LETSUBSTR; the C
                         * production's Shape-A LETARRAY is the byte-clean
                         * baseline astcmp validates (C-vs-C). */
        int idi = (prodno == 83) ? 1 : 0;   /* ID 0-based index */
        const char *name = PD_SVAL(idi + 1);
        int ln = rhs[idi].lineno;
        PdSubstr *ss = (PdSubstr *)rhs[idi + 1].value;
        AstNode *rexpr = (AstNode *)rhs[idi + 3].value;
        if (!ss || !rexpr) { r = NULL; break; }  /* q/s/r None -> p[0]=None */
        /* RHS must be string (p_str_assign:1327 — error, NOT abort). The
         * lineno is p.lineno(EQ) — the EQ token. */
        if (!type_is_string(rexpr->type_)) {
            const TypeInfo *rft = (rexpr->type_ && rexpr->type_->final_type)
                                      ? rexpr->type_->final_type : rexpr->type_;
            const char *rtn = (rft && rft->tag == AST_BASICTYPE)
                                  ? basictype_to_string(rft->basic_type) : "unknown";
            err_expected_string(p->cs, rhs[idi + 2].lineno, rtn);
        }
        /* access_var(default_type=string) — the WRITE lvalue is NOT marked
         * accessed (expr_context=false; only the READ p_expr_id_substr marks). */
        AstNode *id_node = symboltable_access_var(p->cs->symbol_table, p->cs,
                               name, ln, st->basic_types[TYPE_string]);
        if (!id_node) { r = NULL; break; }  /* p[0] = None */
        if (!ss->lower || !ss->upper) {  /* bound typecast failed — see case 291 */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = NULL; break;
        }
        AstNode *lv = ast_new(p->cs, AST_STRSLICE, ln);
        ast_add_child(p->cs, lv, id_node);
        ast_add_child(p->cs, lv, ss->lower);
        ast_add_child(p->cs, lv, ss->upper);
        lv->type_ = st->basic_types[TYPE_string];
        r = make_sentence_node(p, "LETARRAY", PD_LINENO(1));
        ast_add_child(p->cs, r, lv);
        ast_add_child(p->cs, r, rexpr);  /* RAW expr */
        break;
    }
    case 80: { /* statement : ID LP expr RP EQ expr (p_substr_assignment_no_let):
                * single-index scalar substr write. The C PRODUCTION resolves
                * `a$(idx)` via parse_call_or_array's CLASS_var-string branch ->
                * make_strslice_var(expr_context=false) -> STRSLICE[entry,
                * ARGUMENT[uinteger(base-sub idx)]], then LETARRAY[lv, RAW expr]
                * (parser.c:4929-5057). Build an arglist of one ARGUMENT[idx] and
                * route through the shared helper. */
        const char *name = PD_SVAL(1);
        int ln = PD_LINENO(1);
        AstNode *idx = PD_NODE(3);
        AstNode *rexpr = PD_NODE(6);
        if (!idx || !rexpr) { r = NULL; break; }
        AstNode *entry = symboltable_access_call(p->cs->symbol_table, p->cs,
                                                 name, ln, NULL);
        if (!entry) { r = NULL; break; }  /* p[0] = None */
        /* entry.class_ == unknown -> var (p_substr_assignment_no_let:1229). */
        if (entry->u.id.class_ == CLASS_unknown) entry->u.id.class_ = CLASS_var;
        /* RHS must be string (error, NOT abort) at p.lineno(5)=EQ. */
        if (!type_is_string(rexpr->type_)) {
            const TypeInfo *rft = (rexpr->type_ && rexpr->type_->final_type)
                                      ? rexpr->type_->final_type : rexpr->type_;
            const char *rtn = (rft && rft->tag == AST_BASICTYPE)
                                  ? basictype_to_string(rft->basic_type) : "unknown";
            err_expected_string(p->cs, rhs[4].lineno, rtn);
        }
        if (!entry->tag || entry->tag != AST_ID ||
            entry->u.id.class_ != CLASS_var || !type_is_string(entry->type_)) {
            /* non-string-var target — production routes elsewhere; defer. */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        AstNode *al = ast_new(p->cs, AST_ARGLIST, ln);
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, idx->lineno);
        arg->u.argument.byref = p->cs->opts.default_byref;
        ast_add_child(p->cs, arg, idx);
        arg->type_ = idx->type_;
        ast_add_child(p->cs, al, arg);
        AstNode *lv = make_strslice_var(p, entry, al, false, ln);
        if (!lv) { r = NULL; break; }
        r = make_sentence_node(p, "LETARRAY", PD_LINENO(1));
        ast_add_child(p->cs, r, lv);
        ast_add_child(p->cs, r, rexpr);  /* RAW expr */
        break;
    }
    case 81: { /* statement : LET ID arg_list EQ expr (p_substr_assignment):
                * scalar string substr write (1 index) or whole-string (0). The
                * C PRODUCTION builds make_strslice_var -> STRSLICE wrapped in
                * LETARRAY (Shape A), not Python's LETSUBSTR. */
        const char *name = PD_SVAL(2);
        int ln = PD_LINENO(2);
        AstNode *arglist = PD_NODE(3);
        AstNode *rexpr = PD_NODE(5);
        if (!arglist || !rexpr) { r = NULL; break; }  /* p[3]/p[5] None */
        AstNode *entry = symboltable_access_call(p->cs->symbol_table, p->cs,
                                                 name, ln, NULL);
        if (!entry) { r = NULL; break; }
        /* The not-a-var / non-string-target cases are error paths where the
         * OLD production builds a FUNCCALL-based LETARRAY recovery shape
         * (e.g. explicit-mode undeclared `LET a(5) = "5"`, explicit5) while
         * Python p_substr_assignment errors + None. The engine matches PYTHON;
         * defer those to Phase-E-reconcile (do not contort to the old
         * production's error-recovery tree). Only the genuine string-var substr
         * write is wired. */
        if (entry->u.id.class_ != CLASS_var &&
            entry->u.id.class_ != CLASS_unknown) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        if (entry->u.id.class_ == CLASS_unknown) entry->u.id.class_ = CLASS_var;
        if (!type_is_string(entry->type_)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        if (!type_is_string(rexpr->type_)) {
            const TypeInfo *rft = (rexpr->type_ && rexpr->type_->final_type)
                                      ? rexpr->type_->final_type : rexpr->type_;
            const char *rtn = (rft && rft->tag == AST_BASICTYPE)
                                  ? basictype_to_string(rft->basic_type) : "unknown";
            err_expected_string(p->cs, PD_LINENO(4), rtn);
            r = NULL; break;
        }
        if (arglist->child_count > 1) {
            zxbc_error(p->cs, ln, "Accessing string with too many indexes. Expected only one.");
            r = NULL; break;
        }
        /* 0-index whole-string `LET a$() = x`: Python builds LETSUBSTR with the
         * MIN..MAX bounds; the C production's make_strslice_var with an empty
         * arglist yields STRSLICE[entry] (no bounds) — a different shape, so
         * defer the 0-index form. The 1-index form is the common substr write. */
        if (arglist->child_count == 0) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        AstNode *lv = make_strslice_var(p, entry, arglist, false, ln);
        if (!lv) { r = NULL; break; }
        r = make_sentence_node(p, "LETARRAY", PD_LINENO(1));
        ast_add_child(p->cs, r, lv);
        ast_add_child(p->cs, r, rexpr);
        break;
    }

    /* ---- array-substr writes (p_let_arr_substr*, 302-313) ----
     * `[LET] ARRAY_ID arg_list substr EQ expr` and the `LP arguments TO ... RP`
     * variants -> make_array_substr_assign(lineno, id, arg_list, substr, expr)
     * -> the C build_array_substr_lvalue (Shape-A STRSLICE[ARRAYACCESS,
     * STRSLICE[lo,hi]]) wrapped in LETARRAY[lv, RAW expr] (the production's LET
     * path, parser.c:2727-2766 + 5055-5057). `i` is the 1-based ARRAY_ID
     * position; LET forms (even prodno) shift +1. Faithful to zxbparser.py:
     * 2733-2815. */
    case 302: case 303: {
        /* [LET] ARRAY_ID arg_list substr EQ expr (p_let_arr_substr): the TO
         * substr form `a$(dims)(lo TO hi) = rhs`. The C PRODUCTION builds
         * build_array_substr_lvalue (Shape-A STRSLICE[ARRAYACCESS, STRSLICE
         * [lo,hi]]) with the substr bounds at their NATURAL type (NOT the
         * uinteger pre-cast read path uses) — so use PdSubstr.raw_lower/upper.
         * Wrapped in LETARRAY[lv, RAW expr] (parser.c:2727-2766 + 5055-5057). */
        int is_let = (prodno == 302);
        int i = is_let ? 2 : 1;
        const char *aname = PD_SVAL(i);
        int aln = PD_LINENO(i);
        AstNode *arglist = (AstNode *)rhs[i].value;
        PdSubstr *ss = (PdSubstr *)rhs[i + 1].value;
        AstNode *rexpr = (AstNode *)rhs[i + 3].value;
        if (!arglist || !ss || !rexpr) { r = NULL; break; }
        /* A NON-string array `a(3)(1 TO 5) = ...` (let_array_substr4): Python's
         * make_array_substr_assign errors + None, but the OLD production builds
         * a chained STRSLICE-over-ARRAYACCESS via parse_postfix (a recursive-
         * descent divergence) — the engine matches PYTHON, so defer that case
         * to Phase-E-reconcile rather than emit the error + drop (which would
         * DIFF vs the production's kept tree). */
        {
            AstNode *probe = symboltable_get_entry(p->cs->symbol_table, aname);
            if (!probe || probe->tag != AST_ID || !type_is_string(probe->type_)) {
                c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
                r = make_nop(p); break;
            }
        }
        AstNode *lv = build_array_substr_lvalue(p, aname, aln,
                                                arglist->children,
                                                arglist->child_count,
                                                ss->raw_lower, ss->raw_upper);
        if (!lv) { r = NULL; break; }  /* error already emitted -> p[0]=None */
        r = make_sentence_node(p, "LETARRAY", PD_LINENO(1));
        ast_add_child(p->cs, r, lv);
        ast_add_child(p->cs, r, rexpr);  /* RAW expr */
        break;
    }
    /* [LET] ARRAY_ID LP arguments [COMMA] TO [expr] RP EQ expr (306-313,
     * p_let_arr_substr_in_args{,2,3,4}): the array-element substring write
     * where the substr bounds are inside the same paren group. The C PRODUCTION
     * builds build_array_substr_lvalue (Shape-A STRSLICE[ARRAYACCESS, STRSLICE
     * [lo,hi]]) with the bounds at their NATURAL type (str_base2..5,
     * let_array_substr9..12) -> LETARRAY[lv, RAW expr]. `arguments` is the dim
     * list at rhs[i+1]; forms that take the trailing arg as the lower bound pop
     * it. Faithful to zxbparser.py:2759-2815. */
    case 306: case 307: case 308: case 309:
    case 310: case 311: case 312: case 313: {
        bool is_let = (prodno % 2 == 0);  /* even = LET form */
        int i = is_let ? 2 : 1;           /* 1-based ARRAY_ID position */
        const char *aname = PD_SVAL(i);
        int aln = PD_LINENO(i);
        AstNode *arglist = (AstNode *)rhs[i + 1].value;  /* `arguments` after LP */
        if (!arglist) { r = NULL; break; }
        AstNode *lower = NULL, *upper = NULL, *rexpr = NULL;
        AstNode *maxn = make_number(p, 65534, aln, NULL);   /* MAX_STRSLICE_IDX (uinteger nat.) */
        /* The implicit lower `0` of a `( ... , TO ...)` form is uinteger-typed
         * by the production's parse-loop TO-handling (make_typecast(uinteger,
         * NUMBER(0)), parser.c:2525) — NOT the bare ubyte make_number(0). */
        AstNode *zero = make_number(p, 0, aln, st->basic_types[TYPE_uinteger]);
        /* Positions are 1-based from the ARRAY_ID at i; PD_NODE(n)==rhs[n-1].
         * arguments is at i+2. */
        if (prodno == 306 || prodno == 307) {        /* ...LP arguments TO RP EQ expr */
            if (arglist->child_count > 0) {          /* pop last as lower */
                AstNode *last = arglist->children[arglist->child_count - 1];
                lower = (last && last->tag == AST_ARGUMENT && last->child_count > 0)
                            ? last->children[0] : last;
                arglist->child_count--;
            }
            upper = maxn;
            rexpr = PD_NODE(i + 6);
        } else if (prodno == 308 || prodno == 309) { /* ...LP arguments COMMA TO expr RP EQ expr */
            /* Implicit-lower TO form: the parse-loop TO-handling uinteger-casts
             * BOTH bounds (parser.c:2525-2531), so the explicit upper expr is
             * cast too (str_base3, let_array_substr10). */
            lower = zero;
            upper = make_typecast(p->cs, st->basic_types[TYPE_uinteger],
                                  PD_NODE(i + 5), aln);
            rexpr = PD_NODE(i + 8);
        } else if (prodno == 310 || prodno == 311) { /* ...LP arguments COMMA TO RP EQ expr */
            lower = zero;
            upper = maxn;
            rexpr = PD_NODE(i + 7);
        } else {                                     /* 312/313 ...LP arguments TO expr RP EQ expr */
            if (arglist->child_count > 0) {          /* pop last as lower */
                AstNode *last = arglist->children[arglist->child_count - 1];
                lower = (last && last->tag == AST_ARGUMENT && last->child_count > 0)
                            ? last->children[0] : last;
                arglist->child_count--;
            }
            upper = PD_NODE(i + 4);                  /* expr after TO */
            rexpr = PD_NODE(i + 7);
        }
        if (!lower || !upper || !rexpr) { r = NULL; break; }
        /* non-string array -> Python errors+None, old production builds a
         * chained shape; defer (Phase-E-reconcile), like 302/303. */
        AstNode *probe = symboltable_get_entry(p->cs->symbol_table, aname);
        if (!probe || probe->tag != AST_ID || !type_is_string(probe->type_)) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = make_nop(p); break;
        }
        AstNode *lv = build_array_substr_lvalue(p, aname, aln,
                                                arglist->children,
                                                arglist->child_count, lower, upper);
        if (!lv) { r = NULL; break; }
        r = make_sentence_node(p, "LETARRAY", PD_LINENO(1));
        ast_add_child(p->cs, r, lv);
        ast_add_child(p->cs, r, rexpr);  /* RAW expr */
        break;
    }
    /* 304/305 (chained `a$(b)(idx)`): the OLD production builds a chained
     * ARRAYACCESS-over-ARRAYACCESS while Python builds make_array_substr_assign
     * (a substr) — the engine must match PYTHON at the swap, which DIFFs the
     * old production now, so this is Phase-E-reconcile, left UNWIRED (default
     * path). */

    /* ---- SAVE / LOAD / VERIFY (p_save_code/p_save_data/p_load_or_verify/
     * p_load_code/p_load_data, 232-246) ----
     * SENTENCE(kind, str_expr, start, length) — start/length computed per form.
     * The production typecasts at the keyword line `ln` and CODE/COMMA lines;
     * the str-type error fires at `ln` (parser.c:4576-4774). Faithful C-vs-C. */
    case 238: case 239: /* load_or_verify : LOAD | VERIFY -> the kind string */
        *out = pd_new_id(p, prodno == 238 ? "LOAD" : "VERIFY", PD_LINENO(1));
        *out_lineno = PD_LINENO(1);
        return true;
    case 232: case 233: case 234: { /* SAVE expr CODE expr COMMA expr |
                                     * SAVE expr ID | SAVE expr ARRAY_ID
                                     * (p_save_code). */
        int ln = PD_LINENO(1);
        AstNode *sx = PD_NODE(2);
        if (sx && sx->type_ && !type_equal(sx->type_, st->basic_types[TYPE_string]))
            err_expected_string(p->cs, ln, sx->type_->name ? sx->type_->name : "");
        AstNode *start = NULL, *length = NULL;
        if (prodno == 232) {        /* CODE expr COMMA expr */
            start = make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(4), PD_LINENO(3));
            length = make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(6), PD_LINENO(5));
        } else {                    /* SAVE expr ID|ARRAY_ID — SCREEN$/SCREEN/CODE */
            const char *idt = PD_SVAL(3);
            int id_ln = PD_LINENO(3);
            bool is_code = idt && (strcasecmp(idt, "CODE") == 0);
            bool is_screen = idt && (strcasecmp(idt, "SCREEN") == 0 ||
                                     strcasecmp(idt, "SCREEN$") == 0);
            if (!is_code && !is_screen) {
                zxbc_error(p->cs, id_ln,
                           "Unexpected \"%s\" ID. Expected \"SCREEN$\" instead", idt ? idt : "");
                r = NULL; break;
            }
            if (is_code) { start = make_number(p, 0, id_ln, NULL); length = make_number(p, 0, id_ln, NULL); }
            else { start = make_number(p, 16384, id_ln, NULL); length = make_number(p, 6912, id_ln, NULL); }
        }
        r = make_loadsave_node(p, "SAVE", ln, sx, start, length);
        break;
    }
    case 235: case 236: case 237: { /* SAVE expr DATA [ID [LP RP]] (p_save_data) */
        int ln = PD_LINENO(1);
        AstNode *sx = PD_NODE(2);
        if (sx && sx->type_ && !type_equal(sx->type_, st->basic_types[TYPE_string]))
            err_expected_string(p->cs, ln, sx->type_->name ? sx->type_->name : "");
        const char *idn = (prodno == 235) ? NULL : PD_SVAL(4);
        int id_ln = (prodno == 235) ? PD_LINENO(3) : PD_LINENO(4);
        AstNode *start = NULL, *length = NULL;
        if (!loadsave_data_startlen(p, idn, id_ln, PD_LINENO(3), &start, &length)) {
            r = make_loadsave_node(p, "SAVE", ln, NULL, NULL, NULL); break; /* p[0]=None drop */
        }
        r = make_loadsave_node(p, "SAVE", ln, sx, start, length);
        break;
    }
    case 240: case 241: case 242: case 243: { /* load_or_verify expr ID|CODE
                                               * [expr [COMMA expr]] (p_load_code) */
        PdId *lv = (PdId *)rhs[0].value;
        const char *kind = lv ? lv->name : "LOAD";
        int ln = lv ? lv->lineno : PD_LINENO(1);
        AstNode *sx = PD_NODE(2);
        if (sx && sx->type_ && !type_equal(sx->type_, st->basic_types[TYPE_string]))
            err_expected_string(p->cs, ln, sx->type_->name ? sx->type_->name : "");
        AstNode *start = NULL, *length = NULL;
        if (prodno == 240) {        /* expr ID — SCREEN$/SCREEN/CODE */
            const char *idt = PD_SVAL(3);
            int id_ln = PD_LINENO(3);
            bool is_code = idt && (strcasecmp(idt, "CODE") == 0);
            bool is_screen = idt && (strcasecmp(idt, "SCREEN") == 0 ||
                                     strcasecmp(idt, "SCREEN$") == 0);
            if (!is_code && !is_screen) {
                zxbc_error(p->cs, id_ln,
                           "Unexpected \"%s\" ID. Expected \"SCREEN$\" instead", idt ? idt : "");
                r = NULL; break;
            }
            if (is_code) { start = make_number(p, 0, id_ln, NULL); length = make_number(p, 0, id_ln, NULL); }
            else { start = make_number(p, 16384, id_ln, NULL); length = make_number(p, 6912, id_ln, NULL); }
        } else if (prodno == 241) { /* CODE (bare): start=0, length=0 */
            int c_ln = PD_LINENO(3);
            start = make_number(p, 0, c_ln, NULL); length = make_number(p, 0, c_ln, NULL);
        } else if (prodno == 242) { /* CODE expr: start=expr, length=0 */
            int c_ln = PD_LINENO(3);
            start = make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(4), c_ln);
            length = make_number(p, 0, c_ln, NULL);
        } else {                    /* CODE expr COMMA expr */
            start = make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(4), PD_LINENO(3));
            length = make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(6), PD_LINENO(5));
        }
        r = make_loadsave_node(p, kind, ln, sx, start, length);
        break;
    }
    case 244: case 245: case 246: { /* load_or_verify expr DATA [ID [LP RP]]
                                     * (p_load_data) */
        PdId *lv = (PdId *)rhs[0].value;
        const char *kind = lv ? lv->name : "LOAD";
        int ln = lv ? lv->lineno : PD_LINENO(1);
        AstNode *sx = PD_NODE(2);
        if (sx && sx->type_ && !type_equal(sx->type_, st->basic_types[TYPE_string]))
            err_expected_string(p->cs, ln, sx->type_->name ? sx->type_->name : "");
        const char *idn = (prodno == 244) ? NULL : PD_SVAL(4);
        int id_ln = (prodno == 244) ? PD_LINENO(3) : PD_LINENO(4);
        AstNode *start = NULL, *length = NULL;
        if (!loadsave_data_startlen(p, idn, id_ln, PD_LINENO(3), &start, &length)) {
            r = make_loadsave_node(p, kind, ln, NULL, NULL, NULL); break;
        }
        r = make_loadsave_node(p, kind, ln, sx, start, length);
        break;
    }

    /* ---- RESTORE (p_restore, 158/159/160) ----
     * statement : RESTORE | RESTORE ID | RESTORE NUMBER. SENTENCE("RESTORE")
     * + optional AST_ID label child (name/class_=label), matching the
     * production parser (parser.c:4361-4377). */
    case 158: /* RESTORE (bare) */
        r = make_sentence_node(p, "RESTORE", PD_LINENO(1));
        break;
    case 159: case 160: { /* RESTORE ID | RESTORE NUMBER */
        r = make_sentence_node(p, "RESTORE", PD_LINENO(1));
        const char *label = PD_SVAL(2);
        char buf[32];
        if (!label) { snprintf(buf, sizeof(buf), "%d", (int)PD_NUM(2)); label = buf; }
        AstNode *lbl = ast_new(p->cs, AST_ID, PD_LINENO(2));
        lbl->u.id.name = arena_strdup(&p->cs->arena, label);
        lbl->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, r, lbl);
        break;
    }

    /* ---- leaf expressions ---- */
    /* ---- single-arg parenless builtins (KW bexpr) via make_builtin_node ---- */
    case 376: r = pd_builtin1(p, "USR",  BTOK_USR,  PD_NODE(2), PD_LINENO(1)); break;
    case 377: case 378: /* bexpr : RND | RND LP RP (p_expr_rnd) -> float */
        r = pd_builtin0(p, "RND", TYPE_float, PD_LINENO(1)); break;
    case 390: /* string : STR expr (p_str) -> make_builtin STR (float-cast arg,
               * constant-fold to STRING) handled by make_builtin_node. */
        r = pd_builtin1(p, "STR", BTOK_STR, PD_NODE(2), PD_LINENO(1)); break;
    case 391: /* string : INKEY (p_inkey) -> string */
        r = pd_builtin0(p, "INKEY", TYPE_string, PD_LINENO(1)); break;
    case 392: /* string : CHR bexpr (p_chr_one) -> CHR builtin, single ubyte arg */
        r = pd_builtin1(p, "CHR", BTOK_CHR, PD_NODE(2), PD_LINENO(1)); break;
    case 393: { /* string : CHR arg_list (p_chr): >=1 arg; each arg's VALUE
                 * (unwrapped from ARGUMENT) becomes a direct BUILTIN child,
                 * make_builtin_node casts each to ubyte (parser.c:2110-2126,
                 * matching the production's multi-arg CHR builder). */
        AstNode *al = (AstNode *)rhs[1].value;
        if (!al || al->child_count < 1) {
            zxbc_error(p->cs, PD_LINENO(1), "CHR$ function need at less 1 parameter");
            r = NULL; break;
        }
        AstNode *n = ast_new(p->cs, AST_BUILTIN, PD_LINENO(1));
        n->u.builtin.fname = arena_strdup(&p->cs->arena, "CHR");
        AstNode *first = NULL;
        for (int i = 0; i < al->child_count; i++) {
            AstNode *a = al->children[i];
            AstNode *v = (a && a->tag == AST_ARGUMENT && a->child_count > 0)
                             ? a->children[0] : a;
            if (i == 0) first = v;
            ast_add_child(p->cs, n, v);
        }
        r = make_builtin_node(p, n, first, BTOK_CHR, PD_LINENO(1));
        break;
    }
    case 379: r = pd_builtin1(p, "PEEK", BTOK_PEEK, PD_NODE(2), PD_LINENO(1)); break;
    case 381: r = pd_builtin1(p, "IN",   BTOK_IN,   PD_NODE(2), PD_LINENO(1)); break;
    case 386: r = pd_builtin1(p, "LEN",  BTOK_LEN,  PD_NODE(2), PD_LINENO(1)); break;
    case 394: r = pd_builtin1(p, "VAL",  BTOK_VAL,  PD_NODE(2), PD_LINENO(1)); break;
    case 395: r = pd_builtin1(p, "CODE", BTOK_CODE, PD_NODE(2), PD_LINENO(1)); break;
    case 396: r = pd_builtin1(p, "SGN",  BTOK_SGN,  PD_NODE(2), PD_LINENO(1)); break;
    case 407: r = pd_builtin1(p, "INT",  BTOK_INT,  PD_NODE(2), PD_LINENO(1)); break;
    case 408: r = pd_builtin1(p, "ABS",  BTOK_ABS,  PD_NODE(2), PD_LINENO(1)); break;

    /* ---- LBOUND/UBOUND (382-385, p_expr_lbound / p_expr_lbound_expr) ---- */
    case 382: case 383: { /* LBOUND|UBOUND LP ARRAY_ID RP (no dim) */
        const char *fn = (prodno == 382) ? "LBOUND" : "UBOUND";
        const char *aname = PD_SVAL(3);
        int aln = PD_LINENO(3);
        TypeInfo *uint_t = st->basic_types[TYPE_uinteger];
        AstNode *entry = symboltable_access_array(p->cs->symbol_table, p->cs, aname, aln, NULL);
        if (!entry) { r = NULL; break; }
        mark_label_accessed(entry); /* mark_entry_as_accessed */
        AstNode *bl = entry->u.id.arr_boundlist;
        int nbounds = bl ? bl->child_count : 0;
        if (entry->u.id.scope == SCOPE_parameter) {
            AstNode *bn = ast_new(p->cs, AST_BUILTIN, PD_LINENO(1));
            bn->u.builtin.fname = arena_strdup(&p->cs->arena, fn);
            ast_add_child(p->cs, bn, entry);
            ast_add_child(p->cs, bn, make_number(p, 0, aln, uint_t));
            bn->type_ = uint_t;
            r = bn;
        } else {
            r = make_number(p, nbounds, aln, uint_t);
        }
        break;
    }
    case 384: case 385: { /* LBOUND|UBOUND LP ARRAY_ID COMMA expr RP (with dim) */
        const char *fn = (prodno == 384) ? "LBOUND" : "UBOUND";
        const char *aname = PD_SVAL(3);
        int aln = PD_LINENO(3);
        AstNode *expr = PD_NODE(5);
        if (!expr) { r = NULL; break; }
        TypeInfo *uint_t = st->basic_types[TYPE_uinteger];
        AstNode *entry = symboltable_access_array(p->cs->symbol_table, p->cs, aname, aln, NULL);
        if (!entry) { r = NULL; break; }
        mark_label_accessed(entry);
        AstNode *num = make_typecast(p->cs, uint_t, expr, PD_LINENO(6));
        if (!num) { r = NULL; break; }
        AstNode *bl = entry->u.id.arr_boundlist;
        int nbounds = bl ? bl->child_count : 0;
        bool is_lb = (prodno == 384);
        if (check_is_number(num) &&
            (entry->u.id.scope == SCOPE_local || entry->u.id.scope == SCOPE_global)) {
            long val = (long)num->u.number.value;
            if (val < 0 || val > nbounds) {
                zxbc_error(p->cs, PD_LINENO(6), "Dimension out of range");
                r = NULL; break;
            }
            if (val == 0) r = make_number(p, nbounds, aln, uint_t);
            else {
                AstNode *bd = bl->children[val - 1];
                long lo = (bd && bd->child_count > 0 && bd->children[0]->tag == AST_NUMBER)
                              ? (long)bd->children[0]->u.number.value : 0;
                long hi = (bd && bd->child_count > 1 && bd->children[1]->tag == AST_NUMBER)
                              ? (long)bd->children[1]->u.number.value : 0;
                r = make_number(p, is_lb ? lo : hi, aln, uint_t);
            }
        } else {
            if (is_lb) entry->u.id.lbound_used = true;
            else entry->u.id.ubound_used = true;
            AstNode *bn = ast_new(p->cs, AST_BUILTIN, PD_LINENO(1));
            bn->u.builtin.fname = arena_strdup(&p->cs->arena, fn);
            ast_add_child(p->cs, bn, entry);
            ast_add_child(p->cs, bn, num);
            bn->type_ = uint_t;
            r = bn;
        }
        break;
    }
    case 380: { /* PEEK LP numbertype COMMA expr RP (p_expr_peektype_):
                 * make_builtin("PEEK", typecast(uinteger, expr), type_=numbertype) */
        TypeInfo *ptype = (TypeInfo *)rhs[2].value;  /* numbertype */
        AstNode *expr = PD_NODE(5);
        if (!expr) { r = NULL; break; }
        AstNode *cast = make_typecast(p->cs, st->basic_types[TYPE_uinteger], expr, PD_LINENO(4));
        AstNode *bn = ast_new(p->cs, AST_BUILTIN, PD_LINENO(1));
        bn->u.builtin.fname = arena_strdup(&p->cs->arena, "PEEK");
        if (cast) ast_add_child(p->cs, bn, cast);
        bn->type_ = ptype;
        r = bn;
        break;
    }

    case 316: { /* bexpr : ID bexpr (p_bexpr_func) — parenless single-arg call.
                 * make_call(name, [argument(bexpr)]). Build a 1-elem ARGLIST
                 * and route through make_call_node (which resolves the entry
                 * and builds ARRAYLOAD/STRSLICE/FUNCCALL). */
        const char *name = PD_SVAL(1);
        int ln = PD_LINENO(1);
        AstNode *argexpr = PD_NODE(2);
        if (!argexpr) { r = NULL; break; }
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, argexpr->lineno);
        arg->u.argument.byref = p->cs->opts.default_byref;
        ast_add_child(p->cs, arg, argexpr);
        arg->type_ = argexpr->type_;
        AstNode *al = ast_new(p->cs, AST_ARGLIST, ln);
        ast_add_child(p->cs, al, arg);
        r = make_call_node(p, name, ln, al, true, false, false);
        break;
    }
    case 397: { /* bexpr : math_fn bexpr (p_expr_trig) — math_fn (398-406)
                 * carries the fn name; kw mapped from it. */
        PdId *mf = (PdId *)rhs[0].value;       /* {name, kw} */
        r = pd_builtin1(p, mf->name, (BTokenType)mf->lineno, PD_NODE(2), PD_LINENO(1));
        break;
    }
    case 398: case 399: case 400: case 401: case 402:
    case 403: case 404: case 405: case 406: { /* math_fn : SIN|COS|...|SQR */
        const char *nm; BTokenType k;
        switch (prodno) {
            case 398: nm="SIN"; k=BTOK_SIN; break;
            case 399: nm="COS"; k=BTOK_COS; break;
            case 400: nm="TAN"; k=BTOK_TAN; break;
            case 401: nm="ASN"; k=BTOK_ASN; break;
            case 402: nm="ACS"; k=BTOK_ACS; break;
            case 403: nm="ATN"; k=BTOK_ATN; break;
            case 404: nm="LN";  k=BTOK_LN;  break;
            case 405: nm="EXP"; k=BTOK_EXP; break;
            default:  nm="SQR"; k=BTOK_SQR; break;
        }
        /* carry name + kw (kw stashed in the PdId.lineno field). */
        PdId *mf = pd_new_id(p, nm, (int)k);
        *out = mf; *out_lineno = PD_LINENO(1);
        return true;
    }

    case 279: { /* expr : CAST LP numbertype COMMA expr RP (p_cast) ->
                 * make_typecast(numbertype, expr). The production passes the
                 * CAST keyword line (parser.c:2307), not Python's p.lineno(6);
                 * match C-vs-C. */
        TypeInfo *target = (TypeInfo *)rhs[2].value;
        AstNode *expr = (AstNode *)rhs[4].value;
        if (!expr) { r = NULL; break; }
        r = make_typecast(p->cs, target, expr, PD_LINENO(1));
        break;
    }
    case 280: /* bexpr : NUMBER */
        r = make_number(p, PD_NUM(1), PD_LINENO(1), NULL);
        break;
    case 281: /* bexpr : PI */
        r = make_number(p, M_PI, PD_LINENO(1), st->basic_types[TYPE_float]);
        break;
    case 282: /* bexpr : string */
        r = PD_NODE(1);
        break;
    case 285: /* string : STRC */
        r = make_string(p, PD_SVAL(1) ? PD_SVAL(1) : "", PD_LINENO(1));
        break;

    /* ---- substr non-terminal (p_subind_*, zxbparser.py:2600-2638) ----
     * Carry (lower, upper), each pre-typecast to uinteger. The string-slice
     * consumers (288/289/290) pass these to make_strslice_node. */
    case 291: { /* substr : LP expr TO expr RP */
        PdSubstr *ss = arena_alloc(&p->cs->arena, sizeof(PdSubstr));
        TypeInfo *ui = st->basic_types[TYPE_uinteger];
        /* Capture the RAW bounds BEFORE the uinteger cast — make_typecast
         * mutates a constant NUMBER in place (retypes it to uinteger), so the
         * raw snapshot must be a distinct node for the array-substr WRITE path
         * (which keeps the bound at its natural type). pd_clone_bound copies a
         * NUMBER; a non-constant bound make_typecast WRAPS (not mutates), so
         * the original node stays raw and is shared safely. */
        ss->raw_lower = pd_clone_bound(p, PD_NODE(2));
        ss->raw_upper = pd_clone_bound(p, PD_NODE(4));
        ss->lower = make_typecast(p->cs, ui, PD_NODE(2), PD_LINENO(1));
        ss->upper = make_typecast(p->cs, ui, PD_NODE(4), PD_LINENO(3));
        /* A bound that can't convert to uinteger (e.g. a string-typed bound
         * via deprecated-suffix sharing, `a$(a TO b)`, fixtures 50/51) nulls
         * the tuple here — Python-faithful (make_typecast -> None propagates
         * to make_strslice -> None). The OLD production emits the same error
         * but KEEPS the raw bound child (a recursive-descent error-recovery
         * divergence), so the trees diverge on these error files -> flag
         * UNWIRED (Phase-E-reconcile: the engine matches PYTHON at the swap). */
        if (!ss->lower || !ss->upper) {
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        }
        *out = ss; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 292: { /* substr : LP TO expr RP -> lower=NUMBER(0), upper=expr */
        PdSubstr *ss = arena_alloc(&p->cs->arena, sizeof(PdSubstr));
        TypeInfo *ui = st->basic_types[TYPE_uinteger];
        ss->raw_lower = make_number(p, 0, PD_LINENO(2), NULL);
        ss->raw_upper = pd_clone_bound(p, PD_NODE(3));
        ss->lower = make_typecast(p->cs, ui, make_number(p, 0, PD_LINENO(2), NULL), PD_LINENO(1));
        ss->upper = make_typecast(p->cs, ui, PD_NODE(3), PD_LINENO(2));
        if (!ss->lower || !ss->upper) { /* see case 291 — Phase-E-reconcile */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        }
        *out = ss; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 293: { /* substr : LP expr TO RP -> lower=expr, upper=NUMBER(65534) */
        PdSubstr *ss = arena_alloc(&p->cs->arena, sizeof(PdSubstr));
        TypeInfo *ui = st->basic_types[TYPE_uinteger];
        ss->raw_lower = pd_clone_bound(p, PD_NODE(2));
        ss->raw_upper = make_number(p, 65534, PD_LINENO(4), NULL);
        ss->lower = make_typecast(p->cs, ui, PD_NODE(2), PD_LINENO(1));
        ss->upper = make_typecast(p->cs, ui, make_number(p, 65534, PD_LINENO(4), NULL), PD_LINENO(4));
        if (!ss->lower || !ss->upper) { /* see case 291 — Phase-E-reconcile */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        }
        *out = ss; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 294: { /* substr : LP TO RP -> lower=NUMBER(0), upper=NUMBER(65534) */
        PdSubstr *ss = arena_alloc(&p->cs->arena, sizeof(PdSubstr));
        TypeInfo *ui = st->basic_types[TYPE_uinteger];
        ss->raw_lower = make_number(p, 0, PD_LINENO(2), NULL);
        ss->raw_upper = make_number(p, 65534, PD_LINENO(3), NULL);
        ss->lower = make_typecast(p->cs, ui, make_number(p, 0, PD_LINENO(2), NULL), PD_LINENO(1));
        ss->upper = make_typecast(p->cs, ui, make_number(p, 65534, PD_LINENO(3), NULL), PD_LINENO(2));
        *out = ss; *out_lineno = PD_LINENO(1);
        return true;
    }

    /* ---- string-slice reads (make_strslice_node) ---- */
    case 288: { /* string : ID substr (p_expr_id_substr): CONST-string fast
                 * path (get_entry token==CONST) does NOT mark accessed; else
                 * access_var(default_type=string) + mark accessed. */
        const char *name = PD_SVAL(1);
        int ln = PD_LINENO(1);
        PdSubstr *ss = (PdSubstr *)rhs[1].value;
        if (!ss) { r = NULL; break; }
        AstNode *entry = symboltable_get_entry(p->cs->symbol_table, name);
        if (entry && entry->tag == AST_ID && type_is_string(entry->type_) &&
            entry->u.id.class_ == CLASS_const) {
            r = make_strslice_node(p, entry, ss->lower, ss->upper, ln);
            break;
        }
        entry = symboltable_access_var(p->cs->symbol_table, p->cs, name, ln,
                                       st->basic_types[TYPE_string]);
        if (!entry) { r = NULL; break; }  /* p[0] = None */
        mark_label_accessed(entry);       /* mark_entry_as_accessed */
        r = make_strslice_node(p, entry, ss->lower, ss->upper, ln);
        break;
    }
    case 289: { /* string : string substr (p_string_substr) */
        AstNode *base = PD_NODE(1);
        PdSubstr *ss = (PdSubstr *)rhs[1].value;
        if (!base || !ss) { r = NULL; break; }
        r = make_strslice_node(p, base, ss->lower, ss->upper, PD_LINENO(1));
        break;
    }
    case 283: { /* string : func_call substr (p_string_func_call) ->
                 * make_strslice(func_call, substr). */
        AstNode *base = PD_NODE(1);
        PdSubstr *ss = (PdSubstr *)rhs[1].value;
        if (!base || !ss) { r = NULL; break; }
        r = make_strslice_node(p, base, ss->lower, ss->upper, PD_LINENO(1));
        break;
    }
    case 284: { /* string : func_call LP expr RP (p_string_func_call_single) ->
                 * make_strslice(func_call, expr, expr) — single index. */
        AstNode *base = PD_NODE(1);
        AstNode *idx = PD_NODE(3);
        if (!base || !idx) { r = NULL; break; }
        r = make_strslice_node(p, base, idx, idx, PD_LINENO(1));
        break;
    }
    case 286: /* string : string LP RP (p_string_lprp) -> pass-through p[1] */
        r = PD_NODE(1);
        break;
    case 287: { /* string : string LP expr RP (p_string_lp_expr_rp) ->
                 * make_strslice(string, expr, expr) at p.lineno(2). */
        AstNode *base = PD_NODE(1);
        AstNode *idx = PD_NODE(3);
        if (!base || !idx) { r = NULL; break; }
        r = make_strslice_node(p, base, idx, idx, PD_LINENO(2));
        break;
    }
    case 290: { /* string : LP expr RP substr (p_string_expr_lp): the LP-expr
                 * base must be string, else "Expected a string type
                 * expression. Got %s type instead" at the lexer line, p[0]=None.
                 * (zxbparser.py:2588-2597, uses p.lexer.lineno.) */
        AstNode *base = PD_NODE(2);
        PdSubstr *ss = (PdSubstr *)rhs[3].value;
        if (!base || !ss) { r = NULL; break; }
        if (!type_is_string(base->type_)) {
            /* Python errors + None (zxbparser.py:2590-2595). The OLD
             * production instead builds a malformed NESTED STRSLICE over the
             * non-string base (a recursive-descent divergence), so the trees
             * diverge on this error file -> flag UNWIRED (Phase-E-reconcile:
             * the engine matches PYTHON). substr_expr_err. */
            const TypeInfo *bft = (base->type_ && base->type_->final_type)
                                      ? base->type_->final_type : base->type_;
            const char *btn = (bft && bft->tag == AST_BASICTYPE)
                                  ? basictype_to_string(bft->basic_type) : "unknown";
            zxbc_error(p->cs, p->lexer.lineno,
                       "Expected a string type expression. Got %s type instead", btn);
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = NULL; break;
        }
        r = make_strslice_node(p, base, ss->lower, ss->upper, p->lexer.lineno);
        break;
    }
    case 297: /* expr : bexpr */
        r = PD_NODE(1);
        break;
    case 278: /* bexpr : LP expr RP -> p[2] */
        r = PD_NODE(2);
        break;
    case 296: { /* bexpr : ADDRESSOF singleid (p_addr_of_id) */
        PdId *id = (PdId *)rhs[1].value;
        r = addr_of_id(p, id->name, id->lineno);
        break;
    }
    case 314: { /* bexpr : ADDRESSOF ARRAY_ID arg_list (p_addr_of_array_element):
                 * make_array_access -> ARRAYACCESS, then UNARY[ADDRESS] with
                 * type uinteger, matching the production (parser.c:2276-2285:
                 * UNARY ADDRESS over parse_call_or_array(addressof_ctx=true)).
                 * make_call_node(expr_context=true) marks the entry accessed
                 * (== Python mark_entry_as_accessed). */
        const char *aname = PD_SVAL(2);
        int aln = PD_LINENO(2);
        AstNode *arglist = (AstNode *)rhs[2].value;
        if (!arglist) { r = NULL; break; } /* p[3] None -> p[0] None */
        AstNode *acc = make_call_node(p, aname, aln, arglist, true, true, false);
        if (!acc || acc->tag != AST_ARRAYACCESS) { r = NULL; break; }
        AstNode *n = ast_new(p->cs, AST_UNARY, PD_LINENO(1));
        n->u.unary.operator = arena_strdup(&p->cs->arena, "ADDRESS");
        ast_add_child(p->cs, n, acc);
        n->type_ = st->basic_types[TYPE_uinteger];
        r = n;
        break;
    }
    case 315: /* bexpr : ADDRESSOF ID arg_list (p_err_undefined_arr_access):
               * Python ALWAYS errors 'Undeclared array "%s"' + None when the
               * lexer gave ID (zxbparser.py:2833-2836). The OLD production
               * resolves `@name(args)` via parse_call_or_array and, for a
               * name that resolves to an array (lexer ID-vs-ARRAY_ID parse-
               * timing — including a self-referencing array init `DIM a(..)
               * => {@a(1)}` where `a` is not yet declared when @a(1) reduces),
               * builds a valid UNARY[ADDRESS, ARRAYACCESS] address instead of
               * erroring (arrlabels10d). The two cannot be distinguished at
               * this reduce, so defer the WHOLE production to Phase-E-reconcile
               * (the engine matches PYTHON, validated at the swap). */
        c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
        r = NULL;
        break;

    /* ---- call / array-access subsystem ----
     * argument : expr (321, p_argument) -> make_argument(expr) = AST_ARGUMENT
     * with byref=default_byref, child=expr, type_=expr->type_. */
    case 321: {
        AstNode *expr = PD_NODE(1);
        if (!expr) { r = NULL; break; }
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, expr->lineno);
        arg->u.argument.byref = p->cs->opts.default_byref;
        ast_add_child(p->cs, arg, expr);
        arg->type_ = expr->type_;
        r = arg;
        break;
    }
    case 322: { /* argument : ID WEQ expr (p_named_argument): make_argument(
                 * expr, name=ID). Match the production's named-ARGUMENT build
                 * (parser.c:2546-2551): name, byref=default, child=expr,
                 * type_=expr->type_, lineno=expr->lineno. */
        AstNode *expr = PD_NODE(3);
        if (!expr) { r = NULL; break; } /* make_argument None -> None */
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, expr->lineno);
        arg->u.argument.name = arena_strdup(&p->cs->arena, PD_SVAL(1));
        arg->u.argument.byref = p->cs->opts.default_byref;
        ast_add_child(p->cs, arg, expr);
        arg->type_ = expr->type_;
        r = arg;
        break;
    }
    case 323: { /* argument : ARRAY_ID (p_argument_array): access_array the
                 * whole-array name, mark accessed, wrap in ARGUMENT. The
                 * production reaches the bare ARRAY_ID argument via the id-read
                 * path and wraps the array entry node — so build the same
                 * ARGUMENT[entry] with byref=default, type_=entry->type_. */
        const char *aname = PD_SVAL(1);
        int aln = PD_LINENO(1);
        AstNode *entry = symboltable_access_array(p->cs->symbol_table, p->cs, aname, aln, NULL);
        if (!entry) { r = NULL; break; }  /* p[0] = None */
        mark_label_accessed(entry); /* mark_entry_as_accessed */
        AstNode *arg = ast_new(p->cs, AST_ARGUMENT, aln);
        arg->u.argument.byref = p->cs->opts.default_byref;
        ast_add_child(p->cs, arg, entry);
        arg->type_ = entry->type_;
        r = arg;
        break;
    }
    case 319: { /* arguments : argument -> ARGLIST[argument] */
        if (!rhs[0].value) {
            /* p_arguments (zxbparser.py:2865-2869): a NULL argument nulls the
             * whole arglist. The C PRODUCTION parser instead keeps the partial
             * (non-null) args (a recursive-descent error-recovery divergence
             * from Python) — so when the engine takes this Python-faithful
             * None path on an error file, the trees diverge from the old
             * production. Flag UNWIRED (Phase-E-reconcile: at the swap the
             * engine is validated against PYTHON, which it matches). */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = NULL; break;
        }
        AstNode *al = ast_new(p->cs, AST_ARGLIST, PD_LINENO(1));
        ast_add_child(p->cs, al, (AstNode *)rhs[0].value);
        r = al;
        break;
    }
    case 320: { /* arguments : arguments COMMA argument -> append */
        AstNode *al = (AstNode *)rhs[0].value;
        if (!al || !rhs[2].value) {
            /* p_arguments_argument (zxbparser.py:2874-2879): any NULL component
             * nulls the whole arglist (Python-faithful). The old production
             * keeps partial args, so defer this error file to Phase E. */
            c->unwired = true; if (c->unwired_prod == 0) c->unwired_prod = prodno;
            r = NULL; break;
        }
        ast_add_child(p->cs, al, (AstNode *)rhs[2].value);
        r = al;
        break;
    }
    case 317: /* arg_list : LP RP -> empty ARGLIST */
        r = ast_new(p->cs, AST_ARGLIST, PD_LINENO(1));
        break;
    case 318: /* arg_list : LP arguments RP -> the arguments ARGLIST */
        r = PD_NODE(2);
        break;

    /* ---- PRINT subsystem (zxbparser.py:1978-2073) ----
     * Every Python "list" reduce builds the PRINT SENTENCE and grows it; the
     * comparator checks tag/kind/children (and eol is set for Phase-E byte
     * correctness even though the comparator ignores it). */
    case 189: { /* print_elem : expr (p_print_elem_expr): pass the expr through,
                 * boolean -> ubyte typecast. */
        AstNode *e = PD_NODE(1);
        if (e) {
            const TypeInfo *et = e->type_;
            const TypeInfo *ef = (et && et->final_type) ? et->final_type : et;
            if (ef && ef->basic_type == TYPE_boolean) {
                AstNode *cast = make_typecast(p->cs, st->basic_types[TYPE_ubyte],
                                              e, PD_LINENO(1));
                if (cast) e = cast;
            }
        }
        r = e;  /* may be NULL (None) */
        break;
    }
    case 190: case 191: case 192: /* print_elem : print_at | print_tab | attr */
        r = PD_NODE(1);
        break;
    case 193: case 194: { /* print_elem : BOLD|ITALIC expr (p_print_list_expr):
                           * make_sentence("<NAME>_TMP", ubyte(expr)). */
        const char *nm = (prodno == 193) ? "BOLD_TMP" : "ITALIC_TMP";
        AstNode *cast = make_typecast(p->cs, st->basic_types[TYPE_ubyte],
                                      PD_NODE(2), PD_LINENO(1));
        AstNode *sen = make_sentence_node(p, nm, PD_LINENO(1));
        if (cast) ast_add_child(p->cs, sen, cast);
        r = sen;
        break;
    }
    case 203: /* print_elem : <empty> (p_print_list_epsilon) -> None */
        r = NULL;
        break;
    case 197: case 198: case 199:
    case 200: case 201: case 202: { /* attr : OVER|INVERSE|INK|PAPER|BRIGHT|
                                     * FLASH expr (p_attr): make_sentence(
                                     * "<NAME>_TMP", ubyte(expr)). */
        const char *nm =
            (prodno == 197) ? "OVER_TMP" :
            (prodno == 198) ? "INVERSE_TMP" :
            (prodno == 199) ? "INK_TMP" :
            (prodno == 200) ? "PAPER_TMP" :
            (prodno == 201) ? "BRIGHT_TMP" : "FLASH_TMP";
        AstNode *cast = make_typecast(p->cs, st->basic_types[TYPE_ubyte],
                                      PD_NODE(2), PD_LINENO(1));
        AstNode *sen = make_sentence_node(p, nm, PD_LINENO(1));
        if (cast) ast_add_child(p->cs, sen, cast);
        r = sen;
        break;
    }
    case 207: { /* print_at : AT expr COMMA expr (p_print_list_at) ->
                 * SENTENCE("PRINT_AT", ubyte(row), ubyte(col)). */
        AstNode *row = make_typecast(p->cs, st->basic_types[TYPE_ubyte],
                                     PD_NODE(2), PD_LINENO(1));
        AstNode *col = make_typecast(p->cs, st->basic_types[TYPE_ubyte],
                                     PD_NODE(4), PD_LINENO(3));
        AstNode *sen = make_sentence_node(p, "PRINT_AT", PD_LINENO(1));
        if (row) ast_add_child(p->cs, sen, row);
        if (col) ast_add_child(p->cs, sen, col);
        r = sen;
        break;
    }
    case 208: { /* print_tab : TAB expr (p_print_list_tab) ->
                 * SENTENCE("PRINT_TAB", ubyte(col)). */
        AstNode *col = make_typecast(p->cs, st->basic_types[TYPE_ubyte],
                                     PD_NODE(2), PD_LINENO(1));
        AstNode *sen = make_sentence_node(p, "PRINT_TAB", PD_LINENO(1));
        if (col) ast_add_child(p->cs, sen, col);
        r = sen;
        break;
    }
    case 204: { /* print_list : print_elem (p_print_list_elem): build the PRINT
                 * SENTENCE at p.lexer.lineno, eol=True, with the elem as a
                 * direct child (None filtered). */
        AstNode *sen = make_sentence_node(p, "PRINT", p->lexer.lineno);
        sen->u.sentence.eol = true;
        AstNode *elem = (AstNode *)rhs[0].value;
        if (elem) ast_add_child(p->cs, sen, elem);
        r = sen;
        break;
    }
    case 205: { /* print_list : print_list SC print_elem (p_print_list):
                 * eol = (p[3] is not None); append p[3] iff present; SC adds no
                 * separator child. */
        AstNode *sen = (AstNode *)rhs[0].value;
        AstNode *elem = (AstNode *)rhs[2].value;
        sen->u.sentence.eol = (elem != NULL);
        if (elem) ast_add_child(p->cs, sen, elem);
        r = sen;
        break;
    }
    case 206: { /* print_list : print_list COMMA print_elem (p_print_list_comma):
                 * eol = (p[3] is not None); append a PRINT_COMMA SENTENCE, then
                 * p[3] iff present. */
        AstNode *sen = (AstNode *)rhs[0].value;
        AstNode *elem = (AstNode *)rhs[2].value;
        sen->u.sentence.eol = (elem != NULL);
        AstNode *sep = make_sentence_node(p, "PRINT_COMMA", PD_LINENO(2));
        ast_add_child(p->cs, sen, sep);
        if (elem) ast_add_child(p->cs, sen, elem);
        r = sen;
        break;
    }
    case 188: /* statement : PRINT print_list (p_print_sentence) -> the
               * print_list SENTENCE; mark PRINT used. */
        p->cs->print_is_used = true;
        r = PD_NODE(2);
        break;

    /* ---- DATA / READ (157/161, gl.DATAS subsystem) ---- */
    case 157: { /* statement : DATA arguments (p_data): side-effect-only (no AST
                 * node — Python never sets p[0]); the per-item static/FUNCPTR
                 * split + gl.DATAS append via the shared data_build. */
        AstNode *al = (AstNode *)rhs[1].value;
        if (!al || al->child_count == 0) { r = NULL; break; }  /* p[2] None */
        /* :1742-1745  DATA not allowed within Functions nor Subs. */
        if (p->cs->function_level.len > 0) {
            zxbc_error(p->cs, PD_LINENO(1),
                       "DATA not allowed within Functions nor Subs");
            r = NULL;
            break;
        }
        int ln = PD_LINENO(1);
        /* :1734  label_ = make_label(DATA_PTR_CURRENT); record data_labels. */
        char *label_name = p->cs->data_ptr_current ? p->cs->data_ptr_current
                                                   : current_data_label(p->cs);
        AstNode *label_entry =
            symboltable_access_label(p->cs->symbol_table, p->cs, label_name, ln);
        if (label_entry)
            hashmap_set(&p->cs->data_labels, label_name,
                        p->cs->data_ptr_current ? p->cs->data_ptr_current : "");
        /* Unwrap each ARGUMENT to its value expr (Python reads d.value). */
        int n = al->child_count;
        AstNode **vals = arena_alloc(&p->cs->arena, (size_t)n * sizeof(AstNode *));
        for (int i = 0; i < n; i++) {
            AstNode *a = al->children[i];
            vals[i] = (a && a->tag == AST_ARGUMENT && a->child_count > 0)
                          ? a->children[0] : a;
        }
        data_build(p, vals, n, label_name, label_entry, ln);
        r = NULL;
        break;
    }
    case 161: { /* statement : READ arguments (p_read): make_block(*reads),
                 * each read = SENTENCE("READ", entry). Build via the shared
                 * read_build (per-target validation: array -> "Cannot read..",
                 * non-var/non-arrayaccess -> "Can only read..", else the READ).
                 * Python rebuilds an ARRAYLOAD target as a fresh ARRAYACCESS;
                 * read_build accepts AST_ARRAYACCESS directly (the engine's
                 * argument:expr builds the element access as ARRAYACCESS). */
        AstNode *al = (AstNode *)rhs[1].value;
        if (!al) { r = NULL; break; }  /* p[2] None -> p[0] None */
        int n = al->child_count;
        AstNode **targets = (n > 0)
            ? arena_alloc(&p->cs->arena, (size_t)n * sizeof(AstNode *)) : NULL;
        bool err_target = false;
        for (int i = 0; i < n; i++) {
            AstNode *a = al->children[i];
            AstNode *t = (a && a->tag == AST_ARGUMENT && a->child_count > 0)
                             ? a->children[0] : a;
            if (!t) { err_target = true; break; }  /* arg.value None -> p[0]=None */
            targets[i] = t;
        }
        if (err_target) { r = NULL; break; }
        r = read_build(p, targets, n, PD_LINENO(1));
        break;
    }
    case 299: case 301: { /* func_call : ID arg_list | ARRAY_ID arg_list
                           * (p_idcall_expr / p_arr_access_expr) -> make_call.
                           * In the LALR grammar this is a READ (expr context);
                           * the chained-postfix `(` is a separate production,
                           * so next_is_lp=false. p_arr_access_expr also marks
                           * the entry accessed — make_call_node's expr_context
                           * path does that. */
        const char *name = PD_SVAL(1);
        AstNode *al = (AstNode *)rhs[1].value;
        if (!al) { r = NULL; break; }
        r = make_call_node(p, name, PD_LINENO(1), al, true, false, false);
        break;
    }
    case 298: /* bexpr : func_call (p_expr_funccall) -> p[1] */
        r = PD_NODE(1);
        break;
    case 295: { /* bexpr : ID  (p_id_expr, zxbparser.py:2647). access_id with
                 * default_class=var, mark accessed, auto-type -> DEFAULT_TYPE
                 * + warning. Faithful to parser.c:2292-2314 scalar-read core.
                 * The FUNCTION (0-arg/parenless) and array branches are NOT
                 * this production in the LALR grammar (they are bexpr:ID bexpr
                 * / func_call / array productions); if the resolved entry is a
                 * function/sub/array, leave to those (flag unwired so we never
                 * emit a wrong tree). */
        const char *name = PD_SVAL(1);
        int ln = PD_LINENO(1);
        AstNode *entry = symboltable_access_id(p->cs->symbol_table, p->cs,
                                               name, ln, NULL, CLASS_var);
        if (!entry) {
            /* access_id error (explicit mode) — Python returns None. */
            r = NULL;
            break;
        }
        /* mark accessed + auto-type promotion happen BEFORE the class
         * dispatch (p_id_expr, zxbparser.py:2647-2672). */
        entry->u.id.accessed = true;
        if (entry->type_ && entry->type_->final_type &&
            entry->type_->final_type->basic_type == TYPE_unknown) {
            TypeInfo *promoted = type_new_ref(p->cs, p->cs->default_type, ln, true);
            entry->type_ = promoted;
            warn_implicit_type(p->cs, ln, name, p->cs->default_type->name);
        }
        if (entry->u.id.class_ == CLASS_array) {
            /* p_id_expr array branch: NOT in a LET assignment -> error +
             * None; in a LET assignment -> keep the entry. */
            if (!p->cs->let_assignment) {
                zxbc_error(p->cs, ln,
                           "Variable '%s' is an array and cannot be used in this context",
                           name);
                r = NULL;
            } else {
                r = entry;
            }
            break;
        }
        if (entry->u.id.class_ == CLASS_function) {
            /* Function call with 0 args: make_call(name, lineno,
             * make_arg_list(None)) — an empty ARGLIST -> FUNCCALL[id,
             * ARGLIST()]. */
            AstNode *empty_al = ast_new(p->cs, AST_ARGLIST, ln);
            r = make_call_node(p, name, ln, empty_al, true, false, false);
            break;
        }
        if (entry->u.id.class_ == CLASS_sub) {
            /* Forbidden: a SUB used as a function value. */
            zxbc_error(p->cs, ln, "'%s' is a SUB not a FUNCTION", name);
            r = NULL;
            break;
        }
        if (entry->u.id.class_ == CLASS_unknown)
            entry->u.id.class_ = CLASS_var;
        r = entry;
        break;
    }

    /* ---- binary operators (expr OP expr). make_binary_node does the type
     * coercion + constant folding + CONSTEXPR/string-concat exactly as the
     * production parser's parse_infix; the operator strings match
     * operator_name() so the C-vs-C AST compare is self-consistent. p.lineno
     * is the OPERATOR's line (p.lineno(2)).
     *
     * pd_binary mirrors parse_infix's error recovery EXACTLY: when
     * make_binary_node returns NULL (a type error, e.g. `"A" + i` -> "Cannot
     * convert value to string"), parse_infix does `if (!result) return left;`
     * (parser.c:3522) — i.e. it keeps the LEFT operand and drops the operator.
     * Python's p_expr_*_expr instead propagates None, but the C-vs-C compare is
     * against the PRODUCTION parser (the byte-clean swap baseline), which takes
     * this recovery; matching it keeps DIFF 0 (surfaced by typecast1 once PRINT
     * was wired). Affects only error fixtures (exit 1; the partial tree never
     * reaches codegen). ---- */
    case 255: r = pd_binary(p, "PLUS", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 256: r = pd_binary(p, "MINUS", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 257: r = pd_binary(p, "MULT", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 258: r = pd_binary(p, "DIV", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 259: r = pd_binary(p, "MOD", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 260: r = pd_binary(p, "POW", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 261: r = pd_binary(p, "SHL", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 262: r = pd_binary(p, "SHR", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 264: r = pd_binary(p, "EQ", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 265: r = pd_binary(p, "LT", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 266: r = pd_binary(p, "LE", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 267: r = pd_binary(p, "GT", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 268: r = pd_binary(p, "GE", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 269: r = pd_binary(p, "NE", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 270: r = pd_binary(p, "OR", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 271: r = pd_binary(p, "BOR", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 272: r = pd_binary(p, "XOR", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 273: r = pd_binary(p, "BXOR", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 274: r = pd_binary(p, "AND", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;
    case 275: r = pd_binary(p, "BAND", PD_NODE(1), PD_NODE(3), PD_LINENO(2)); break;

    /* ---- unary operators ---- */
    case 263: /* expr : MINUS expr %prec UMINUS */
        r = make_unary_node(p->cs, "MINUS", PD_NODE(2), PD_LINENO(1));
        break;
    case 276: /* expr : NOT expr */
        r = make_unary_node(p->cs, "NOT", PD_NODE(2), PD_LINENO(1));
        break;
    case 277: /* expr : BNOT expr */
        r = make_unary_node(p->cs, "BNOT", PD_NODE(2), PD_LINENO(1));
        break;

    /* ---- simple statements ---- */
    case 56: /* statement : BORDER expr */
        r = make_sentence_node(p, "BORDER", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_ubyte], PD_NODE(2), PD_LINENO(1)));
        break;
    case 65: /* statement : CLS */
        r = make_sentence_node(p, "CLS", PD_LINENO(1));
        break;

    /* ---- attr_list (graphics attrs, p_attr_list 195/196) ----
     * make_block flatten of the per-attr SENTENCEs: a lone attr stays the
     * SENTENCE; N>=2 a flat BLOCK (zxbparser.py:2006-2016). The attr
     * SENTENCEs are built by cases 197-202. */
    case 195: /* attr_list : attr SC -> the lone attr */
        r = PD_NODE(1);
        break;
    case 196: { /* attr_list : attr_list attr SC -> make_block(p[1], p[2]) */
        AstNode *al = PD_NODE(1);
        AstNode *attr = PD_NODE(2);
        if (al && al->tag != AST_BLOCK) {
            AstNode *b = make_block_node(p, al->lineno);
            ast_add_child(p->cs, b, al);
            al = b;
        } else if (!al) {
            al = make_block_node(p, PD_LINENO(1));
        }
        if (attr) ast_add_child(p->cs, al, attr);
        r = al;
        break;
    }

    /* ---- PLOT / DRAW / DRAW3 / CIRCLE (+/- attr_list) (57-64) ----
     * The production typecasts every coordinate at the KEYWORD line (`ln`),
     * NOT Python's per-expr lineno, and appends the attr_list (if any) as the
     * LAST child (parser.c:4363-4442). Match C-vs-C. */
    case 57: case 58: { /* PLOT [attr_list] expr COMMA expr -> ubyte x,y */
        int a = (prodno == 58) ? 1 : 0;  /* attr offset */
        int ln = PD_LINENO(1);
        AstNode *attr = (prodno == 58) ? PD_NODE(2) : NULL;
        r = make_sentence_node(p, "PLOT", ln);
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_ubyte], rhs[a+1].value, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_ubyte], rhs[a+3].value, ln));
        if (attr) ast_add_child(p->cs, r, attr);
        break;
    }
    case 61: case 62: { /* DRAW [attr_list] expr COMMA expr -> integer x,y */
        int a = (prodno == 62) ? 1 : 0;
        int ln = PD_LINENO(1);
        AstNode *attr = (prodno == 62) ? PD_NODE(2) : NULL;
        r = make_sentence_node(p, "DRAW", ln);
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_integer], rhs[a+1].value, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_integer], rhs[a+3].value, ln));
        if (attr) ast_add_child(p->cs, r, attr);
        break;
    }
    case 59: case 60: { /* DRAW [attr_list] expr COMMA expr COMMA expr -> DRAW3
                         * integer x,y + float z */
        int a = (prodno == 60) ? 1 : 0;
        int ln = PD_LINENO(1);
        AstNode *attr = (prodno == 60) ? PD_NODE(2) : NULL;
        r = make_sentence_node(p, "DRAW3", ln);
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_integer], rhs[a+1].value, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_integer], rhs[a+3].value, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_float], rhs[a+5].value, ln));
        if (attr) ast_add_child(p->cs, r, attr);
        break;
    }
    case 63: case 64: { /* CIRCLE [attr_list] expr COMMA expr COMMA expr ->
                         * byte x,y,r (all signed) */
        int a = (prodno == 64) ? 1 : 0;
        int ln = PD_LINENO(1);
        AstNode *attr = (prodno == 64) ? PD_NODE(2) : NULL;
        r = make_sentence_node(p, "CIRCLE", ln);
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_byte], rhs[a+1].value, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_byte], rhs[a+3].value, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, st->basic_types[TYPE_byte], rhs[a+5].value, ln));
        if (attr) ast_add_child(p->cs, r, attr);
        break;
    }
    case 144: /* statement : END expr (p_end) -> END[expr] (RAW, matching the
               * production parser.c:4180-4187 which adds the expr raw). */
        r = make_sentence_node(p, "END", PD_LINENO(1));
        ast_add_child(p->cs, r, PD_NODE(2));
        break;
    case 145: /* statement : END (bare) -> END[NUMBER(0, uinteger)] matching
               * the production (parser.c:4185 — NUMBER(0) typed uinteger, not
               * Python's auto-ubyte). */
        r = make_sentence_node(p, "END", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_number(p, 0, PD_LINENO(1), st->basic_types[TYPE_uinteger]));
        break;
    case 146: /* statement : ERROR expr (p_error_raise) -> ERROR[expr] RAW.
               * The C production (parser.c:4191-4196) adds the code expr raw
               * (NOT Python's MINUS(typecast(ubyte,expr),1)) — match C-vs-C. */
        r = make_sentence_node(p, "ERROR", PD_LINENO(1));
        ast_add_child(p->cs, r, PD_NODE(2));
        break;
    case 147: /* statement : STOP expr (p_stop_raise) -> STOP[expr] RAW
               * (production parser.c:4209 keeps the explicit expr raw). */
        r = make_sentence_node(p, "STOP", PD_LINENO(1));
        ast_add_child(p->cs, r, PD_NODE(2));
        break;
    case 148: { /* statement : STOP (bare) -> STOP[MINUS(typecast(ubyte,
                 * NUMBER(9)), NUMBER(1))] folding to 8 (production
                 * parser.c:4211-4216). */
        int ln = PD_LINENO(1);
        AstNode *nine = make_number(p, 9, ln, NULL);
        AstNode *q = make_typecast(p->cs, st->basic_types[TYPE_ubyte], nine, ln);
        AstNode *one = make_number(p, 1, ln, NULL);
        AstNode *code = make_binary_node(p->cs, "MINUS", q, one, ln, NULL);
        r = make_sentence_node(p, "STOP", ln);
        ast_add_child(p->cs, r, code);
        break;
    }
    case 66: /* statement : ASM (make_asm_sentence(p[1], lineno)) */
        r = make_asm_node(p, PD_SVAL(1) ? PD_SVAL(1) : "", PD_LINENO(1));
        break;
    case 216: /* statement : PAUSE expr */
        r = make_sentence_node(p, "PAUSE", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(2), PD_LINENO(1)));
        break;

    /* ---- GOTO / GOSUB ----
     * goto : GO TO | GO SUB | GOTO | GOSUB (p_go): result is the kind string;
     * GOSUB inside a SUB/FUNCTION is an error (checked here, p_go timing).
     * We carry the kind as a PdId{name=kind}. */
    case 86: case 87: case 88: case 89: {
        const char *kind;
        int ln = PD_LINENO(1);
        if (prodno == 86) kind = "GOTO";       /* GO TO */
        else if (prodno == 87) kind = "GOSUB"; /* GO SUB */
        else if (prodno == 88) kind = "GOTO";  /* GOTO */
        else kind = "GOSUB";                   /* GOSUB */
        if (strcmp(kind, "GOSUB") == 0 && p->cs->function_level.len > 0)
            zxbc_error(p->cs, ln, "GOSUB not allowed within SUB or FUNCTION");
        PdId *k = pd_new_id(p, kind, ln);
        *out = k;
        *out_lineno = ln;
        return true;
    }
    case 84: case 85: { /* statement : goto NUMBER | goto ID
                         * p_goto: entry = check_and_make_label(p[2], ln);
                         * make_sentence(p[1].upper(), entry). To match the
                         * production parser's tree (C-vs-C compare) build the
                         * SENTENCE(kind) with a single AST_ID label child
                         * (name, class_=label), as parser.c:3705-3711. */
        PdId *k = (PdId *)rhs[0].value;
        int ln = k->lineno;
        const char *label;
        char buf[32];
        if (prodno == 84) { /* goto NUMBER — label is the integer rendered */
            snprintf(buf, sizeof(buf), "%d", (int)PD_NUM(2));
            label = buf;
        } else {            /* goto ID */
            label = PD_SVAL(2) ? PD_SVAL(2) : "";
        }
        r = make_sentence_node(p, k->name, ln);
        AstNode *lbl = ast_new(p->cs, AST_ID, ln);
        lbl->u.id.name = arena_strdup(&p->cs->arena, label);
        lbl->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, r, lbl);
        break;
    }

    /* ---- ON expr GOTO|GOSUB label_list (p_on_goto, 209-213) ----
     * label_list carries the label nodes (Python: a list of check_and_make_label
     * entries, zxbparser.py:2082-2096). Carry them as the children of an
     * AST_ARGLIST container; p_on_goto spreads them. Each label is an
     * AST_ID(name, class_=label), matching the production (parser.c:4615-4617).
     * make_label_id builds the rendered label name (NUMBER -> its integer). */
    case 210: case 211: case 212: { /* label_list : ID | NUMBER |
                                     * label_list COMMA ID|NUMBER */
        AstNode *lst;
        int idx;  /* 0-based RHS index of the new ID/NUMBER token */
        if (prodno == 212) { lst = (AstNode *)rhs[0].value; idx = 2; }
        else { lst = ast_new(p->cs, AST_ARGLIST, PD_LINENO(1)); idx = 0; }
        const char *label;
        char buf[32];
        if (rhs[idx].sval) label = rhs[idx].sval;
        else { snprintf(buf, sizeof(buf), "%d", (int)rhs[idx].num); label = buf; }
        AstNode *lbl = ast_new(p->cs, AST_ID, rhs[idx].lineno);
        lbl->u.id.name = arena_strdup(&p->cs->arena, label);
        lbl->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, lst, lbl);
        *out = lst; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 213: { /* label_list : label_list COMMA NUMBER (same as 212's NUMBER
                 * arm, kept distinct in the grammar) */
        AstNode *lst = (AstNode *)rhs[0].value;
        char buf[32];
        const char *label = rhs[2].sval;
        if (!label) { snprintf(buf, sizeof(buf), "%d", (int)rhs[2].num); label = buf; }
        AstNode *lbl = ast_new(p->cs, AST_ID, rhs[2].lineno);
        lbl->u.id.name = arena_strdup(&p->cs->arena, label);
        lbl->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, lst, lbl);
        *out = lst; *out_lineno = PD_LINENO(1);
        return true;
    }
    case 209: { /* statement : ON expr goto label_list (p_on_goto) ->
                 * SENTENCE("ON_" + goto_kind, expr, *labels). The production
                 * adds expr RAW (parser.c:4608, no typecast) — match C-vs-C. */
        AstNode *expr = PD_NODE(2);
        PdId *gk = (PdId *)rhs[2].value;  /* goto: "GOTO"/"GOSUB" */
        AstNode *lst = (AstNode *)rhs[3].value;
        char kind[16];
        snprintf(kind, sizeof(kind), "ON_%s", gk ? gk->name : "GOTO");
        r = make_sentence_node(p, kind, PD_LINENO(1));
        if (expr) ast_add_child(p->cs, r, expr);
        if (lst) for (int i = 0; i < lst->child_count; i++)
            ast_add_child(p->cs, r, lst->children[i]);
        break;
    }
    case 217: /* statement : POKE expr COMMA expr */
        r = make_sentence_node(p, "POKE", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(2), PD_LINENO(3)));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_ubyte], PD_NODE(4), PD_LINENO(3)));
        break;
    case 218: /* statement : POKE LP expr COMMA expr RP */
        r = make_sentence_node(p, "POKE", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(3), PD_LINENO(4)));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_ubyte], PD_NODE(5), PD_LINENO(4)));
        break;
    case 219: case 220: case 221: case 222: {
        /* statement : POKE [LP] numbertype [COMMA] expr COMMA expr [RP]
         * (p_poke2 219/220, p_poke3 221/222). The production stores the
         * numbertype on the POKE sentence's type_ and typecasts addr->uinteger,
         * val->numbertype, both at the POKE line (parser.c:4214-4248). */
        bool paren = (prodno == 220 || prodno == 222);
        bool poke3 = (prodno == 221 || prodno == 222);
        int ti = paren ? 2 : 1;          /* numbertype 0-based index */
        int ai = ti + 1 + (poke3 ? 1 : 0); /* addr after type (and COMMA for poke3) */
        int vi = ai + 2;                  /* val after `addr COMMA` */
        TypeInfo *nt = (TypeInfo *)rhs[ti].value;
        AstNode *addr = (AstNode *)rhs[ai].value;
        AstNode *val = (AstNode *)rhs[vi].value;
        if (!addr || !val) { r = NULL; break; } /* p[0] = None */
        int ln = PD_LINENO(1);
        r = make_sentence_node(p, "POKE", ln);
        if (nt) r->type_ = nt;
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_uinteger], addr, ln));
        ast_add_child(p->cs, r, make_typecast(p->cs, nt, val, ln));
        break;
    }
    case 67: /* statement : RANDOMIZE */
        r = make_sentence_node(p, "RANDOMIZE", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_number(p, 0, PD_LINENO(1), st->basic_types[TYPE_ulong]));
        break;
    case 68: /* statement : RANDOMIZE expr */
        r = make_sentence_node(p, "RANDOMIZE", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_ulong], PD_NODE(2), PD_LINENO(1)));
        break;
    case 69: /* statement : BEEP expr COMMA expr */
        r = make_sentence_node(p, "BEEP", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_float], PD_NODE(2), PD_LINENO(1)));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_float], PD_NODE(4), PD_LINENO(3)));
        break;
    case 223: /* statement : OUT expr COMMA expr */
        r = make_sentence_node(p, "OUT", PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_uinteger], PD_NODE(2), PD_LINENO(3)));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_ubyte], PD_NODE(4), PD_LINENO(4)));
        break;
    /* p_simple_instruction (one prodno per keyword): make_sentence(KIND,
     * typecast(ubyte, p[2])). Kind == the keyword text. */
    case 224: case 225: case 226: case 227:
    case 228: case 229: case 230: case 231: {
        const char *kind =
            (prodno == 224) ? "ITALIC" :
            (prodno == 225) ? "BOLD" :
            (prodno == 226) ? "INK" :
            (prodno == 227) ? "PAPER" :
            (prodno == 228) ? "BRIGHT" :
            (prodno == 229) ? "FLASH" :
            (prodno == 230) ? "OVER" : "INVERSE";
        r = make_sentence_node(p, kind, PD_LINENO(1));
        ast_add_child(p->cs, r,
            make_typecast(p->cs, st->basic_types[TYPE_ubyte], PD_NODE(2), PD_LINENO(1)));
        break;
    }

    default:
        /* Production not yet ported — flag for the harness. */
        c->unwired = true;
        if (c->unwired_prod == 0) c->unwired_prod = prodno;
        r = make_nop(p); /* keep the parse going so we can survey coverage */
        break;
    }

    *out = r;
    *out_lineno = (r ? r->lineno : rln);
    return true;
}

/* PLY symbol id for NEWLINE (resolved once; see ply_sym_name). */
static int pd_newline_sym_id(void) {
    static int id = -2;
    if (id == -2) id = ply_term_id("NEWLINE");
    return id;
}

/* Render the error-token's PLY `p.value` exactly as Python's lexer would.
 * The engine's errtoken is the current lookahead == the BToken pd_lex last
 * returned (c->last_btoken), so reuse the production's tok_ply_value (correct
 * for punctuation lexemes, the upper-cased keyword reserved-word value, ID/
 * STRC source text, and the NUMBER float repr). EXCEPTION: a preproc-state
 * token whose PLY type is INTEGER/STRING carries the raw text in sval — Python
 * sees that string as p.value, not the float — so use sval there. */
static const char *pd_errtok_value(const PlySym *t, const BToken *bt,
                                   char *buf, size_t sz) {
    static int int_id = -2, str_id = -2;
    if (int_id == -2) { int_id = ply_term_id("INTEGER"); str_id = ply_term_id("STRING"); }
    if ((t->type == int_id || t->type == str_id) && t->sval)
        return t->sval;
    if (bt) return tok_ply_value(bt, buf, sz);
    if (t->sval) return t->sval;
    return "?";
}

/* p_error (zxbparser.py:3561-3583) — faithful port. errtoken==NULL means EOF.
 * For a token: "Syntax Error. Unexpected token '%s' <%s>" (value, type), or
 * "Unexpected end of line" for NEWLINE, at the token's lineno. For EOF: emit
 * the loop-not-closed hint off the live cs->loop_stack (gl.LOOPS), then (unless
 * there were prior errors — gl.has_errors) "Unexpected end of file" at the
 * final lexer line. Does NOT set c->unwired — the engine recovers exactly as
 * PLY (error_count=3) and keeps parsing. */
static void pd_error(void *ud, const PlySym *errtoken) {
    PdCtx *c = ud;
    Parser *p = c->p;
    CompilerState *cs = p->cs;

    c->p_error_fired = true;
    if (!c->emit_errors) {
        /* astcmp-meter mode: a p_error file is honestly classified UNWIRED:-1
         * (the engine is PLY-faithful; the production tolerates raw artifacts),
         * NOT a DIFF. No message emitted so the valid-corpus meter + DIFF=0 are
         * unchanged. The real message is emitted in emit_errors mode below. */
        c->unwired = true;
        if (c->unwired_prod == 0) c->unwired_prod = -1;
        return;
    }

    if (errtoken != NULL) {
        if (errtoken->type != pd_newline_sym_id()) {
            char buf[64];
            const BToken *bt = (c->last_btoken_set &&
                                c->last_btoken.lineno == errtoken->lineno)
                                   ? &c->last_btoken : NULL;
            const char *val = pd_errtok_value(errtoken, bt, buf, sizeof(buf));
            const char *tname = (errtoken->type >= 0 && errtoken->type < PLY_N_SYM)
                                    ? ply_sym_name[errtoken->type] : "?";
            zxbc_error(cs, errtoken->lineno,
                       "Syntax Error. Unexpected token '%s' <%s>", val, tname);
        } else {
            zxbc_error(cs, errtoken->lineno, "Unexpected end of line");
        }
        return;
    }

    /* EOF (None). Loop-not-closed hints from the live loop stack (gl.LOOPS). */
    if (cs->loop_stack.len > 0) {
        LoopInfo *li = &cs->loop_stack.data[cs->loop_stack.len - 1];
        if (li->type == LOOP_FOR)
            err_for_without_next(cs, li->lineno);
        else
            err_loop_not_closed(cs, li->lineno,
                                li->type == LOOP_WHILE ? "WHILE" : "DO");
    }
    /* If there were previous errors, this EOF is a consequence of them
     * (gl.has_errors) — do not also emit "Unexpected end of file". */
    if (cs->error_count > 0)
        return;
    zxbc_error(cs, c->last_lineno, "Unexpected end of file");
}

/* Init for the PLY-engine path: like parser_init but WITHOUT priming the
 * first token (the engine pulls tokens itself via pd_lex). */
void parser_init_noprime(Parser *p, CompilerState *cs, const char *input) {
    memset(p, 0, sizeof(*p));
    p->cs = cs;
    blexer_init(&p->lexer, cs, input);
    p->had_error = false;
    p->panic_mode = false;
    tokdump_init();
}

/* Build the program via the PLY engine. Returns the engine's `start` value
 * (the program BLOCK before the implicit-END / data-decl finalisation, which
 * Phase E will add). *unwired_out reports whether any unported production or
 * p_error fired. */
AstNode *plyparse_program(Parser *p, bool *unwired_out, int *unwired_prod_out) {
    PdCtx c;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.last_lineno = p->lexer.lineno;

    PlyParser eng;
    ply_parser_init(&eng, pd_lex, pd_action, pd_error, &c);
    eng.cur_lineno = p->lexer.lineno;
    void *res = ply_parse(&eng);

    if (unwired_out) *unwired_out = c.unwired;
    if (unwired_prod_out) *unwired_prod_out = c.unwired_prod;
    return (AstNode *)res;
}

/* Phase C-full ERROR-EMIT mode: pd_error emits the real p_error message+line. */
AstNode *plyparse_program_emit_errors(Parser *p, bool *p_error_fired_out) {
    PdCtx c;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.last_lineno = p->lexer.lineno;
    c.emit_errors = true;

    PlyParser eng;
    ply_parser_init(&eng, pd_lex, pd_action, pd_error, &c);
    eng.cur_lineno = p->lexer.lineno;
    void *res = ply_parse(&eng);

    if (p_error_fired_out) *p_error_fired_out = c.p_error_fired;
    return (AstNode *)res;
}
