/*
 * Recursive-descent parser for Z80 assembly.
 * Mirrors the grammar in src/zxbasm/asmparse.py
 *
 * The parser works on a token stream from lexer.c and builds
 * AsmInstr objects that are added to the Memory model.
 */
#include "zxbasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "compat.h"   /* access()/R_OK, cross-platform */

/* Token types, Lexer, Token are all declared in zxbasm.h */

/* ----------------------------------------------------------------
 * Parser state
 * ---------------------------------------------------------------- */
typedef struct Parser {
    AsmState *as;
    Lexer lex;
    Token cur;       /* current token */
    Token peek_tok;  /* one-token lookahead */
    bool has_peek;
    /* PLY error-recovery state (ply/yacc.py:79 `error_count = 3`). PLY
     * uses an LALR state machine where after p_error fires, subsequent
     * `errorcount > 0` triggers from inside error-recovery silently
     * reset the counter without invoking errorfunc; only on the FIRST
     * malformed token AFTER a fully-cleared recovery does p_error fire
     * again. The exact shift-counting is state-machine-dependent and
     * not portable to recursive descent without a parse table. We
     * approximate the empirically-observed behaviour on the
     * err_pragma_namespace_cascade probe: a `pragma X Y` line whose
     * recovery does NOT declare a fresh label (no asm:ID reduce shifted
     * a usable symbol) is followed by a SILENT error on the next line.
     * `prev_err_no_decl` tracks that condition; when set, the next
     * unexpected-token report is suppressed. */
    bool prev_err_no_decl;
    bool decl_since_err;
    /* Per-statement: did the current parse_asm/statement make any
     * mem_declare_label call? Reset at parse_program's statement-start;
     * read at parse_program's statement-end to decide whether the next
     * line's first asm_unexpected should be cascade-suppressed. This is
     * distinct from `decl_since_err`, which tracks decls between asm_
     * unexpected calls within a single line. */
    bool stmt_had_decl;
} Parser;

static void parser_init(Parser *p, AsmState *as, const char *input)
{
    p->as = as;
    lexer_init(&p->lex, as, input);
    p->has_peek = false;
    p->prev_err_no_decl = false;
    p->decl_since_err = false;
    p->stmt_had_decl = false;
    p->cur = lexer_next(&p->lex);
}

static Token parser_peek(Parser *p)
{
    if (!p->has_peek) {
        p->peek_tok = lexer_next(&p->lex);
        p->has_peek = true;
    }
    return p->peek_tok;
}

static void parser_advance(Parser *p)
{
    if (p->has_peek) {
        p->cur = p->peek_tok;
        p->has_peek = false;
    } else {
        p->cur = lexer_next(&p->lex);
    }
}

static void asm_unexpected(Parser *p);

static bool parser_match(Parser *p, TokenType type)
{
    if (p->cur.type == type) {
        parser_advance(p);
        return true;
    }
    return false;
}

static bool parser_expect(Parser *p, TokenType type)
{
    if (p->cur.type == type) {
        parser_advance(p);
        return true;
    }
    /* Route through the centralised PLY p_error formatter so the
     * unexpected-token report carries the source lexeme (Python's p.value)
     * and the canonical PLY token NAME (e.g. '~' [BXOR], 'HL' [HL])
     * instead of the C-side enum id. asm_unexpected handles both the
     * NEWLINE/EOF branch and the normal token branch. */
    asm_unexpected(p);
    return false;
}

/* Skip to next newline (error recovery) */
static void parser_skip_to_newline(Parser *p)
{
    while (p->cur.type != TOK_NEWLINE && p->cur.type != TOK_EOF) {
        parser_advance(p);
    }
}

/* ----------------------------------------------------------------
 * Helper: Check if token is a register
 * ---------------------------------------------------------------- */
static bool is_reg8(TokenType t)
{
    return t == TOK_B || t == TOK_C || t == TOK_D || t == TOK_E ||
           t == TOK_H || t == TOK_L;
}

static bool is_reg8_bcde(TokenType t)
{
    return t == TOK_B || t == TOK_C || t == TOK_D || t == TOK_E;
}

static bool is_reg8i(TokenType t)
{
    return t == TOK_IXH || t == TOK_IXL || t == TOK_IYH || t == TOK_IYL;
}

static bool is_reg16(TokenType t)
{
    return t == TOK_BC || t == TOK_DE || t == TOK_HL || t == TOK_IX || t == TOK_IY;
}

static bool is_reg16i(TokenType t)
{
    return t == TOK_IX || t == TOK_IY;
}

static bool is_jp_flag(TokenType t)
{
    return t == TOK_Z || t == TOK_NZ || t == TOK_C || t == TOK_NC ||
           t == TOK_PO || t == TOK_PE || t == TOK_P || t == TOK_M;
}

static bool is_jr_flag(TokenType t)
{
    return t == TOK_Z || t == TOK_NZ || t == TOK_C || t == TOK_NC;
}

/* Get register name string */
static const char *reg_name(TokenType t)
{
    switch (t) {
    case TOK_A: return "A"; case TOK_B: return "B"; case TOK_C: return "C";
    case TOK_D: return "D"; case TOK_E: return "E"; case TOK_H: return "H";
    case TOK_L: return "L"; case TOK_I: return "I"; case TOK_R: return "R";
    case TOK_IXH: return "IXH"; case TOK_IXL: return "IXL";
    case TOK_IYH: return "IYH"; case TOK_IYL: return "IYL";
    case TOK_AF: return "AF"; case TOK_BC: return "BC"; case TOK_DE: return "DE";
    case TOK_HL: return "HL"; case TOK_IX: return "IX"; case TOK_IY: return "IY";
    case TOK_SP: return "SP";
    case TOK_Z: return "Z"; case TOK_NZ: return "NZ"; case TOK_NC: return "NC";
    case TOK_PO: return "PO"; case TOK_PE: return "PE";
    case TOK_P: return "P"; case TOK_M: return "M";
    default: return "?";
    }
}

/* PLY token-type name for p_error's "[<TYPE>]" field — the names the
 * Python lexer assigns (src/zxbasm/asmlex.py:17-227): punctuation/
 * operators have dedicated names (':' -> CO, ')' -> RP, '*' -> MUL, …),
 * literals are INTEGER/STRING/ID, and registers/flags/instructions carry
 * their uppercase spelling (reg_name covers the registers and flags).
 * For an instruction/pseudo-op (rare as an "unexpected" token) the name
 * is the uppercased mnemonic, written into `buf`. */
static const char *tok_ply_name(TokenType t, const char *sval,
                                char *buf, size_t bufsz) {
    switch (t) {
    case TOK_NEWLINE: return "NEWLINE";
    case TOK_COLON:   return "CO";
    case TOK_COMMA:   return "COMMA";
    case TOK_PLUS:    return "PLUS";
    case TOK_MINUS:   return "MINUS";
    case TOK_MUL:     return "MUL";
    case TOK_DIV:     return "DIV";
    case TOK_MOD:     return "MOD";
    case TOK_POW:     return "POW";
    case TOK_LSHIFT:  return "LSHIFT";
    case TOK_RSHIFT:  return "RSHIFT";
    case TOK_BAND:    return "BAND";
    case TOK_BOR:     return "BOR";
    case TOK_BXOR:    return "BXOR";
    case TOK_LP:      return "LP";
    case TOK_RP:      return "RP";
    case TOK_LB:      return "LB";
    case TOK_RB:      return "RB";
    case TOK_APO:     return "APO";
    case TOK_ADDR:    return "ADDR";
    case TOK_INTEGER: return "INTEGER";
    case TOK_STRING:  return "STRING";
    case TOK_ID:      return "ID";
    default: {
        const char *rn = reg_name(t);
        if (rn[0] != '?') return rn;   /* registers + flags */
        /* instruction / pseudo-op: uppercase the mnemonic spelling */
        if (sval && buf && bufsz) {
            size_t i = 0;
            for (; sval[i] && i + 1 < bufsz; i++)
                buf[i] = (char)toupper((unsigned char)sval[i]);
            buf[i] = '\0';
            return buf;
        }
        return "?";
    }
    }
}

/* Display value for p_error's "'<value>'" field — Python prints p.value:
 * the literal text for punctuation, the decimal value for INTEGER, and
 * the lexeme for ID/STRING/registers/instructions. */
static const char *tok_display_value(const Token *tk, char *buf, size_t bufsz) {
    switch (tk->type) {
    case TOK_COLON:  return ":";
    case TOK_COMMA:  return ",";
    case TOK_PLUS:   return "+";
    case TOK_MINUS:  return "-";
    case TOK_MUL:    return "*";
    case TOK_DIV:    return "/";
    case TOK_MOD:    return "%";
    case TOK_POW:    return "^";
    case TOK_LSHIFT: return "<<";
    case TOK_RSHIFT: return ">>";
    case TOK_BAND:   return "&";
    case TOK_BOR:    return "|";
    case TOK_BXOR:   return "~";
    case TOK_LP:     return "(";
    case TOK_RP:     return ")";
    case TOK_LB:     return "[";
    case TOK_RB:     return "]";
    case TOK_APO:    return "'";
    case TOK_ADDR:   return "$";
    case TOK_INTEGER:
        snprintf(buf, bufsz, "%lld", (long long)tk->ival);
        return buf;
    case TOK_ID:
    case TOK_STRING:
        return tk->sval ? tk->sval : "";
    default:
        /* register / flag / instruction / pseudo-op: Python's PLY ID lexer
         * (src/zxbasm/asmlex.py:300-304) sets t.value = tmp.upper() for
         * anything matching reserved_instructions/pseudo/regs/flags — so
         * p.value in p_error is the UPPERCASE lexeme, not the source
         * spelling. tok.sval is already uppercased for keywords (lexer.c
         * t_INITIAL_ID port); tok.original_id is the verbatim source case
         * (lower for `hl`, etc.). Prefer sval so `LD A, hl` reports
         * 'HL' [HL] not 'hl' [HL]. */
        if (tk->sval) return tk->sval;
        if (tk->original_id) return tk->original_id;
        return reg_name(tk->type);
    }
}

/* Centralised PLY p_error (src/zxbasm/asmparse.py:981-986): a NEWLINE/EOF
 * is "Unexpected end of line [NEWLINE]", anything else is
 * "Unexpected token '<value>' [<TYPE>]".  Replaces the C parser's
 * ad-hoc "Expected expression"/"Syntax error"/"Unexpected token" sites
 * so the diagnostic text and the offending-token report match Python. */
static void asm_unexpected(Parser *p) {
    /* PLY error-recovery suppression (ply/yacc.py:79 `errorcount`).
     *
     * After p_error fires, PLY's state machine often ends up in a state
     * where the next bad line's lookahead immediately re-errors with
     * `errorcount > 0` (so p_error is NOT re-invoked — the offending
     * token is silently discarded). Empirically on the
     * err_pragma_namespace_cascade probe: a `pragma X Y` line whose
     * recovery declares NO fresh label (no `asm : ID` reduce in PLY
     * terms) leaves the parser in a state that silences the NEXT
     * error if it also has no-decl recovery. Cleared by any
     * mem_declare_label() during the previous statement. */
    /* Suppress only if the previous statement made no declaration AND
     * the current statement (so far) also made none. A successful
     * `asm : ID` reduce (mem_declare_label) on the prior statement means
     * PLY's error-recovery `errorcount` was cleared by valid shifts, so
     * the next error must re-fire. The intra-statement decl_since_err
     * stays as before (used to keep the in-line "first error wins"
     * behaviour); the cross-statement decision reads stmt_had_decl. */
    bool suppress = p->prev_err_no_decl && !p->decl_since_err && !p->stmt_had_decl;
    p->prev_err_no_decl = !p->stmt_had_decl;
    p->decl_since_err = false;
    if (suppress) {
        p->as->error_count++;       /* counted-but-silent */
        return;
    }
    if (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_EOF) {
        asm_error(p->as, p->cur.lineno,
                  "Syntax error. Unexpected end of line [NEWLINE]");
    } else {
        char nbuf[32], vbuf[32];
        const char *val = tok_display_value(&p->cur, vbuf, sizeof vbuf);
        const char *name = tok_ply_name(p->cur.type, p->cur.sval, nbuf, sizeof nbuf);
        asm_error(p->as, p->cur.lineno,
                  "Syntax error. Unexpected token '%s' [%s]", val, name);
    }
}

/* ----------------------------------------------------------------
 * Expression parsing (operator precedence)
 * Matches Python precedence from asmparse.py
 * ---------------------------------------------------------------- */
static Expr *parse_expr(Parser *p);
static Expr *parse_pexpr(Parser *p);

/* Check if current token can start an expression */
static bool is_expr_start(TokenType t)
{
    return t == TOK_INTEGER || t == TOK_ID || t == TOK_ADDR ||
           t == TOK_LP || t == TOK_LB || t == TOK_PLUS || t == TOK_MINUS;
}

/* Primary expression: integer, label, $, (expr), [expr] */
static Expr *parse_primary(Parser *p)
{
    int lineno = p->cur.lineno;

    if (p->cur.type == TOK_INTEGER) {
        int64_t val = p->cur.ival;
        parser_advance(p);
        return expr_int(p->as, val, lineno);
    }

    if (p->cur.type == TOK_ID) {
        char *name = p->cur.sval;
        parser_advance(p);
        Label *lbl = mem_get_label(p->as, name, lineno);
        return expr_label(p->as, lbl, lineno);
    }

    if (p->cur.type == TOK_ADDR) {
        /* $ = current address */
        parser_advance(p);
        return expr_int(p->as, p->as->mem.index, lineno);
    }

    if (p->cur.type == TOK_LP) {
        parser_advance(p);
        Expr *e = parse_expr(p);
        if (p->cur.type == TOK_RP)
            parser_advance(p);
        return e;
    }

    if (p->cur.type == TOK_LB) {
        parser_advance(p);
        Expr *e = parse_expr(p);
        if (p->cur.type == TOK_RB)
            parser_advance(p);
        return e;
    }

    /* No expression-starting token: PLY's p_error reports the offending
     * token (or "Unexpected end of line" at a NEWLINE) — e.g. `and :`
     * (CO) or a missing operand before NEWLINE. */
    asm_unexpected(p);
    return expr_int(p->as, 0, lineno);
}

/* Unary: +expr, -expr */
static Expr *parse_unary(Parser *p)
{
    int lineno = p->cur.lineno;

    if (p->cur.type == TOK_MINUS) {
        parser_advance(p);
        Expr *operand = parse_unary(p);
        return expr_unary(p->as, '-', operand, lineno);
    }
    if (p->cur.type == TOK_PLUS) {
        parser_advance(p);
        Expr *operand = parse_unary(p);
        return expr_unary(p->as, '+', operand, lineno);
    }
    return parse_primary(p);
}

/* Power: expr ^ expr (right-associative) */
static Expr *parse_power(Parser *p)
{
    Expr *left = parse_unary(p);
    while (p->cur.type == TOK_POW) {
        int lineno = p->cur.lineno;
        parser_advance(p);
        Expr *right = parse_unary(p);
        left = expr_binary(p->as, '^', left, right, lineno);
    }
    return left;
}

/* Mul/Div/Mod: expr * expr, expr / expr, expr % expr */
static Expr *parse_muldiv(Parser *p)
{
    Expr *left = parse_power(p);
    while (p->cur.type == TOK_MUL || p->cur.type == TOK_DIV || p->cur.type == TOK_MOD) {
        int lineno = p->cur.lineno;
        int op = (p->cur.type == TOK_MUL) ? '*' :
                 (p->cur.type == TOK_DIV) ? '/' : '%';
        parser_advance(p);
        Expr *right = parse_power(p);
        left = expr_binary(p->as, op, left, right, lineno);
    }
    return left;
}

/* Add/Sub: expr + expr, expr - expr */
static Expr *parse_addsub(Parser *p)
{
    Expr *left = parse_muldiv(p);
    while (p->cur.type == TOK_PLUS || p->cur.type == TOK_MINUS) {
        int lineno = p->cur.lineno;
        int op = (p->cur.type == TOK_PLUS) ? '+' : '-';
        parser_advance(p);
        Expr *right = parse_muldiv(p);
        left = expr_binary(p->as, op, left, right, lineno);
    }
    return left;
}

/* Shifts and bitwise: <<, >>, &, |, ~ (all left-associative, same precedence in Python) */
static Expr *parse_bitwise(Parser *p)
{
    Expr *left = parse_addsub(p);
    while (p->cur.type == TOK_LSHIFT || p->cur.type == TOK_RSHIFT ||
           p->cur.type == TOK_BAND || p->cur.type == TOK_BOR ||
           p->cur.type == TOK_BXOR) {
        int lineno = p->cur.lineno;
        int op;
        switch (p->cur.type) {
        case TOK_LSHIFT: op = EXPR_OP_LSHIFT; break;
        case TOK_RSHIFT: op = EXPR_OP_RSHIFT; break;
        case TOK_BAND: op = '&'; break;
        case TOK_BOR: op = '|'; break;
        case TOK_BXOR: op = '~'; break;
        default: op = '?'; break;
        }
        parser_advance(p);
        Expr *right = parse_addsub(p);
        left = expr_binary(p->as, op, left, right, lineno);
    }
    return left;
}

static Expr *parse_expr(Parser *p)
{
    return parse_bitwise(p);
}

/* Parse parenthesized expression: (expr) */
static Expr *parse_pexpr(Parser *p)
{
    if (p->cur.type == TOK_LP) {
        parser_advance(p);
        Expr *e = parse_expr(p);
        parser_expect(p, TOK_RP);
        return e;
    }
    return parse_expr(p);
}

/* Parse an expression that might be parenthesized.
 * This unified function handles both expr and pexpr contexts
 * used heavily in the grammar. */
static Expr *parse_any_expr(Parser *p)
{
    return parse_expr(p);
}

/* ----------------------------------------------------------------
 * Instruction creation helpers
 * ---------------------------------------------------------------- */
static AsmInstr *make_instr(Parser *p, int lineno, const char *mnemonic)
{
    AsmInstr *instr = arena_calloc(&p->as->arena, 1, sizeof(AsmInstr));
    instr->lineno = lineno;
    instr->type = ASM_NORMAL;

    const Z80Opcode *op = z80_find_opcode(mnemonic);
    if (!op) {
        asm_error(p->as, lineno, "Invalid mnemonic '%s'", mnemonic);
        return NULL;
    }
    instr->asm_name = op->asm_name;
    instr->opcode = op;
    instr->arg_count = count_arg_slots(mnemonic, instr->arg_bytes, ASM_MAX_ARGS);
    instr->pending = false;
    return instr;
}

static AsmInstr *make_instr_expr(Parser *p, int lineno, const char *mnemonic, Expr *arg)
{
    AsmInstr *instr = make_instr(p, lineno, mnemonic);
    if (!instr) return NULL;

    if (arg && instr->arg_count > 0) {
        instr->args[0] = arg;
        /* Check if pending */
        int64_t val;
        if (expr_try_eval(p->as, arg, &val)) {
            instr->resolved_args[0] = val;
            instr->pending = false;
        } else {
            instr->pending = true;
        }
    }
    return instr;
}

static AsmInstr *make_instr_2expr(Parser *p, int lineno, const char *mnemonic,
                                   Expr *arg1, Expr *arg2)
{
    AsmInstr *instr = make_instr(p, lineno, mnemonic);
    if (!instr) return NULL;

    instr->args[0] = arg1;
    instr->args[1] = arg2;
    instr->arg_count = 2;

    /* Check if pending */
    int64_t val;
    bool pending = false;
    if (arg1) {
        if (expr_try_eval(p->as, arg1, &val))
            instr->resolved_args[0] = val;
        else
            pending = true;
    }
    if (arg2) {
        if (expr_try_eval(p->as, arg2, &val))
            instr->resolved_args[1] = val;
        else
            pending = true;
    }
    instr->pending = pending;
    return instr;
}

/* Create DEFB instruction */
static AsmInstr *make_defb(Parser *p, int lineno, Expr **exprs, int count)
{
    AsmInstr *instr = arena_calloc(&p->as->arena, 1, sizeof(AsmInstr));
    instr->lineno = lineno;
    instr->type = ASM_DEFB;
    instr->asm_name = "DEFB";
    instr->data_exprs = arena_alloc(&p->as->arena, sizeof(Expr *) * (size_t)count);
    memcpy(instr->data_exprs, exprs, sizeof(Expr *) * (size_t)count);
    instr->data_count = count;

    /* Check if any are pending */
    bool pending = false;
    for (int i = 0; i < count; i++) {
        int64_t val;
        if (!expr_try_eval(p->as, exprs[i], &val))
            pending = true;
    }
    instr->pending = pending;
    return instr;
}

/* Create DEFB from raw bytes (INCBIN) */
static AsmInstr *make_defb_raw(Parser *p, int lineno, uint8_t *data, int count)
{
    AsmInstr *instr = arena_calloc(&p->as->arena, 1, sizeof(AsmInstr));
    instr->lineno = lineno;
    instr->type = ASM_DEFB;
    instr->asm_name = "DEFB";
    instr->raw_bytes = arena_alloc(&p->as->arena, (size_t)count);
    memcpy(instr->raw_bytes, data, (size_t)count);
    instr->raw_count = count;
    instr->data_count = count;
    instr->pending = false;
    return instr;
}

/* Create DEFW instruction */
static AsmInstr *make_defw(Parser *p, int lineno, Expr **exprs, int count)
{
    AsmInstr *instr = arena_calloc(&p->as->arena, 1, sizeof(AsmInstr));
    instr->lineno = lineno;
    instr->type = ASM_DEFW;
    instr->asm_name = "DEFW";
    instr->data_exprs = arena_alloc(&p->as->arena, sizeof(Expr *) * (size_t)count);
    memcpy(instr->data_exprs, exprs, sizeof(Expr *) * (size_t)count);
    instr->data_count = count;

    bool pending = false;
    for (int i = 0; i < count; i++) {
        int64_t val;
        if (!expr_try_eval(p->as, exprs[i], &val))
            pending = true;
    }
    instr->pending = pending;
    return instr;
}

/* Create DEFS instruction */
static AsmInstr *make_defs(Parser *p, int lineno, Expr *count_expr, Expr *fill_expr)
{
    AsmInstr *instr = arena_calloc(&p->as->arena, 1, sizeof(AsmInstr));
    instr->lineno = lineno;
    instr->type = ASM_DEFS;
    instr->asm_name = "DEFS";
    instr->defs_count = count_expr;
    instr->defs_fill = fill_expr;

    int64_t val;
    instr->pending = !expr_try_eval(p->as, count_expr, &val);
    if (fill_expr && !expr_try_eval(p->as, fill_expr, &val))
        instr->pending = true;
    return instr;
}

/* ----------------------------------------------------------------
 * Mnemonic string builders
 * ---------------------------------------------------------------- */
static char *mnemonic_buf(Parser *p, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return arena_strdup(&p->as->arena, buf);
}

/* ----------------------------------------------------------------
 * Lookahead: is this '(' starting a memory-indirect address, or
 * just grouping parens in a larger expression?
 *
 * Memory indirect: LD HL,(expr)   — ')' followed by end-of-operand
 * Grouping:        LD HL,(expr)+1 — ')' followed by operator
 *
 * Scans ahead without consuming tokens. Returns true if indirect.
 * ---------------------------------------------------------------- */
static bool is_indirect_paren(Parser *p)
{
    if (p->cur.type != TOK_LP && p->cur.type != TOK_LB) return false;

    /* Save lexer state */
    Lexer saved_lex = p->lex;
    Token saved_cur = p->cur;
    bool saved_has_peek = p->has_peek;
    Token saved_peek = p->peek_tok;

    /* Skip past matching paren */
    TokenType open = p->cur.type;
    TokenType close = (open == TOK_LP) ? TOK_RP : TOK_RB;
    int depth = 1;
    parser_advance(p); /* consume ( */
    while (p->cur.type != TOK_EOF && depth > 0) {
        if (p->cur.type == open) depth++;
        else if (p->cur.type == close) depth--;
        if (depth > 0) parser_advance(p);
    }
    if (depth == 0) parser_advance(p); /* move past ) */

    /* Check what follows — operator means grouping, not indirect */
    bool indirect = (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_EOF ||
                     p->cur.type == TOK_COLON || p->cur.type == TOK_COMMA);

    /* Restore state */
    p->lex = saved_lex;
    p->cur = saved_cur;
    p->has_peek = saved_has_peek;
    p->peek_tok = saved_peek;

    return indirect;
}

/* ----------------------------------------------------------------
 * Parse (IX+N) / (IY+N) indexed addressing
 * Returns the register name and the offset expression
 * ---------------------------------------------------------------- */
static bool parse_idx_addr(Parser *p, const char **reg, Expr **offset, bool bracket)
{
    /* Already consumed ( or [ */
    TokenType regtype = p->cur.type;
    if (regtype != TOK_IX && regtype != TOK_IY) return false;
    *reg = reg_name(regtype);
    int op_lineno = p->cur.lineno;   /* IX/IY token's line == Python p.lineno(2) */
    parser_advance(p);

    /* Next should be +/- followed by expression, or closing paren for +0 */
    TokenType close = bracket ? TOK_RB : TOK_RP;
    if (p->cur.type == close) {
        /* (IX) or [IX] → offset 0 */
        *offset = expr_int(p->as, 0, p->cur.lineno);
    } else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
        /* `LP IX LP ...` shape — e.g. `ld (ix (- 12 + 5)), 0`. Python's
         * grammar (src/zxbasm/asmparse.py:291-303) has no `expr : LP expr RP`
         * production — expr's FIRST is INTEGER / ID / ADDR / MINUS / PLUS,
         * but NOT LP. So PLY enters p_error at the inner `(`, error-recovers
         * across the parenthesised sub-expression, and surfaces the failure
         * at the OUTER `)` with `Syntax error. Unexpected token ')' [RP]`.
         *
         * Mimic that here: parse-and-discard the inner LP-RP expression so
         * the stream is positioned at the outer RP, then emit the PLY-shape
         * unexpected-token diagnostic at that RP. */
        (void)parse_primary(p);  /* consumes inner `( expr )` */
        if (p->cur.type == close) {
            asm_unexpected(p);   /* `Unexpected token ')' [RP]` at outer RP */
            parser_advance(p);
        }
        *offset = expr_int(p->as, 0, op_lineno);
        return true;
    } else {
        /* Python p_ind8_I (src/zxbasm/asmparse.py:291-321): the bare
         * `LP IX expr RP` form requires the offset to lead with a unary
         * '+'/'-'; a value/identifier with no sign is rejected by the
         * explicit semantic check at asmparse.py:319-320. (Other non-sign
         * leads — '(', '*', … — fail in PLY's grammar with a generic
         * p_error; the C's '[%d]' formatter can't reproduce that token-
         * name text, so those stay out of this check's scope.)
         *
         * Record the error but still consume the operand as the grammar
         * production does — `LP IX expr RP` reduces as a unit, so PLY
         * emits exactly one error and parsing continues from after the
         * ')'. Returning early here would instead leave the rest of the
         * operand in the stream and trigger a cascade. */
        if (p->cur.type == TOK_INTEGER || p->cur.type == TOK_ID) {
            char tokbuf[64];
            const char *toktext;
            if (p->cur.type == TOK_ID) {
                toktext = p->cur.sval ? p->cur.sval : "?";
            } else {
                snprintf(tokbuf, sizeof(tokbuf), "%lld",
                         (long long)p->cur.ival);
                toktext = tokbuf;
            }
            asm_error(p->as, op_lineno,
                      "Unexpected token '%s'. Expected '+' or '-'",
                      toktext);
        }
        /* Parse the full offset expression: handles IX+N, IX-N, IX+A-B etc. */
        *offset = parse_any_expr(p);
    }

    /* Expect closing paren/bracket */
    parser_expect(p, close);
    return true;
}

/* ----------------------------------------------------------------
 * Parse a single instruction
 * ---------------------------------------------------------------- */
static void parse_asm(Parser *p)
{
    Token t = p->cur;
    int lineno = t.lineno;
    AsmInstr *instr = NULL;

    /* Empty line or just a label */
    if (t.type == TOK_NEWLINE || t.type == TOK_EOF || t.type == TOK_COLON) {
        return;
    }

    /* Label declaration: ID or INTEGER at start of statement */
    if (t.type == TOK_ID || t.type == TOK_INTEGER) {
        /* Check if followed by EQU or : or is a label on its own line */
        Token next = parser_peek(p);

        if (next.type == TOK_EQU) {
            /* ID EQU expr */
            char *name = t.type == TOK_ID ? t.sval : arena_strdup(&p->as->arena, t.sval);
            if (t.type == TOK_INTEGER) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)t.ival);
                name = arena_strdup(&p->as->arena, buf);
            }
            parser_advance(p); /* consume ID */
            parser_advance(p); /* consume EQU */
            Expr *val = parse_any_expr(p);
            mem_declare_label(p->as, name, lineno, val, false);
            p->decl_since_err = true; p->stmt_had_decl = true;
            return;
        }

        /* PLY ID-then-ID: `asm : ID` cannot reduce (FOLLOW(asm) does not
         * include ID) and no shift action exists for ID after ID; p_error
         * fires on the SECOND token. Mimic that: advance past the first
         * ID (PLY's shift before failing the reduce), then route the
         * unexpected report onto the second. This also keeps the first
         * ID out of the label table so later identical-shape lines do
         * not trip a phantom "already defined" cascade. */
        if (t.type == TOK_ID && next.type == TOK_ID) {
            parser_advance(p);          /* shift the first ID */
            asm_unexpected(p);          /* p_error on the second */
            parser_advance(p);          /* discard the second ID */
            /* PLY default error recovery on the asm grammar: after
             * shifting the error symbol, PLY tries to reduce sub-rules
             * in the now-recovered state. If the remaining tokens form
             * `ID NEWLINE`, that ID reduces as `asm : ID` (label decl)
             * with NEWLINE in FOLLOW(asm). The asmparse trace on the
             * err_pragma_namespace_cascade probe confirms this: `foo`
             * (the third ID of `#pragma push_namespace foo`) is
             * declared as a label during recovery. Mirror that here so
             * the subsequent line's error suppression model
             * (asm_unexpected / decl_since_err) matches PLY. */
            if (p->cur.type == TOK_ID) {
                Token id = p->cur;
                Token peek2 = parser_peek(p);
                if (peek2.type == TOK_NEWLINE || peek2.type == TOK_EOF) {
                    parser_advance(p);  /* shift the trailing ID */
                    mem_declare_label(p->as, id.sval, id.lineno, NULL, false);
                    p->decl_since_err = true; p->stmt_had_decl = true;
                }
            }
            /* Leave cur at NEWLINE / further tokens; parse_program's
             * error-resync (error_count > err0 branch) will skip_to
             * newline + advance, which is fine when only the line
             * NEWLINE remains. Critically, do NOT advance past the
             * newline here ourselves — if we do, parse_program's
             * recovery will then skip the NEXT line's tokens (the
             * lexer's "illegal character '#'" advances the lex
             * cursor before our advance grabs it, so the post-
             * newline cur is already inside the next line). */
            return;
        }

        if (next.type == TOK_COLON || next.type == TOK_NEWLINE ||
            next.type == TOK_EOF ||
            /* Label followed by an instruction.
             *
             * PLY equivalent: `asm : ID` reduces only when lookahead is in
             * FOLLOW(asm) = {NEWLINE, CO, $end}. The "label + opcode on
             * same line" arm is the COLON-less spelling that PLY accepts
             * via `asms : asms CO asm` only when CO is shifted — but the
             * recursive-descent port permits it without the CO if the
             * follower is a recognised opcode/pseudo token. ID-ID is
             * handled above. */
            (t.type == TOK_ID &&
             next.type != TOK_COMMA && next.type != TOK_LP &&
             next.type != TOK_LB && next.type != TOK_PLUS &&
             next.type != TOK_MINUS)) {
            /* Could be a label declaration */
            /* In Python: p_asm_label handles ID and INTEGER as labels */
            char *name;
            if (t.type == TOK_INTEGER) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)t.ival);
                name = arena_strdup(&p->as->arena, buf);
            } else {
                name = t.sval;
            }

            /* Only treat as label if not a keyword/instruction/register */
            if (t.type == TOK_ID || t.type == TOK_INTEGER) {
                parser_advance(p);
                mem_declare_label(p->as, name, lineno, NULL, false);
                p->decl_since_err = true; p->stmt_had_decl = true;
                /* Optionally consume colon */
                if (p->cur.type == TOK_COLON)
                    parser_advance(p);
                /* If more tokens on this line, continue parsing (e.g. TEST: LD A,5) */
                if (p->cur.type != TOK_NEWLINE && p->cur.type != TOK_EOF &&
                    p->cur.type != TOK_COLON) {
                    t = p->cur;
                    lineno = t.lineno;
                    /* Fall through to parse the instruction after the label */
                } else {
                    return;
                }
            }
        }
    }

    /* ---- NOP, EXX, and other single-byte instructions ---- */
    switch (t.type) {
    case TOK_NOP: case TOK_EXX: case TOK_CCF: case TOK_SCF:
    case TOK_LDIR: case TOK_LDI: case TOK_LDDR: case TOK_LDD:
    case TOK_CPIR: case TOK_CPI: case TOK_CPDR: case TOK_CPD:
    case TOK_DAA: case TOK_NEG: case TOK_CPL: case TOK_HALT:
    case TOK_EI: case TOK_DI: case TOK_OUTD: case TOK_OUTI:
    case TOK_OTDR: case TOK_OTIR: case TOK_IND: case TOK_INI:
    case TOK_INDR: case TOK_INIR: case TOK_RETI: case TOK_RETN:
    case TOK_RLA: case TOK_RLCA: case TOK_RRA: case TOK_RRCA:
    case TOK_RLD: case TOK_RRD:
        instr = make_instr(p, lineno, t.sval);
        parser_advance(p);
        if (instr) mem_add_instruction(p->as, instr);
        return;

    case TOK_RET:
        parser_advance(p);
        if (is_jp_flag(p->cur.type)) {
            const char *flag = reg_name(p->cur.type);
            parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "RET %s", flag));
        } else {
            instr = make_instr(p, lineno, "RET");
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;

    /* ZX Next simple instructions */
    case TOK_LDIX: case TOK_LDWS: case TOK_LDIRX: case TOK_LDDX:
    case TOK_LDDRX: case TOK_LDPIRX: case TOK_OUTINB:
    case TOK_SWAPNIB: case TOK_MIRROR_INSTR: case TOK_PIXELDN:
    case TOK_PIXELAD: case TOK_SETAE:
        instr = make_instr(p, lineno, t.sval);
        parser_advance(p);
        if (instr) mem_add_instruction(p->as, instr);
        return;

    default:
        break;
    }

    /* ---- LD instruction ---- */
    if (t.type == TOK_LD) {
        parser_advance(p);

        /* Destination */
        TokenType dst = p->cur.type;

        if (dst == TOK_A) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            TokenType src = p->cur.type;

            if (src == TOK_I) { parser_advance(p); instr = make_instr(p, lineno, "LD A,I"); }
            else if (src == TOK_R) { parser_advance(p); instr = make_instr(p, lineno, "LD A,R"); }
            else if (src == TOK_A) { parser_advance(p); instr = make_instr(p, lineno, "LD A,A"); }
            else if (is_reg8(src)) {
                const char *r = reg_name(src);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD A,%s", r));
            }
            else if (is_reg8i(src)) {
                const char *r = reg_name(src);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD A,%s", r));
            }
            else if ((src == TOK_LP || src == TOK_LB) && is_indirect_paren(p)) {
                bool bracket = (src == TOK_LB);
                parser_advance(p);
                if (p->cur.type == TOK_BC) {
                    parser_advance(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr(p, lineno, "LD A,(BC)");
                } else if (p->cur.type == TOK_DE) {
                    parser_advance(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr(p, lineno, "LD A,(DE)");
                } else if (p->cur.type == TOK_HL) {
                    parser_advance(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr(p, lineno, "LD A,(HL)");
                } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                    const char *reg;
                    Expr *offset;
                    parse_idx_addr(p, &reg, &offset, bracket);
                    instr = make_instr_expr(p, lineno,
                        mnemonic_buf(p, "LD A,(%s+N)", reg), offset);
                } else {
                    /* LD A,(NN) — memory indirect */
                    Expr *addr = parse_any_expr(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr_expr(p, lineno, "LD A,(NN)", addr);
                }
            }
            else {
                /* LD A,N — immediate */
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, "LD A,N", val);
            }
        }
        else if (dst == TOK_I) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            parser_expect(p, TOK_A);
            instr = make_instr(p, lineno, "LD I,A");
        }
        else if (dst == TOK_R) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            parser_expect(p, TOK_A);
            instr = make_instr(p, lineno, "LD R,A");
        }
        else if (dst == TOK_SP) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                instr = make_instr(p, lineno, "LD SP,HL");
            } else if (is_reg16i(p->cur.type)) {
                const char *r = reg_name(p->cur.type);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD SP,%s", r));
            } else if ((p->cur.type == TOK_LP || p->cur.type == TOK_LB) &&
                       is_indirect_paren(p)) {
                bool bracket = (p->cur.type == TOK_LB);
                parser_advance(p);
                Expr *addr = parse_any_expr(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr_expr(p, lineno, "LD SP,(NN)", addr);
            } else {
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, "LD SP,NN", val);
            }
        }
        else if (is_reg8(dst) || dst == TOK_B || dst == TOK_C ||
                 dst == TOK_D || dst == TOK_E || dst == TOK_H || dst == TOK_L) {
            const char *r = reg_name(dst);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);

            if (p->cur.type == TOK_A) {
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,A", r));
            } else if (is_reg8(p->cur.type)) {
                const char *r2 = reg_name(p->cur.type);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,%s", r, r2));
            } else if (is_reg8i(p->cur.type)) {
                const char *r2 = reg_name(p->cur.type);
                parser_advance(p);
                /* Check for invalid: H/L with IXH/IXL/IYH/IYL */
                if ((strcmp(r, "H") == 0 || strcmp(r, "L") == 0) &&
                    (strcmp(r2, "IXH") == 0 || strcmp(r2, "IXL") == 0 ||
                     strcmp(r2, "IYH") == 0 || strcmp(r2, "IYL") == 0)) {
                    asm_error(p->as, lineno, "Unexpected token '%s'", r2);
                    return;
                }
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,%s", r, r2));
            } else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
                bool bracket = (p->cur.type == TOK_LB);
                parser_advance(p);
                if (p->cur.type == TOK_HL) {
                    parser_advance(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,(HL)", r));
                } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                    const char *ireg;
                    Expr *offset;
                    parse_idx_addr(p, &ireg, &offset, bracket);
                    instr = make_instr_expr(p, lineno,
                        mnemonic_buf(p, "LD %s,(%s+N)", r, ireg), offset);
                } else {
                    /* `ld r, [<x>]` where <x> is neither HL nor IX/IY:
                     * PLY reports the offending token (e.g. `.c` [ID]). */
                    asm_unexpected(p);
                    parser_skip_to_newline(p);
                    return;
                }
            } else {
                /* LD r,N — immediate */
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, mnemonic_buf(p, "LD %s,N", r), val);
            }
        }
        else if (is_reg8i(dst)) {
            const char *r = reg_name(dst);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_A) {
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,A", r));
            } else if (is_reg8_bcde(p->cur.type)) {
                const char *r2 = reg_name(p->cur.type);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,%s", r, r2));
            } else if (is_reg8i(p->cur.type)) {
                const char *r2 = reg_name(p->cur.type);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "LD %s,%s", r, r2));
            } else {
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, mnemonic_buf(p, "LD %s,N", r), val);
            }
        }
        else if (is_reg16(dst)) {
            const char *r = reg_name(dst);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);

            if ((p->cur.type == TOK_LP || p->cur.type == TOK_LB) &&
                is_indirect_paren(p)) {
                bool bracket = (p->cur.type == TOK_LB);
                parser_advance(p);
                Expr *addr = parse_any_expr(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr_expr(p, lineno, mnemonic_buf(p, "LD %s,(NN)", r), addr);
            } else {
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, mnemonic_buf(p, "LD %s,NN", r), val);
            }
        }
        else if (dst == TOK_LP || dst == TOK_LB) {
            /* LD (something), something */
            bool bracket = (dst == TOK_LB);
            parser_advance(p);

            if (p->cur.type == TOK_BC) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                parser_expect(p, TOK_A);
                instr = make_instr(p, lineno, "LD (BC),A");
            } else if (p->cur.type == TOK_DE) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                parser_expect(p, TOK_A);
                instr = make_instr(p, lineno, "LD (DE),A");
            } else if (p->cur.type == TOK_HL) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                /* LD (HL), reg/imm */
                if (p->cur.type == TOK_A) {
                    parser_advance(p);
                    instr = make_instr(p, lineno, "LD (HL),A");
                } else if (is_reg8(p->cur.type)) {
                    const char *r2 = reg_name(p->cur.type);
                    parser_advance(p);
                    instr = make_instr(p, lineno, mnemonic_buf(p, "LD (HL),%s", r2));
                } else {
                    Expr *val = parse_any_expr(p);
                    instr = make_instr_expr(p, lineno, "LD (HL),N", val);
                }
            } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                const char *ireg;
                Expr *offset;
                parse_idx_addr(p, &ireg, &offset, bracket);
                parser_expect(p, TOK_COMMA);
                /* LD (IX+N), reg/imm */
                if (p->cur.type == TOK_A) {
                    parser_advance(p);
                    instr = make_instr_expr(p, lineno,
                        mnemonic_buf(p, "LD (%s+N),A", ireg), offset);
                } else if (is_reg8(p->cur.type)) {
                    const char *r2 = reg_name(p->cur.type);
                    parser_advance(p);
                    instr = make_instr_expr(p, lineno,
                        mnemonic_buf(p, "LD (%s+N),%s", ireg, r2), offset);
                } else {
                    Expr *val = parse_any_expr(p);
                    instr = make_instr_2expr(p, lineno,
                        mnemonic_buf(p, "LD (%s+N),N", ireg), offset, val);
                }
            } else if (p->cur.type == TOK_SP) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                /* EX (SP), reg */
                /* Actually this shouldn't be LD — probably wrong path */
                asm_error(p->as, lineno, "Syntax error");
                parser_skip_to_newline(p);
                return;
            } else {
                /* LD (NN), A/reg16/SP */
                Expr *addr = parse_any_expr(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                if (p->cur.type == TOK_A) {
                    parser_advance(p);
                    instr = make_instr_expr(p, lineno, "LD (NN),A", addr);
                } else if (p->cur.type == TOK_SP) {
                    parser_advance(p);
                    instr = make_instr_expr(p, lineno, "LD (NN),SP", addr);
                } else if (is_reg16(p->cur.type)) {
                    const char *r2 = reg_name(p->cur.type);
                    parser_advance(p);
                    instr = make_instr_expr(p, lineno,
                        mnemonic_buf(p, "LD (NN),%s", r2), addr);
                } else {
                    asm_error(p->as, lineno, "Syntax error");
                    parser_skip_to_newline(p);
                    return;
                }
            }
        }
        else {
            asm_error(p->as, lineno, "Syntax error. Unexpected token '%s'",
                      p->cur.sval ? p->cur.sval : "?");
            parser_skip_to_newline(p);
            return;
        }

        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- PUSH / POP ---- */
    if (t.type == TOK_PUSH || t.type == TOK_POP) {
        const char *op = t.sval;
        parser_advance(p);

        /* PUSH/POP NAMESPACE */
        if (p->cur.type == TOK_NAMESPACE) {
            parser_advance(p);
            Memory *m = &p->as->mem;
            if (t.type == TOK_PUSH) {
                vec_push(m->namespace_stack, m->namespace_);
                if (p->cur.type == TOK_ID || p->cur.type == TOK_INTEGER) {
                    m->namespace_ = normalize_namespace(p->as, p->cur.sval ? p->cur.sval : ".");
                    parser_advance(p);
                }
            } else {
                /* POP NAMESPACE */
                if (m->namespace_stack.len == 0) {
                    asm_error(p->as, lineno,
                        "Stack underflow. No more Namespaces to pop. Current namespace is %s",
                        m->namespace_);
                } else {
                    m->namespace_ = vec_pop(m->namespace_stack);
                }
            }
            return;
        }

        if (p->cur.type == TOK_AF) {
            parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s AF", op));
        } else if (is_reg16(p->cur.type)) {
            const char *r = reg_name(p->cur.type);
            parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s %s", op, r));
        } else if (t.type == TOK_PUSH && p->as->zxnext) {
            /* ZX Next: PUSH NN (immediate) */
            Expr *val = parse_any_expr(p);
            /* Byte swap for PUSH NN: (val & 0xFF) << 8 | (val >> 8) & 0xFF */
            Expr *ff = expr_int(p->as, 0xFF, lineno);
            Expr *n8 = expr_int(p->as, 8, lineno);
            Expr *swapped = expr_binary(p->as, '|',
                expr_binary(p->as, EXPR_OP_LSHIFT,
                    expr_binary(p->as, '&', val, ff, lineno),
                    n8, lineno),
                expr_binary(p->as, '&',
                    expr_binary(p->as, EXPR_OP_RSHIFT, val, n8, lineno),
                    ff, lineno),
                lineno);
            instr = make_instr_expr(p, lineno, "PUSH NN", swapped);
        } else {
            asm_error(p->as, lineno, "Syntax error");
            parser_skip_to_newline(p);
            return;
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- INC / DEC ---- */
    if (t.type == TOK_INC || t.type == TOK_DEC) {
        const char *op = t.sval;
        parser_advance(p);

        if (p->cur.type == TOK_A || is_reg8(p->cur.type) || is_reg16(p->cur.type) ||
            p->cur.type == TOK_SP || is_reg8i(p->cur.type)) {
            const char *r = reg_name(p->cur.type);
            parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s %s", op, r));
        } else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s (HL)", op));
            } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                const char *ireg;
                Expr *offset;
                parse_idx_addr(p, &ireg, &offset, bracket);
                instr = make_instr_expr(p, lineno,
                    mnemonic_buf(p, "%s (%s+N)", op, ireg), offset);
            } else {
                asm_error(p->as, lineno, "Syntax error");
                parser_skip_to_newline(p);
                return;
            }
        } else {
            asm_error(p->as, lineno, "Syntax error");
            parser_skip_to_newline(p);
            return;
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- ADD / ADC / SBC ---- */
    if (t.type == TOK_ADD || t.type == TOK_ADC || t.type == TOK_SBC) {
        const char *op = t.sval;
        parser_advance(p);

        if (p->cur.type == TOK_A) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_A) { parser_advance(p); instr = make_instr(p, lineno, mnemonic_buf(p, "%s A,A", op)); }
            else if (is_reg8(p->cur.type)) {
                const char *r = reg_name(p->cur.type); parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s A,%s", op, r));
            }
            else if (is_reg8i(p->cur.type)) {
                const char *r = reg_name(p->cur.type); parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s A,%s", op, r));
            }
            else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
                bool bracket = (p->cur.type == TOK_LB);
                parser_advance(p);
                if (p->cur.type == TOK_HL) {
                    parser_advance(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr(p, lineno, mnemonic_buf(p, "%s A,(HL)", op));
                } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                    const char *ireg; Expr *offset;
                    parse_idx_addr(p, &ireg, &offset, bracket);
                    instr = make_instr_expr(p, lineno,
                        mnemonic_buf(p, "%s A,(%s+N)", op, ireg), offset);
                } else {
                    asm_error(p->as, lineno, "Syntax error");
                    parser_skip_to_newline(p); return;
                }
            } else {
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, mnemonic_buf(p, "%s A,N", op), val);
            }
        }
        else if (p->cur.type == TOK_HL) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_BC || p->cur.type == TOK_DE ||
                p->cur.type == TOK_HL || p->cur.type == TOK_SP) {
                const char *r = reg_name(p->cur.type); parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s HL,%s", op, r));
            } else if (p->cur.type == TOK_A && p->as->zxnext) {
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "ADD HL,A"));
            } else if (p->as->zxnext) {
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, "ADD HL,NN", val);
            } else {
                /* Without --zxnext, `ADD HL,<non-reg16>` is a PLY-level
                 * syntax error at the offending token (e.g. `ADD HL,A`
                 * -> `'A' [A]`; `ADD HL,0201h` -> `'513' [INTEGER]`).
                 * Use asm_unexpected so the report carries the PLY-shape
                 * token render rather than a bare "Syntax error". */
                asm_unexpected(p);
                parser_skip_to_newline(p); return;
            }
        }
        else if (is_reg16i(p->cur.type)) {
            const char *r = reg_name(p->cur.type);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_BC || p->cur.type == TOK_DE ||
                p->cur.type == TOK_HL || p->cur.type == TOK_SP ||
                is_reg16i(p->cur.type)) {
                const char *r2 = reg_name(p->cur.type); parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s %s,%s", op, r, r2));
            } else {
                asm_error(p->as, lineno, "Syntax error");
                parser_skip_to_newline(p); return;
            }
        }
        else if ((p->cur.type == TOK_DE || p->cur.type == TOK_BC) &&
                 t.type == TOK_ADD && p->as->zxnext) {
            const char *r = reg_name(p->cur.type);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_A) {
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "ADD %s,A", r));
            } else {
                Expr *val = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, mnemonic_buf(p, "ADD %s,NN", r), val);
            }
        }
        else {
            asm_error(p->as, lineno, "Syntax error");
            parser_skip_to_newline(p); return;
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- AND, OR, XOR, SUB, CP (bitwise/arithmetic) ---- */
    if (t.type == TOK_AND || t.type == TOK_OR || t.type == TOK_XOR ||
        t.type == TOK_SUB || t.type == TOK_CP) {
        const char *op = t.sval;
        parser_advance(p);

        if (p->cur.type == TOK_A || is_reg8(p->cur.type)) {
            const char *r = reg_name(p->cur.type); parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s %s", op, r));
        }
        else if (is_reg8i(p->cur.type)) {
            const char *r = reg_name(p->cur.type); parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s %s", op, r));
        }
        else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s (HL)", op));
            } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                const char *ireg; Expr *offset;
                parse_idx_addr(p, &ireg, &offset, bracket);
                instr = make_instr_expr(p, lineno,
                    mnemonic_buf(p, "%s (%s+N)", op, ireg), offset);
            } else {
                asm_error(p->as, lineno, "Syntax error");
                parser_skip_to_newline(p); return;
            }
        }
        else {
            Expr *val = parse_any_expr(p);
            instr = make_instr_expr(p, lineno, mnemonic_buf(p, "%s N", op), val);
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- JP, JR, CALL, DJNZ ---- */
    if (t.type == TOK_JP) {
        parser_advance(p);
        /* JP (HL) */
        if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, "JP (HL)");
            } else if (is_reg16i(p->cur.type)) {
                const char *r = reg_name(p->cur.type);
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, mnemonic_buf(p, "JP (%s)", r));
            } else if (p->cur.type == TOK_C && p->as->zxnext) {
                /* JP (C) — ZX Next */
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, "JP (C)");
            } else {
                asm_error(p->as, lineno, "Syntax error");
                parser_skip_to_newline(p); return;
            }
        } else if (is_jp_flag(p->cur.type)) {
            const char *flag = reg_name(p->cur.type);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            Expr *addr = parse_any_expr(p);
            instr = make_instr_expr(p, lineno,
                mnemonic_buf(p, "JP %s,NN", flag), addr);
        } else {
            Expr *addr = parse_any_expr(p);
            instr = make_instr_expr(p, lineno, "JP NN", addr);
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    if (t.type == TOK_JR) {
        parser_advance(p);
        if (is_jr_flag(p->cur.type)) {
            const char *flag = reg_name(p->cur.type);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            Expr *addr = parse_any_expr(p);
            /* Make relative: addr - (org + 2) */
            Expr *rel = expr_binary(p->as, '-', addr,
                expr_int(p->as, p->as->mem.index + 2, lineno), lineno);
            instr = make_instr_expr(p, lineno,
                mnemonic_buf(p, "JR %s,N", flag), rel);
        } else {
            Expr *addr = parse_any_expr(p);
            Expr *rel = expr_binary(p->as, '-', addr,
                expr_int(p->as, p->as->mem.index + 2, lineno), lineno);
            instr = make_instr_expr(p, lineno, "JR N", rel);
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    if (t.type == TOK_CALL) {
        parser_advance(p);
        if (is_jp_flag(p->cur.type)) {
            const char *flag = reg_name(p->cur.type);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            Expr *addr = parse_any_expr(p);
            instr = make_instr_expr(p, lineno,
                mnemonic_buf(p, "CALL %s,NN", flag), addr);
        } else {
            Expr *addr = parse_any_expr(p);
            instr = make_instr_expr(p, lineno, "CALL NN", addr);
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    if (t.type == TOK_DJNZ) {
        parser_advance(p);
        Expr *addr = parse_any_expr(p);
        Expr *rel = expr_binary(p->as, '-', addr,
            expr_int(p->as, p->as->mem.index + 2, lineno), lineno);
        instr = make_instr_expr(p, lineno, "DJNZ N", rel);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- RST ---- */
    if (t.type == TOK_RST) {
        parser_advance(p);
        Expr *val_expr = parse_any_expr(p);
        int64_t val;
        if (!expr_eval(p->as, val_expr, &val, false)) return;
        if (val != 0 && val != 8 && val != 16 && val != 24 &&
            val != 32 && val != 40 && val != 48 && val != 56) {
            asm_error(p->as, lineno, "Invalid RST number %d", (int)val);
            return;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "RST %XH", (unsigned)val);
        instr = make_instr(p, lineno, buf);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- IM ---- */
    if (t.type == TOK_IM) {
        parser_advance(p);
        Expr *val_expr = parse_any_expr(p);
        int64_t val;
        if (!expr_eval(p->as, val_expr, &val, false)) return;
        if (val != 0 && val != 1 && val != 2) {
            asm_error(p->as, lineno, "Invalid IM number %d", (int)val);
            return;
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "IM %d", (int)val);
        instr = make_instr(p, lineno, buf);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- IN ---- */
    if (t.type == TOK_IN) {
        parser_advance(p);
        TokenType r = p->cur.type;
        if (r == TOK_A || is_reg8(r)) {
            const char *rn = reg_name(r);
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
                bool bracket = (p->cur.type == TOK_LB);
                parser_advance(p);
                if (p->cur.type == TOK_C) {
                    parser_advance(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr(p, lineno, mnemonic_buf(p, "IN %s,(C)", rn));
                } else {
                    Expr *port = parse_any_expr(p);
                    parser_expect(p, bracket ? TOK_RB : TOK_RP);
                    instr = make_instr_expr(p, lineno, "IN A,(N)", port);
                }
            } else {
                Expr *port = parse_any_expr(p);
                instr = make_instr_expr(p, lineno, "IN A,(N)", port);
            }
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- OUT ---- */
    if (t.type == TOK_OUT) {
        parser_advance(p);
        if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            if (p->cur.type == TOK_C) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                if (p->cur.type == TOK_A || is_reg8(p->cur.type)) {
                    const char *r = reg_name(p->cur.type);
                    parser_advance(p);
                    instr = make_instr(p, lineno, mnemonic_buf(p, "OUT (C),%s", r));
                }
            } else {
                Expr *port = parse_any_expr(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                parser_expect(p, TOK_COMMA);
                parser_expect(p, TOK_A);
                instr = make_instr_expr(p, lineno, "OUT (N),A", port);
            }
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- EX ---- */
    if (t.type == TOK_EX) {
        parser_advance(p);
        if (p->cur.type == TOK_AF) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            parser_expect(p, TOK_AF);
            parser_expect(p, TOK_APO);
            instr = make_instr(p, lineno, "EX AF,AF'");
        } else if (p->cur.type == TOK_DE) {
            parser_advance(p);
            parser_expect(p, TOK_COMMA);
            parser_expect(p, TOK_HL);
            instr = make_instr(p, lineno, "EX DE,HL");
        } else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            parser_expect(p, TOK_SP);
            parser_expect(p, bracket ? TOK_RB : TOK_RP);
            parser_expect(p, TOK_COMMA);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                instr = make_instr(p, lineno, "EX (SP),HL");
            } else if (is_reg16i(p->cur.type)) {
                const char *r = reg_name(p->cur.type);
                parser_advance(p);
                instr = make_instr(p, lineno, mnemonic_buf(p, "EX (SP),%s", r));
            }
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- Rotation/shift: RL, RLC, RR, RRC, SLA, SLL, SRA, SRL ---- */
    if (t.type == TOK_RL || t.type == TOK_RLC || t.type == TOK_RR ||
        t.type == TOK_RRC || t.type == TOK_SLA || t.type == TOK_SLL ||
        t.type == TOK_SRA || t.type == TOK_SRL) {
        const char *op = t.sval;
        parser_advance(p);

        if (p->cur.type == TOK_A || is_reg8(p->cur.type)) {
            const char *r = reg_name(p->cur.type);
            parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s %s", op, r));
        } else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s (HL)", op));
            } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                const char *ireg; Expr *offset;
                parse_idx_addr(p, &ireg, &offset, bracket);
                instr = make_instr_expr(p, lineno,
                    mnemonic_buf(p, "%s (%s+N)", op, ireg), offset);
            }
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- BIT, RES, SET ---- */
    if (t.type == TOK_BIT || t.type == TOK_RES || t.type == TOK_SET) {
        const char *op = t.sval;
        parser_advance(p);

        Expr *bit_expr = parse_any_expr(p);
        int64_t bit;
        if (!expr_eval(p->as, bit_expr, &bit, false)) return;
        if (bit < 0 || bit > 7) {
            asm_error(p->as, lineno, "Invalid bit position %d. Must be in [0..7]", (int)bit);
            /* Python's bit-op action raises this semantic error but the
             * production still reduces (consuming the rest of the
             * operands), so no follow-on parse error fires. The C handler
             * left `, <reg>` unconsumed, which the statement loop then
             * mis-parsed into a spurious "Unexpected token" — consume to
             * the line end to match Python's single-error output. */
            parser_skip_to_newline(p);
            return;
        }

        parser_expect(p, TOK_COMMA);

        if (p->cur.type == TOK_A || is_reg8(p->cur.type)) {
            const char *r = reg_name(p->cur.type);
            parser_advance(p);
            instr = make_instr(p, lineno, mnemonic_buf(p, "%s %d,%s", op, (int)bit, r));
        } else if (p->cur.type == TOK_LP || p->cur.type == TOK_LB) {
            bool bracket = (p->cur.type == TOK_LB);
            parser_advance(p);
            if (p->cur.type == TOK_HL) {
                parser_advance(p);
                parser_expect(p, bracket ? TOK_RB : TOK_RP);
                instr = make_instr(p, lineno, mnemonic_buf(p, "%s %d,(HL)", op, (int)bit));
            } else if (p->cur.type == TOK_IX || p->cur.type == TOK_IY) {
                const char *ireg; Expr *offset;
                parse_idx_addr(p, &ireg, &offset, bracket);
                instr = make_instr_expr(p, lineno,
                    mnemonic_buf(p, "%s %d,(%s+N)", op, (int)bit, ireg), offset);
            }
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- Pseudo-ops ---- */
    if (t.type == TOK_ORG) {
        parser_advance(p);
        Expr *val = parse_any_expr(p);
        int64_t v;
        if (expr_eval(p->as, val, &v, false))
            mem_set_org(p->as, (int)v, lineno);
        return;
    }

    if (t.type == TOK_ALIGN) {
        parser_advance(p);
        Expr *val = parse_any_expr(p);
        int64_t align;
        if (!expr_eval(p->as, val, &align, false)) return;
        if (align < 2) {
            asm_error(p->as, lineno, "ALIGN value must be greater than 1");
            return;
        }
        int new_org = p->as->mem.index +
            (int)((align - p->as->mem.index % align) % align);
        mem_set_org(p->as, new_org, lineno);
        return;
    }

    if (t.type == TOK_DEFB) {
        parser_advance(p);
        /* Parse expression list (strings expand to byte sequences) */
        VEC(Expr *) exprs;
        vec_init(exprs);

        for (;;) {
            if (p->cur.type == TOK_STRING) {
                /* String: each char -> one DEFB expression */
                const char *s = p->cur.sval;
                parser_advance(p);
                for (int i = 0; s[i]; i++) {
                    vec_push(exprs, expr_int(p->as, (unsigned char)s[i], lineno));
                }
            } else {
                Expr *e = parse_any_expr(p);
                vec_push(exprs, e);
            }
            if (p->cur.type != TOK_COMMA) break;
            parser_advance(p); /* consume comma */
        }

        instr = make_defb(p, lineno, exprs.data, exprs.len);
        vec_free(exprs);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    if (t.type == TOK_DEFW) {
        parser_advance(p);
        VEC(Expr *) exprs;
        vec_init(exprs);

        for (;;) {
            Expr *e = parse_any_expr(p);
            vec_push(exprs, e);
            if (p->cur.type != TOK_COMMA) break;
            parser_advance(p);
        }

        instr = make_defw(p, lineno, exprs.data, exprs.len);
        vec_free(exprs);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    if (t.type == TOK_DEFS) {
        parser_advance(p);
        Expr *count_expr = parse_any_expr(p);
        Expr *fill_expr = NULL;
        if (p->cur.type == TOK_COMMA) {
            parser_advance(p);
            fill_expr = parse_any_expr(p);
        } else {
            fill_expr = expr_int(p->as, 0, lineno);
        }

        /* Check for too many args */
        if (p->cur.type == TOK_COMMA) {
            asm_error(p->as, lineno, "too many arguments for DEFS");
            parser_skip_to_newline(p);
            return;
        }

        instr = make_defs(p, lineno, count_expr, fill_expr);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    if (t.type == TOK_PROC) {
        parser_advance(p);
        /* PLY grammar is `asm : PROC` — the rule needs the next token to
         * be a valid `asm` terminator (NEWLINE / EOF / COLON). With LOOKAHEAD
         * = ID (e.g. `proc foo`) PLY refuses to reduce, calls p_error on the
         * ID, and the recovery discards the half-formed asm — so enter_proc
         * is NEVER invoked and the trailing "Missing ENDP" cascade does
         * not fire. Mirror that by deferring enter_proc until we confirm
         * the lookahead is acceptable. */
        if (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_EOF ||
            p->cur.type == TOK_COLON) {
            mem_enter_proc(p->as, lineno);
        }
        return;
    }

    if (t.type == TOK_ENDP) {
        parser_advance(p);
        mem_exit_proc(p->as, lineno);
        return;
    }

    if (t.type == TOK_LOCAL) {
        parser_advance(p);
        /* Parse comma-separated list of identifiers */
        for (;;) {
            if (p->cur.type != TOK_ID) {
                asm_error(p->as, lineno, "Expected identifier after LOCAL");
                break;
            }
            mem_set_label(p->as, p->cur.sval, p->cur.lineno, true);
            parser_advance(p);
            if (p->cur.type != TOK_COMMA) break;
            parser_advance(p);
        }
        return;
    }

    if (t.type == TOK_NAMESPACE) {
        parser_advance(p);
        if (p->cur.type == TOK_ID) {
            p->as->mem.namespace_ = normalize_namespace(p->as, p->cur.sval);
            parser_advance(p);
        }
        return;
    }

    if (t.type == TOK_END) {
        parser_advance(p);
        if (p->cur.type != TOK_NEWLINE && p->cur.type != TOK_EOF) {
            Expr *addr = parse_any_expr(p);
            int64_t v;
            if (expr_eval(p->as, addr, &v, false)) {
                p->as->has_autorun = true;
                p->as->autorun_addr = v;
            }
        }
        /* Skip rest of input (END means stop) */
        while (p->cur.type != TOK_EOF) {
            parser_advance(p);
        }
        return;
    }

    if (t.type == TOK_INCBIN) {
        parser_advance(p);
        if (p->cur.type != TOK_STRING) {
            asm_error(p->as, lineno, "Expected filename after INCBIN");
            parser_skip_to_newline(p);
            return;
        }
        char *fname = p->cur.sval;
        parser_advance(p);

        /* Optional offset and length */
        int64_t offset = 0;
        int64_t length = -1;

        if (p->cur.type == TOK_COMMA) {
            parser_advance(p);
            Expr *off_expr = parse_any_expr(p);
            expr_eval(p->as, off_expr, &offset, false);
        }
        if (p->cur.type == TOK_COMMA) {
            parser_advance(p);
            Expr *len_expr = parse_any_expr(p);
            expr_eval(p->as, len_expr, &length, false);
            if (length < 1) {
                asm_error(p->as, lineno, "INCBIN length must be greater than 0");
                return;
            }
        }

        /* Search for file relative to current file */
        char path[1024];
        if (p->as->current_file) {
            /* Try relative to current file directory */
            const char *dir = p->as->current_file;
            const char *last_sep = strrchr(dir, '/');
            if (last_sep) {
                snprintf(path, sizeof(path), "%.*s/%s",
                         (int)(last_sep - dir), dir, fname);
            } else {
                snprintf(path, sizeof(path), "%s", fname);
            }
        } else {
            snprintf(path, sizeof(path), "%s", fname);
        }

        /* Resolve via the search path (mirrors zxbpp.search_filename in
         * src/zxbasm/asmparse.py:393): a file absent from every search
         * location is "not found" (zxbpp.py:204). "cannot read file" is
         * reserved for the open/read failure of a file that *was* located
         * (the Python open_file IOError branch, asmparse.py:402). */
        const char *resolved = NULL;
        char ip_path[1024];
        if (access(path, R_OK) == 0) {
            resolved = path;
        } else if (access(fname, R_OK) == 0) {
            resolved = fname;
        } else {
            /* Search the threaded-in include path (arch stdlib/runtime +
             * -I dirs), mirroring Python's INCBIN -> zxbpp.search_filename
             * (src/zxbasm/asmparse.py:393).  An absolute fname is never
             * joined; only relative names are tried under each dir. */
            bool is_abs = (fname[0] == '/');
            if (!is_abs) {
                for (int ip = 0; ip < p->as->include_paths_count; ip++) {
                    snprintf(ip_path, sizeof(ip_path), "%s/%s",
                             p->as->include_paths[ip], fname);
                    if (access(ip_path, R_OK) == 0) {
                        resolved = ip_path;
                        break;
                    }
                }
            }
        }
        if (!resolved) {
            asm_error(p->as, lineno, "file '%s' not found", fname);
            return;
        }

        FILE *f = fopen(resolved, "rb");
        if (!f) {
            asm_error(p->as, lineno, "cannot read file '%s'", fname);
            return;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (offset < 0) offset = fsize + offset;
        if (offset < 0 || offset >= fsize) {
            /* Python p_incbin reports this at p.lineno(4) — the offset
             * EXPR, a PLY non-terminal whose lineno defaults to 0 (unlike
             * the length errors at p.lineno(4)'s sibling COMMA terminal,
             * which carry the real line). */
            asm_error(p->as, 0, "INCBIN offset is out of range");
            fclose(f);
            return;
        }

        if (length < 0) length = fsize - offset;
        if (offset + length > fsize) {
            asm_warning(p->as, lineno,
                "INCBIN length if beyond file length by %d bytes",
                (int)(fsize - (offset + length)));
        }

        uint8_t *data = arena_alloc(&p->as->arena, (size_t)length);
        fseek(f, (long)offset, SEEK_SET);
        size_t nread = fread(data, 1, (size_t)length, f);
        fclose(f);

        instr = make_defb_raw(p, lineno, data, (int)nread);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- #init preprocessor directive ---- */
    if (t.type == TOK_INIT) {
        parser_advance(p);
        if (p->cur.type == TOK_STRING) {
            InitEntry entry;
            entry.label = arena_strdup(&p->as->arena, p->cur.sval);
            entry.lineno = p->cur.lineno;
            vec_push(p->as->inits, entry);
            parser_advance(p);
        }
        return;
    }

    /* ---- ZX Next: MUL D,E ---- */
    if (t.type == TOK_MUL_INSTR) {
        parser_advance(p);
        parser_expect(p, TOK_D);
        parser_expect(p, TOK_COMMA);
        parser_expect(p, TOK_E);
        instr = make_instr(p, lineno, "MUL D,E");
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- ZX Next: NEXTREG ---- */
    if (t.type == TOK_NEXTREG) {
        parser_advance(p);
        Expr *reg = parse_any_expr(p);
        parser_expect(p, TOK_COMMA);
        if (p->cur.type == TOK_A) {
            parser_advance(p);
            instr = make_instr_expr(p, lineno, "NEXTREG N,A", reg);
        } else {
            Expr *val = parse_any_expr(p);
            instr = make_instr_2expr(p, lineno, "NEXTREG N,N", reg, val);
        }
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- ZX Next: TEST ---- */
    if (t.type == TOK_TEST) {
        parser_advance(p);
        Expr *val = parse_any_expr(p);
        instr = make_instr_expr(p, lineno, "TEST N", val);
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* ---- ZX Next: BSLA/BSRA/BSRL/BSRF/BRLC DE,B ---- */
    if (t.type == TOK_BSLA || t.type == TOK_BSRA || t.type == TOK_BSRL ||
        t.type == TOK_BSRF || t.type == TOK_BRLC) {
        const char *op = t.sval;
        parser_advance(p);
        parser_expect(p, TOK_DE);
        parser_expect(p, TOK_COMMA);
        parser_expect(p, TOK_B);
        instr = make_instr(p, lineno, mnemonic_buf(p, "%s DE,B", op));
        if (instr) mem_add_instruction(p->as, instr);
        return;
    }

    /* If we get here, it's an error — use the PLY-shape formatter so the
     * report carries source lexeme + canonical PLY token NAME (Python's
     * p.value / p.type). The non-printable / unknown-char path (e.g. '~'
     * which lexes to TOK_BXOR) renders as '~' [BXOR] not '?' [14]. */
    (void)lineno;
    asm_unexpected(p);
    parser_skip_to_newline(p);
}

/* ----------------------------------------------------------------
 * Main parse loop
 * ---------------------------------------------------------------- */
static void parse_program(Parser *p)
{
    while (p->cur.type != TOK_EOF) {
        if (p->as->error_count > 0 && p->as->error_count > p->as->max_errors) {
            return;
        }

        /* Skip blank lines */
        if (p->cur.type == TOK_NEWLINE) {
            parser_advance(p);
            continue;
        }

        /* Parse one or more instructions separated by colons.  A syntax
         * error inside a statement makes PLY discard the rest of the
         * logical line (error recovery resyncs on NEWLINE) and emit no
         * further error for it — so once error_count rises, skip to the
         * line end instead of mis-parsing the remainder via the colon
         * loop (which produced the spurious cascade, e.g. `and :` ->
         * "Unexpected token 'A'"). */
        int err0 = p->as->error_count;
        /* Reset per-statement decl flag; mem_declare_label sets it. */
        p->stmt_had_decl = false;
        parse_asm(p);

        /* After an instruction, expect colon (more instructions), newline, or EOF */
        while (p->as->error_count == err0 && p->cur.type == TOK_COLON) {
            parser_advance(p);
            if (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_EOF)
                break;
            parse_asm(p);
        }

        if (p->as->error_count > err0) {
            /* Statement errored — resync on the NEWLINE (no cascade). */
            parser_skip_to_newline(p);
            if (p->cur.type == TOK_NEWLINE) parser_advance(p);
            continue;
        }

        /* Expect newline or EOF */
        if (p->cur.type == TOK_NEWLINE) {
            parser_advance(p);
        } else if (p->cur.type != TOK_EOF) {
            asm_unexpected(p);
            parser_skip_to_newline(p);
            if (p->cur.type == TOK_NEWLINE) parser_advance(p);
        }
    }
}

/* ----------------------------------------------------------------
 * Public API — called from asm_core.c
 * ---------------------------------------------------------------- */
int asm_parser_parse(AsmState *as, const char *input)
{
    /* Empty / whitespace-only input: PLY's parser cannot reduce its
     * start symbol and calls p_error(None) — written verbatim, no
     * "file:line: error:" prefix, no newline, then has_errors += 1
     * (src/zxbasm/asmparse.py:986-989). Reached when the preprocessed
     * text has no tokens (an empty source, or one whose preprocessor
     * produced no output). */
    /* Wholly-empty / whitespace-only source: PLY's parser cannot reduce
     * its start symbol and calls p_error(None) — written verbatim, no
     * "file:line: error:" prefix, no newline (src/zxbasm/asmparse.py:986-989).
     * Matches the empty-source / no-preproc-output case (e.g. newl.err). */
    if (input) {
        const char *c = input;
        while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' ||
               *c == '\f' || *c == '\v')
            c++;
        if (*c == '\0') {
            fprintf(as->err_file ? as->err_file : stderr,
                    "General syntax error at assembler "
                    "(unexpected End of File?)");
            as->error_count++;
            return as->error_count;
        }
    }

    Parser parser;
    parser_init(&parser, as, input);

    /* PLY p_error(None) — fires when the parse ends at EOF without the
     * stack reaching the accept state (asmparse.py:981-989). For a non-
     * empty source whose preprocessed output yields ONLY NEWLINE tokens
     * (i.e. no `asm` production ever reduces beyond the empty-asms case),
     * Python's PLY also calls p_error(None). The C preprocessor emits a
     * placeholder NEWLINE for a bad source line that was processed-with-
     * errors (e.g. `#define @` -> `#line ...\n\n`), so the asm parser
     * sees `NEWLINE EOF` rather than `EOF` — drain leading NEWLINEs and
     * emit the footer if NO real (non-NEWLINE) token follows AND at
     * least one NEWLINE was observed. Example: preprocerr2.asm.
     *
     * The `at least one NEWLINE` requirement is what keeps the include-
     * failure path (preprocerr1.asm) from spuriously emitting the
     * footer — there the C preproc emits no placeholder NEWLINE after
     * its initial #line, so the lex stream is empty (first = EOF). */
    {
        Lexer probe = parser.lex;     /* snapshot lex state */
        Token first = parser.cur;
        bool saw_newline = false;
        while (first.type == TOK_NEWLINE) {
            saw_newline = true;
            first = lexer_next(&probe);
        }
        if (saw_newline && first.type == TOK_EOF) {
            fprintf(as->err_file ? as->err_file : stderr,
                    "General syntax error at assembler "
                    "(unexpected End of File?)");
            as->error_count++;
            return as->error_count;
        }
    }

    parse_program(&parser);
    return as->error_count;
}
