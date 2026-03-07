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
} Parser;

static void parser_init(Parser *p, AsmState *as, const char *input)
{
    p->as = as;
    lexer_init(&p->lex, as, input);
    p->has_peek = false;
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
    if (p->cur.type != TOK_NEWLINE && p->cur.type != TOK_EOF) {
        asm_error(p->as, p->cur.lineno,
                  "Syntax error. Unexpected token '%s' [%d]",
                  p->cur.sval ? p->cur.sval : "?", p->cur.type);
    } else if (p->cur.type == TOK_NEWLINE) {
        asm_error(p->as, p->cur.lineno,
                  "Syntax error. Unexpected end of line [NEWLINE]");
    }
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

    asm_error(p->as, lineno, "Expected expression");
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
    parser_advance(p);

    /* Next should be +/- followed by expression, or closing paren for +0 */
    TokenType close = bracket ? TOK_RB : TOK_RP;
    if (p->cur.type == close) {
        /* (IX) or [IX] → offset 0 */
        *offset = expr_int(p->as, 0, p->cur.lineno);
    } else {
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
            return;
        }

        if (next.type == TOK_COLON || next.type == TOK_NEWLINE ||
            next.type == TOK_EOF ||
            /* Label followed by an instruction */
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
                    asm_error(p->as, lineno, "Unexpected token");
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
            } else {
                Expr *val = parse_any_expr(p);
                if (p->as->zxnext) {
                    instr = make_instr_expr(p, lineno, "ADD HL,NN", val);
                } else {
                    asm_error(p->as, lineno, "Syntax error");
                    parser_skip_to_newline(p); return;
                }
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
        mem_enter_proc(p->as, lineno);
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

        FILE *f = fopen(path, "rb");
        if (!f) {
            f = fopen(fname, "rb");
        }
        if (!f) {
            asm_error(p->as, lineno, "cannot read file '%s'", fname);
            return;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (offset < 0) offset = fsize + offset;
        if (offset < 0 || offset >= fsize) {
            asm_error(p->as, lineno, "INCBIN offset is out of range");
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

    /* If we get here, it's an error */
    asm_error(p->as, lineno, "Syntax error. Unexpected token '%s' [%d]",
              p->cur.sval ? p->cur.sval : "?", p->cur.type);
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

        /* Parse one or more instructions separated by colons */
        parse_asm(p);

        /* After an instruction, expect colon (more instructions), newline, or EOF */
        while (p->cur.type == TOK_COLON) {
            parser_advance(p);
            if (p->cur.type == TOK_NEWLINE || p->cur.type == TOK_EOF)
                break;
            parse_asm(p);
        }

        /* Expect newline or EOF */
        if (p->cur.type == TOK_NEWLINE) {
            parser_advance(p);
        } else if (p->cur.type != TOK_EOF) {
            asm_error(p->as, p->cur.lineno,
                      "Syntax error. Unexpected token '%s' [%d]",
                      p->cur.sval ? p->cur.sval : "?", p->cur.type);
            parser_skip_to_newline(p);
            if (p->cur.type == TOK_NEWLINE) parser_advance(p);
        }
    }
}

/* ----------------------------------------------------------------
 * Public API — called from asm_core.c
 * ---------------------------------------------------------------- */
int parser_parse(AsmState *as, const char *input)
{
    Parser parser;
    parser_init(&parser, as, input);
    parse_program(&parser);
    return as->error_count;
}
