/*
 * parser.c — BASIC parser for ZX BASIC compiler
 *
 * Hand-written recursive descent parser with Pratt expression parsing.
 * Ported from src/zxbc/zxbparser.py.
 */
#include "parser.h"
#include "errmsg.h"

#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ----------------------------------------------------------------
 * Token management
 * ---------------------------------------------------------------- */

static void advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = blexer_next(&p->lexer);
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
            n->type_ = st->basic_types[TYPE_float];
        }
    }
    return n;
}

static AstNode *make_string(Parser *p, const char *value, int lineno) {
    AstNode *n = ast_new(p->cs, AST_STRING, lineno);
    n->u.string.value = arena_strdup(&p->cs->arena, value);
    n->u.string.length = (int)strlen(value);
    n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
    return n;
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

static AstNode *make_asm_node(Parser *p, const char *code, int lineno) {
    AstNode *n = ast_new(p->cs, AST_ASM, lineno);
    n->u.asm_block.code = arena_strdup(&p->cs->arena, code);
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
static AstNode *parse_call_or_array(Parser *p, const char *name, int lineno, bool expr_context);
static AstNode *parse_arglist(Parser *p);

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
            /* PEEK(type, addr) */
            consume(p, BTOK_COMMA, "Expected ',' after PEEK type");
            AstNode *addr = parse_expression(p, PREC_NONE + 1);
            if (addr) ast_add_child(p->cs, n, addr);
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
    if (had_paren) {
        while (match(p, BTOK_COMMA)) {
            AstNode *extra = parse_expression(p, PREC_NONE + 1);
            if (extra) ast_add_child(p->cs, n, extra);
        }
        consume(p, BTOK_RP, "Expected ')' after builtin argument");
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
            n->type_ = st->basic_types[TYPE_float];
            break;
        case BTOK_LEN:
            n->type_ = st->basic_types[TYPE_uinteger];
            break;
        case BTOK_PEEK:
            n->type_ = st->basic_types[TYPE_ubyte];
            break;
        case BTOK_CODE:
            n->type_ = st->basic_types[TYPE_ubyte];
            break;
        case BTOK_USR:
            n->type_ = st->basic_types[TYPE_uinteger];
            break;
        case BTOK_STR:
            n->type_ = st->basic_types[TYPE_string];
            break;
        case BTOK_CHR:
            n->type_ = st->basic_types[TYPE_string];
            break;
        case BTOK_VAL:
            n->type_ = st->basic_types[TYPE_float];
            break;
        default:
            n->type_ = arg->type_;
            break;
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

        AstNode *n = ast_new(p->cs, AST_TYPECAST, lineno);
        n->type_ = target;
        ast_add_child(p->cs, n, expr);
        return n;
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

        AstNode *n = ast_new(p->cs, AST_UNARY, lineno);
        n->u.unary.operator = arena_strdup(&p->cs->arena, "ADDRESS");

        /* Check for array/call access: @name(...) */
        AstNode *operand;
        if (check(p, BTOK_LP)) {
            operand = parse_call_or_array(p, name, lineno, true);
        } else {
            operand = ast_new(p->cs, AST_ID, lineno);
            operand->u.id.name = arena_strdup(&p->cs->arena, name);
        }
        ast_add_child(p->cs, n, operand);
        n->type_ = p->cs->symbol_table->basic_types[TYPE_uinteger];
        return n;
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
            return parse_call_or_array(p, name, lineno, true);
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
            /* Function with 0 args — treat as call (matching Python p_id_expr) */
            if (entry->u.id.class_ == CLASS_function) {
                AstNode *call = ast_new(p->cs, AST_FUNCCALL, lineno);
                AstNode *args = ast_new(p->cs, AST_ARGLIST, lineno);
                ast_add_child(p->cs, call, entry);
                ast_add_child(p->cs, call, args);
                call->type_ = entry->type_;
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

/* Parse function call or array access: name(...) */
static AstNode *parse_call_or_array(Parser *p, const char *name, int lineno, bool expr_context) {
    consume(p, BTOK_LP, "Expected '('");

    /* Parse argument list */
    AstNode *arglist = ast_new(p->cs, AST_ARGLIST, lineno);

    bool has_to = false;
    if (!check(p, BTOK_RP)) {
        do {
            /* Check for TO without lower bound: (TO expr) */
            AstNode *arg_expr = NULL;
            if (check(p, BTOK_TO)) {
                has_to = true;
                advance(p);
                AstNode *upper = NULL;
                if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                    upper = parse_expression(p, PREC_NONE + 1);
                AstNode *slice = ast_new(p->cs, AST_STRSLICE, lineno);
                /* NULL lower = from start */
                if (upper) ast_add_child(p->cs, slice, upper);
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
            /* Check for string slice: expr TO [expr] */
            if (match(p, BTOK_TO)) {
                has_to = true;
                AstNode *upper = NULL;
                if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                    upper = parse_expression(p, PREC_NONE + 1);
                AstNode *slice = ast_new(p->cs, AST_STRSLICE, arg_expr ? arg_expr->lineno : lineno);
                if (arg_expr) ast_add_child(p->cs, slice, arg_expr);
                if (upper) ast_add_child(p->cs, slice, upper);
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

    /* String slice: name$(from TO to) */
    if (has_to) {
        AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
        AstNode *id_node = ast_new(p->cs, AST_ID, lineno);
        id_node->u.id.name = arena_strdup(&p->cs->arena, name);
        id_node->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        ast_add_child(p->cs, n, id_node);
        for (int i = 0; i < arglist->child_count; i++)
            ast_add_child(p->cs, n, arglist->children[i]);
        n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        return n;
    }

    /* Resolve via symbol table: auto-declares if needed (matching Python's access_call) */
    AstNode *entry = symboltable_access_call(p->cs->symbol_table, p->cs, name, lineno, NULL);

    /* In Python, newly implicitly declared CLASS_unknown entries have callable=False.
     * Only access_func (used for statement-level sub calls) sets callable=True.
     * In expression context, CLASS_unknown non-string entries are not callable.
     * In statement context (could be a forward sub call), they're allowed. */
    if (expr_context && entry && entry->u.id.class_ == CLASS_unknown && !type_is_string(entry->type_)) {
        err_not_array_nor_func(p->cs, lineno, name);
        return NULL;
    }

    if (entry && entry->u.id.class_ == CLASS_array) {
        /* Array access */
        AstNode *n = ast_new(p->cs, AST_ARRAYACCESS, lineno);
        entry->u.id.accessed = true;
        ast_add_child(p->cs, n, entry);
        for (int i = 0; i < arglist->child_count; i++)
            ast_add_child(p->cs, n, arglist->children[i]);
        n->type_ = entry->type_;
        return n;
    }

    if (entry && entry->u.id.class_ == CLASS_var && type_is_string(entry->type_)) {
        /* String slicing: name(expr) */
        AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
        entry->u.id.accessed = true;
        ast_add_child(p->cs, n, entry);
        for (int i = 0; i < arglist->child_count; i++)
            ast_add_child(p->cs, n, arglist->children[i]);
        n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
        return n;
    }

    /* Function call */
    AstNode *n = ast_new(p->cs, AST_FUNCCALL, lineno);
    if (entry) {
        entry->u.id.accessed = true;
        n->type_ = entry->type_;
    } else {
        /* access_call returned NULL (error already reported) — create placeholder */
        entry = ast_new(p->cs, AST_ID, lineno);
        entry->u.id.name = arena_strdup(&p->cs->arena, name);
        n->type_ = p->cs->default_type;
    }
    ast_add_child(p->cs, n, entry);
    ast_add_child(p->cs, n, arglist);
    return n;
}

/* Parse postfix indexing/slicing: expr(...) */
static AstNode *parse_postfix(Parser *p, AstNode *left) {
    while (check(p, BTOK_LP)) {
        int lineno = p->current.lineno;
        advance(p); /* consume ( */

        /* Parse argument list — may contain TO for string slicing */
        AstNode *arglist = ast_new(p->cs, AST_ARGLIST, lineno);
        bool has_to = false;

        if (!check(p, BTOK_RP)) {
            do {
                /* TO without lower bound */
                if (check(p, BTOK_TO)) {
                    has_to = true;
                    advance(p);
                    AstNode *upper = NULL;
                    if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                        upper = parse_expression(p, PREC_NONE + 1);
                    AstNode *slice = ast_new(p->cs, AST_STRSLICE, lineno);
                    if (upper) ast_add_child(p->cs, slice, upper);
                    ast_add_child(p->cs, arglist, slice);
                    continue;
                }
                AstNode *arg_expr = parse_expression(p, PREC_NONE + 1);
                if (match(p, BTOK_TO)) {
                    has_to = true;
                    AstNode *upper = NULL;
                    if (!check(p, BTOK_RP) && !check(p, BTOK_COMMA))
                        upper = parse_expression(p, PREC_NONE + 1);
                    AstNode *slice = ast_new(p->cs, AST_STRSLICE, arg_expr ? arg_expr->lineno : lineno);
                    if (arg_expr) ast_add_child(p->cs, slice, arg_expr);
                    if (upper) ast_add_child(p->cs, slice, upper);
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

        if (has_to) {
            /* String slice */
            AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
            ast_add_child(p->cs, n, left);
            for (int i = 0; i < arglist->child_count; i++)
                ast_add_child(p->cs, n, arglist->children[i]);
            n->type_ = p->cs->symbol_table->basic_types[TYPE_string];
            left = n;
        } else {
            /* Array access or function call on expression result */
            AstNode *n = ast_new(p->cs, AST_ARRAYACCESS, lineno);
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
    left = parse_postfix(p, left);

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
static AstNode *parse_print_statement(Parser *p);
static AstNode *parse_sub_or_func_decl(Parser *p, bool is_function);

/* Graphics attributes for PLOT/DRAW/CIRCLE: INK/PAPER/BRIGHT/FLASH/OVER/INVERSE expr ;
 * Python grammar: attr_list : attr SC | attr_list attr SC
 * Note: BOLD/ITALIC are NOT valid here (only in PRINT). */
static void parse_gfx_attributes(Parser *p, AstNode *parent) {
    for (;;) {
        const char *attr_name = NULL;
        if (match(p, BTOK_INK))          attr_name = "INK_TMP";
        else if (match(p, BTOK_PAPER))   attr_name = "PAPER_TMP";
        else if (match(p, BTOK_BRIGHT))  attr_name = "BRIGHT_TMP";
        else if (match(p, BTOK_FLASH))   attr_name = "FLASH_TMP";
        else if (match(p, BTOK_OVER))    attr_name = "OVER_TMP";
        else if (match(p, BTOK_INVERSE)) attr_name = "INVERSE_TMP";
        if (!attr_name) break;
        AstNode *val = parse_expression(p, PREC_NONE + 1);
        AstNode *attr_sent = make_sentence_node(p, attr_name, p->previous.lineno);
        if (val) ast_add_child(p->cs, attr_sent, val);
        ast_add_child(p->cs, parent, attr_sent);
        consume(p, BTOK_SC, "Expected ';' after graphics attribute");
    }
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

        /* Create label in symbol table (labels are always global) */
        AstNode *label_node = symboltable_access_label(p->cs->symbol_table, p->cs,
                                                        label_text, p->previous.lineno);
        if (label_node && label_node->u.id.class_ == CLASS_label) {
            if (label_node->u.id.declared) {
                zxbc_error(p->cs, p->previous.lineno,
                           "Label '%s' already used at %s:%d",
                           label_text, p->cs->current_file, label_node->lineno);
            }
            label_node->u.id.declared = true;
        }

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
            AstNode *lbl_sent = make_sentence_node(p, "LABEL", lineno);
            AstNode *lbl_id = ast_new(p->cs, AST_ID, lineno);
            lbl_id->u.id.name = arena_strdup(&p->cs->arena, label_text);
            lbl_id->u.id.class_ = CLASS_label;
            ast_add_child(p->cs, lbl_sent, lbl_id);
            ast_add_child(p->cs, block, lbl_sent);
            if (stmt) ast_add_child(p->cs, block, stmt);
            return block;
        }

        AstNode *lbl_sent = make_sentence_node(p, "LABEL", lineno);
        AstNode *lbl_id = ast_new(p->cs, AST_ID, lineno);
        lbl_id->u.id.name = arena_strdup(&p->cs->arena, label_text);
        lbl_id->u.id.class_ = CLASS_label;
        ast_add_child(p->cs, lbl_sent, lbl_id);
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

    /* RETURN */
    if (match(p, BTOK_RETURN)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "RETURN", ln);
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            if (expr) ast_add_child(p->cs, s, expr);
        }
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

    /* STOP */
    if (match(p, BTOK_STOP)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "STOP", ln);
        AstNode *code = make_number(p, 9, ln, p->cs->symbol_table->basic_types[TYPE_uinteger]);
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            code = parse_expression(p, PREC_NONE + 1);
        }
        ast_add_child(p->cs, s, code);
        return s;
    }

    /* CLS */
    if (match(p, BTOK_CLS)) {
        return make_sentence_node(p, "CLS", p->previous.lineno);
    }

    /* BORDER expr */
    if (match(p, BTOK_BORDER)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "BORDER", ln);
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        if (expr) ast_add_child(p->cs, s, expr);
        return s;
    }

    /* PAUSE expr */
    if (match(p, BTOK_PAUSE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "PAUSE", ln);
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
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
        if (addr) ast_add_child(p->cs, s, addr);
        if (val) ast_add_child(p->cs, s, val);
        return s;
    }

    /* OUT port, val */
    if (match(p, BTOK_OUT)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "OUT", ln);
        AstNode *port = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' after OUT port");
        AstNode *val = parse_expression(p, PREC_NONE + 1);
        if (port) ast_add_child(p->cs, s, port);
        if (val) ast_add_child(p->cs, s, val);
        return s;
    }

    /* BEEP duration, pitch */
    if (match(p, BTOK_BEEP)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "BEEP", ln);
        AstNode *dur = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' in BEEP");
        AstNode *pitch = parse_expression(p, PREC_NONE + 1);
        if (dur) ast_add_child(p->cs, s, dur);
        if (pitch) ast_add_child(p->cs, s, pitch);
        return s;
    }

    /* RANDOMIZE [expr] */
    if (match(p, BTOK_RANDOMIZE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "RANDOMIZE", ln);
        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            if (expr) ast_add_child(p->cs, s, expr);
        }
        return s;
    }

    /* PLOT [attributes;] x, y */
    if (match(p, BTOK_PLOT)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "PLOT", ln);
        parse_gfx_attributes(p, s);
        AstNode *x = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' after PLOT x");
        AstNode *y = parse_expression(p, PREC_NONE + 1);
        if (x) ast_add_child(p->cs, s, x);
        if (y) ast_add_child(p->cs, s, y);
        return s;
    }

    /* DRAW [attributes;] x, y [, z] */
    if (match(p, BTOK_DRAW)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "DRAW", ln);
        parse_gfx_attributes(p, s);
        AstNode *x = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' after DRAW x");
        AstNode *y = parse_expression(p, PREC_NONE + 1);
        if (x) ast_add_child(p->cs, s, x);
        if (y) ast_add_child(p->cs, s, y);
        if (match(p, BTOK_COMMA)) {
            AstNode *z = parse_expression(p, PREC_NONE + 1);
            if (z) ast_add_child(p->cs, s, z);
            s->u.sentence.kind = arena_strdup(&p->cs->arena, "DRAW3");
        }
        return s;
    }

    /* CIRCLE [attributes;] x, y, r */
    if (match(p, BTOK_CIRCLE)) {
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, "CIRCLE", ln);
        parse_gfx_attributes(p, s);
        AstNode *x = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' in CIRCLE");
        AstNode *y = parse_expression(p, PREC_NONE + 1);
        consume(p, BTOK_COMMA, "Expected ',' in CIRCLE");
        AstNode *r = parse_expression(p, PREC_NONE + 1);
        if (x) ast_add_child(p->cs, s, x);
        if (y) ast_add_child(p->cs, s, y);
        if (r) ast_add_child(p->cs, s, r);
        return s;
    }

    /* FUNCTION / SUB declarations */
    if (check(p, BTOK_FUNCTION)) {
        return parse_sub_or_func_decl(p, true);
    }
    if (check(p, BTOK_SUB)) {
        return parse_sub_or_func_decl(p, false);
    }

    /* DECLARE (forward declaration) */
    if (match(p, BTOK_DECLARE)) {
        bool is_func = check(p, BTOK_FUNCTION);
        if (!is_func && !check(p, BTOK_SUB)) {
            parser_error(p, "Expected FUNCTION or SUB after DECLARE");
            return NULL;
        }
        /* For now, parse and discard — forward decl handling */
        return parse_sub_or_func_decl(p, is_func);
    }

    /* EXIT DO/FOR/WHILE */
    if (match(p, BTOK_EXIT)) {
        int ln = p->previous.lineno;
        const char *loop_kw = NULL;
        if (match(p, BTOK_DO)) loop_kw = "EXIT_DO";
        else if (match(p, BTOK_FOR)) loop_kw = "EXIT_FOR";
        else if (match(p, BTOK_WHILE)) loop_kw = "EXIT_WHILE";
        else {
            parser_error(p, "Expected DO, FOR, or WHILE after EXIT");
            return NULL;
        }
        return make_sentence_node(p, loop_kw, ln);
    }

    /* CONTINUE DO/FOR/WHILE */
    if (match(p, BTOK_CONTINUE)) {
        int ln = p->previous.lineno;
        const char *loop_kw = NULL;
        if (match(p, BTOK_DO)) loop_kw = "CONTINUE_DO";
        else if (match(p, BTOK_FOR)) loop_kw = "CONTINUE_FOR";
        else if (match(p, BTOK_WHILE)) loop_kw = "CONTINUE_WHILE";
        else {
            parser_error(p, "Expected DO, FOR, or WHILE after CONTINUE");
            return NULL;
        }
        return make_sentence_node(p, loop_kw, ln);
    }

    /* DATA */
    if (match(p, BTOK_DATA)) {
        int ln = p->previous.lineno;
        /* DATA not allowed inside functions/subs (matching Python) */
        if (p->cs->function_level.len > 0) {
            zxbc_error(p->cs, ln, "DATA not allowed within Functions nor Subs");
        }
        AstNode *s = make_sentence_node(p, "DATA", ln);
        do {
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            if (expr) ast_add_child(p->cs, s, expr);
        } while (match(p, BTOK_COMMA));
        p->cs->data_is_used = true;
        return s;
    }

    /* READ */
    if (match(p, BTOK_READ)) {
        int ln = p->previous.lineno;
        AstNode *block = make_block_node(p, ln);
        do {
            AstNode *s = make_sentence_node(p, "READ", ln);
            /* Can read into variable, array element, or expression */
            AstNode *target = parse_expression(p, PREC_NONE + 1);
            if (target) ast_add_child(p->cs, s, target);
            ast_add_child(p->cs, block, s);
        } while (match(p, BTOK_COMMA));
        return block;
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

    /* SAVE/LOAD/VERIFY — just skip arguments for now */
    if (match(p, BTOK_SAVE) || match(p, BTOK_LOAD) || match(p, BTOK_VERIFY)) {
        const char *kind = btok_name(p->previous.type);
        int ln = p->previous.lineno;
        AstNode *s = make_sentence_node(p, kind, ln);
        while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO))
            advance(p);
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
            int ln = p->previous.lineno;
            AstNode *s = make_sentence_node(p, attr_name, ln);
            AstNode *val = parse_expression(p, PREC_NONE + 1);
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
        /* Skip the required library name for now */
        if (check(p, BTOK_STRC)) advance(p);
        return make_nop(p);
    }
    if (match(p, BTOK__PRAGMA)) {
        /* #pragma NAME = VALUE — set compiler option (matching Python setattr(OPTIONS, name, value))
         * #pragma push(NAME) / #pragma pop(NAME) — save/restore option */
        if (check(p, BTOK_ID)) {
            const char *opt_name = p->current.sval;
            advance(p);
            if (match(p, BTOK_EQ)) {
                /* Parse value: True/False (ID), integer, or string */
                bool bool_val = false;
                bool is_bool = false;
                if (check(p, BTOK_ID)) {
                    const char *val = p->current.sval;
                    advance(p);
                    if (strcasecmp(val, "true") == 0) { bool_val = true; is_bool = true; }
                    else if (strcasecmp(val, "false") == 0) { bool_val = false; is_bool = true; }
                } else if (check(p, BTOK_NUMBER)) {
                    bool_val = (p->current.numval != 0);
                    is_bool = true;
                    advance(p);
                }
                /* Apply known options */
                if (is_bool) {
                    if (strcasecmp(opt_name, "explicit") == 0) p->cs->opts.explicit_ = bool_val;
                    else if (strcasecmp(opt_name, "strict") == 0) p->cs->opts.strict = bool_val;
                    else if (strcasecmp(opt_name, "strict_bool") == 0) p->cs->opts.strict_bool = bool_val;
                    else if (strcasecmp(opt_name, "array_check") == 0) p->cs->opts.array_check = bool_val;
                    else if (strcasecmp(opt_name, "memory_check") == 0) p->cs->opts.memory_check = bool_val;
                    else if (strcasecmp(opt_name, "sinclair") == 0) p->cs->opts.sinclair = bool_val;
                }
            }
        }
        /* Skip any remaining tokens on this line */
        while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF)) advance(p);
        return make_nop(p);
    }

    /* ID or ARRAY_ID at start — either assignment or sub call */
    if (check(p, BTOK_ID) || check(p, BTOK_ARRAY_ID)) {
        advance(p);
        const char *name = p->previous.sval;
        int ln = p->previous.lineno;
        bool was_let = p->cs->let_assignment;
        p->cs->let_assignment = false;

        /* Array element assignment: ID(index) = expr or ID(i)(j TO k) = expr */
        if (check(p, BTOK_LP)) {
            AstNode *call_node = parse_call_or_array(p, name, ln, false);
            /* Handle chained postfix: a$(b)(1 TO 5) */
            call_node = parse_postfix(p, call_node);

            /* Check if followed by = (assignment to array element or func return) */
            if (match(p, BTOK_EQ) || was_let) {
                if (p->previous.type != BTOK_EQ) consume(p, BTOK_EQ, "Expected '=' in assignment");
                AstNode *expr = parse_expression(p, PREC_NONE + 1);
                AstNode *s = make_sentence_node(p, "LETARRAY", ln);
                ast_add_child(p->cs, s, call_node);
                if (expr) ast_add_child(p->cs, s, expr);
                return s;
            }

            /* If followed by an operator, this is an expression-as-statement
             * e.g. test(1) + test(2) — parse remaining binary ops */
            if (get_precedence(p->current.type) > PREC_NONE) {
                call_node = parse_infix(p, call_node, PREC_NONE + 1);
            }

            /* Sub call: ID(args) as statement */
            if (call_node && call_node->tag == AST_FUNCCALL) {
                call_node->tag = AST_CALL;
            }
            return call_node;
        }

        /* Simple assignment: ID = expr */
        if (match(p, BTOK_EQ) || was_let) {
            if (p->previous.type != BTOK_EQ) {
                consume(p, BTOK_EQ, "Expected '=' in assignment");
            }
            /* Resolve target via symbol table (triggers explicit mode check) */
            AstNode *var = symboltable_access_id(p->cs->symbol_table, p->cs,
                                                  name, ln, NULL, CLASS_var);
            if (var) {
                /* Check if target is assignable */
                if (var->u.id.class_ == CLASS_const) {
                    zxbc_error(p->cs, ln, "'%s' is a CONST, not a VAR", name);
                } else if (var->u.id.class_ == CLASS_sub) {
                    zxbc_error(p->cs, ln, "Cannot assign a value to '%s'. It's not a variable", name);
                } else if (var->u.id.class_ == CLASS_function) {
                    zxbc_error(p->cs, ln, "'%s' is a FUNCTION, not a VAR", name);
                }
                if (var->u.id.class_ == CLASS_unknown)
                    var->u.id.class_ = CLASS_var;
            } else {
                /* access_id returned NULL (explicit mode error already reported) */
                var = ast_new(p->cs, AST_ID, ln);
                var->u.id.name = arena_strdup(&p->cs->arena, name);
                var->u.id.class_ = CLASS_unknown;
            }
            AstNode *expr = parse_expression(p, PREC_NONE + 1);
            AstNode *s = make_sentence_node(p, "LET", ln);
            ast_add_child(p->cs, s, var);
            if (expr) ast_add_child(p->cs, s, expr);
            return s;
        }

        /* Sub call without parentheses: ID expr, expr, ...
         * Check if the identifier is callable (sub/function/unknown).
         * Variables and constants can't be called. */
        {
            AstNode *entry = symboltable_lookup(p->cs->symbol_table, name);
            if (entry && entry->u.id.class_ == CLASS_var) {
                zxbc_error(p->cs, ln, "'%s' is a VAR, not a FUNCTION", name);
            } else if (entry && entry->u.id.class_ == CLASS_const) {
                zxbc_error(p->cs, ln, "'%s' is a CONST, not a FUNCTION", name);
            }
        }
        AstNode *s = make_sentence_node(p, "CALL", ln);
        AstNode *id_node = ast_new(p->cs, AST_ID, ln);
        id_node->u.id.name = arena_strdup(&p->cs->arena, name);
        ast_add_child(p->cs, s, id_node);

        if (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
            AstNode *arglist = ast_new(p->cs, AST_ARGLIST, ln);
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
            ast_add_child(p->cs, s, arglist);
        }
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

    /* Error recovery */
    parser_error(p, "Unexpected token");
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
            if (stmt) ast_add_child(p->cs, then_block, stmt);
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
                if (stmt) ast_add_child(p->cs, else_block, stmt);
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
                        if (stmt) ast_add_child(p->cs, else_block, stmt);
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
        if (stmt) ast_add_child(p->cs, then_block, stmt);
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
            /* Chain: previous else's last child becomes this nested IF */
            ast_add_child(p->cs, else_block, nested_if);
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
    }

    /* Validate the FOR variable (matching Python's access_var in p_for_sentence_start) */
    {
        AstNode *var = symboltable_access_id(p->cs->symbol_table, p->cs,
                                              var_name, var_lineno, NULL, CLASS_var);
        if (var) {
            if (var->u.id.class_ == CLASS_const)
                zxbc_error(p->cs, var_lineno, "'%s' is a CONST, not a VAR", var_name);
            else if (var->u.id.class_ == CLASS_function)
                zxbc_error(p->cs, var_lineno, "'%s' is a FUNCTION, not a VAR", var_name);
            else if (var->u.id.class_ == CLASS_sub)
                zxbc_error(p->cs, var_lineno, "Cannot assign a value to '%s'. It's not a variable", var_name);
            if (var->u.id.class_ == CLASS_unknown)
                var->u.id.class_ = CLASS_var;
        }
    }

    /* Push loop info */
    LoopInfo li = { LOOP_FOR, lineno, arena_strdup(&p->cs->arena, var_name) };
    vec_push(p->cs->loop_stack, li);

    /* Parse body */
    skip_newlines(p);
    AstNode *body = make_block_node(p, lineno);
    while (!check(p, BTOK_EOF) && !check(p, BTOK_NEXT)) {
        AstNode *stmt = parse_statement(p);
        if (stmt) ast_add_child(p->cs, body, stmt);
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
    }

    /* NEXT [var] */
    consume(p, BTOK_NEXT, "Expected NEXT");
    if (check(p, BTOK_ID)) {
        advance(p);
        /* Optionally validate var matches */
    }

    /* Pop loop info */
    if (p->cs->loop_stack.len > 0) {
        vec_pop(p->cs->loop_stack);
    }

    AstNode *s = make_sentence_node(p, "FOR", lineno);
    AstNode *var = ast_new(p->cs, AST_ID, lineno);
    var->u.id.name = arena_strdup(&p->cs->arena, var_name);
    ast_add_child(p->cs, s, var);
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
        if (stmt) ast_add_child(p->cs, body, stmt);
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

    if (match(p, BTOK_WHILE)) {
        pre_cond = parse_expression(p, PREC_NONE + 1);
        kind = "DO_WHILE";
    } else if (match(p, BTOK_UNTIL)) {
        pre_cond = parse_expression(p, PREC_NONE + 1);
        kind = "DO_UNTIL";
    }

    skip_newlines(p);
    AstNode *body = make_block_node(p, lineno);
    while (!check(p, BTOK_EOF) && !check(p, BTOK_LOOP)) {
        AstNode *stmt = parse_statement(p);
        if (stmt) ast_add_child(p->cs, body, stmt);
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
    }

    consume(p, BTOK_LOOP, "Expected LOOP");

    /* LOOP WHILE cond / LOOP UNTIL cond */
    AstNode *post_cond = NULL;
    if (match(p, BTOK_WHILE)) {
        post_cond = parse_expression(p, PREC_NONE + 1);
        kind = "LOOP_WHILE";
    } else if (match(p, BTOK_UNTIL)) {
        post_cond = parse_expression(p, PREC_NONE + 1);
        kind = "LOOP_UNTIL";
    }

    if (p->cs->loop_stack.len > 0)
        vec_pop(p->cs->loop_stack);

    AstNode *s = make_sentence_node(p, kind, lineno);
    if (pre_cond) ast_add_child(p->cs, s, pre_cond);
    ast_add_child(p->cs, s, body);
    if (post_cond) ast_add_child(p->cs, s, post_cond);
    return s;
}

/* ----------------------------------------------------------------
 * DIM / CONST declaration
 * ---------------------------------------------------------------- */
/* Parse brace-enclosed initializer: {expr, expr, ...} or {{...}, {...}} */
static AstNode *parse_array_initializer(Parser *p) {
    int lineno = p->current.lineno;
    consume(p, BTOK_LBRACE, "Expected '{'");
    AstNode *init = ast_new(p->cs, AST_ARRAYINIT, lineno);

    if (!check(p, BTOK_RBRACE)) {
        do {
            if (check(p, BTOK_LBRACE)) {
                /* Nested initializer for multi-dim arrays */
                AstNode *sub = parse_array_initializer(p);
                if (sub) ast_add_child(p->cs, init, sub);
            } else {
                AstNode *expr = parse_expression(p, PREC_NONE + 1);
                if (expr) ast_add_child(p->cs, init, expr);
            }
        } while (match(p, BTOK_COMMA));
    }
    consume(p, BTOK_RBRACE, "Expected '}'");
    return init;
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
        if (match(p, BTOK_RIGHTARROW)) {
            init = parse_array_initializer(p);
        } else if (match(p, BTOK_EQ)) {
            if (check(p, BTOK_LBRACE)) {
                init = parse_array_initializer(p);
            } else {
                init = parse_expression(p, PREC_NONE + 1);
            }
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

        AstNode *decl = ast_new(p->cs, AST_ARRAYDECL, lineno);
        AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, name, lineno, CLASS_array);
        id_node->type_ = type;
        ast_add_child(p->cs, decl, id_node);
        ast_add_child(p->cs, decl, bounds);
        if (arr_at_expr) ast_add_child(p->cs, decl, arr_at_expr);
        if (init) ast_add_child(p->cs, decl, init);
        decl->type_ = type;
        return decl;
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

    TypeInfo *type = parse_typedef(p);

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

    if (!type) {
        type = type_new_ref(p->cs, p->cs->default_type, lineno, true);
        /* Strict mode: error on implicit type in DIM */
        if (p->cs->opts.strict) {
            for (int i = 0; i < name_count; i++)
                zxbc_error(p->cs, lineno, "strict mode: missing type declaration for '%s'", names[i]);
        }
    }

    if (name_count == 1) {
        AstNode *decl = ast_new(p->cs, AST_VARDECL, lineno);
        SymbolClass cls = is_const ? CLASS_const : CLASS_var;
        AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, name, lineno, cls);
        /* Check for duplicate declaration */
        if (id_node->u.id.declared && id_node->lineno != lineno) {
            zxbc_error(p->cs, lineno, "Variable '%s' already declared at %s:%d",
                       name, p->cs->current_file, id_node->lineno);
        }
        id_node->type_ = type;
        ast_add_child(p->cs, decl, id_node);
        if (at_expr) ast_add_child(p->cs, decl, at_expr);
        if (init_expr) ast_add_child(p->cs, decl, init_expr);
        decl->type_ = type;
        return decl;
    }

    /* Multiple vars: create a block of declarations */
    AstNode *block = make_block_node(p, lineno);
    SymbolClass cls = is_const ? CLASS_const : CLASS_var;
    for (int i = 0; i < name_count; i++) {
        AstNode *decl = ast_new(p->cs, AST_VARDECL, lineno);
        AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, names[i], lineno, cls);
        id_node->type_ = type;
        ast_add_child(p->cs, decl, id_node);
        decl->type_ = type;
        ast_add_child(p->cs, block, decl);
    }
    return block;
}

/* ----------------------------------------------------------------
 * PRINT statement
 * ---------------------------------------------------------------- */
static AstNode *parse_print_statement(Parser *p) {
    consume(p, BTOK_PRINT, "Expected PRINT");
    int lineno = p->previous.lineno;
    p->cs->print_is_used = true;

    AstNode *s = make_sentence_node(p, "PRINT", lineno);

    while (!check(p, BTOK_NEWLINE) && !check(p, BTOK_EOF) && !check(p, BTOK_CO)) {
        /* Print attributes: INK, PAPER, BRIGHT, FLASH, etc. */
        if (match(p, BTOK_AT)) {
            AstNode *row = parse_expression(p, PREC_NONE + 1);
            consume(p, BTOK_COMMA, "Expected ',' after AT row");
            AstNode *col = parse_expression(p, PREC_NONE + 1);
            AstNode *at_sent = make_sentence_node(p, "PRINT_AT", lineno);
            if (row) ast_add_child(p->cs, at_sent, row);
            if (col) ast_add_child(p->cs, at_sent, col);
            ast_add_child(p->cs, s, at_sent);
            match(p, BTOK_SC); /* optional semicolon */
            continue;
        }
        if (match(p, BTOK_TAB)) {
            AstNode *col = parse_expression(p, PREC_NONE + 1);
            AstNode *tab_sent = make_sentence_node(p, "PRINT_TAB", lineno);
            if (col) ast_add_child(p->cs, tab_sent, col);
            ast_add_child(p->cs, s, tab_sent);
            match(p, BTOK_SC);
            continue;
        }

        /* Print attributes: INK, PAPER, BRIGHT, FLASH, OVER, INVERSE, BOLD, ITALIC */
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
                AstNode *val = parse_expression(p, PREC_NONE + 1);
                AstNode *attr_sent = make_sentence_node(p, attr_name, lineno);
                if (val) ast_add_child(p->cs, attr_sent, val);
                ast_add_child(p->cs, s, attr_sent);
                match(p, BTOK_SC);
                continue;
            }
        }

        /* Separator: ; or , */
        if (match(p, BTOK_SC)) {
            AstNode *sep = make_sentence_node(p, "PRINT_COMMA", lineno);
            ast_add_child(p->cs, s, sep);
            continue;
        }
        if (match(p, BTOK_COMMA)) {
            AstNode *sep = make_sentence_node(p, "PRINT_TAB", lineno);
            ast_add_child(p->cs, s, sep);
            continue;
        }

        /* Print expression */
        AstNode *expr = parse_expression(p, PREC_NONE + 1);
        if (expr) {
            AstNode *pe = make_sentence_node(p, "PRINT_ITEM", expr->lineno);
            ast_add_child(p->cs, pe, expr);
            ast_add_child(p->cs, s, pe);
        } else {
            break;
        }
    }

    return s;
}

/* ----------------------------------------------------------------
 * FUNCTION / SUB declaration
 * ---------------------------------------------------------------- */
static AstNode *parse_sub_or_func_decl(Parser *p, bool is_function) {
    if (is_function) {
        consume(p, BTOK_FUNCTION, "Expected FUNCTION");
    } else {
        consume(p, BTOK_SUB, "Expected SUB");
    }
    int lineno = p->previous.lineno;

    /* Optional calling convention */
    Convention conv = CONV_unknown;
    if (match(p, BTOK_FASTCALL)) conv = CONV_fastcall;
    else if (match(p, BTOK_STDCALL)) conv = CONV_stdcall;

    /* Function name (may be a keyword used as identifier) */
    if (!is_name_token(p)) {
        parser_error(p, "Expected function/sub name");
        return NULL;
    }
    const char *func_name = get_name_token(p);
    advance(p);

    /* Parameters */
    AstNode *params = ast_new(p->cs, AST_PARAMLIST, lineno);
    if (match(p, BTOK_LP)) {
        if (!check(p, BTOK_RP)) {
            do {
                bool byref = p->cs->opts.default_byref;
                if (match(p, BTOK_BYREF)) byref = true;
                else if (match(p, BTOK_BYVAL)) byref = false;

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

                TypeInfo *param_type = parse_typedef(p);
                if (!param_type) {
                    param_type = type_new_ref(p->cs, p->cs->default_type, param_line, true);
                    if (p->cs->opts.strict)
                        zxbc_error(p->cs, param_line, "strict mode: missing type declaration for '%s'", param_name);
                }

                /* Default value */
                AstNode *default_val = NULL;
                if (match(p, BTOK_EQ)) {
                    default_val = parse_expression(p, PREC_NONE + 1);
                }

                AstNode *param_node = ast_new(p->cs, AST_ARGUMENT, param_line);
                param_node->u.argument.name = arena_strdup(&p->cs->arena, param_name);
                param_node->u.argument.byref = byref;
                param_node->u.argument.is_array = is_array;
                param_node->type_ = param_type;
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
            ret_type = type_new_ref(p->cs, p->cs->default_type, lineno, true);
            if (p->cs->opts.strict)
                zxbc_error(p->cs, lineno, "strict mode: missing type declaration for '%s'", func_name);
        }
    }

    /* Declare function/sub in the CURRENT (parent) scope BEFORE entering body scope.
     * This enables recursive calls — the function name is visible from inside. */
    SymbolClass cls = is_function ? CLASS_function : CLASS_sub;
    AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, func_name, lineno, cls);

    /* Check for class mismatch with previous declaration (DECLARE FUNCTION vs SUB) */
    if (id_node->u.id.class_ != CLASS_unknown && id_node->u.id.class_ != cls) {
        zxbc_error(p->cs, lineno, "'%s' is a %s, not a %s", func_name,
                   symbolclass_to_string(id_node->u.id.class_),
                   symbolclass_to_string(cls));
    }

    id_node->u.id.class_ = cls;
    id_node->u.id.convention = conv;
    id_node->type_ = ret_type;

    /* Enter function body scope */
    symboltable_enter_scope(p->cs->symbol_table, p->cs);

    /* Register parameters in the function scope (so body can reference them).
     * Strip deprecated suffixes ($%&!) so lookups match (Python strips on get_entry). */
    for (int i = 0; i < params->child_count; i++) {
        AstNode *param = params->children[i];
        const char *pname = param->u.argument.name;
        /* Strip deprecated suffix if present */
        size_t plen = strlen(pname);
        char stripped[256];
        if (plen > 0 && plen < sizeof(stripped) && is_deprecated_suffix(pname[plen - 1])) {
            memcpy(stripped, pname, plen - 1);
            stripped[plen - 1] = '\0';
            pname = stripped;
        }
        SymbolClass pcls = param->u.argument.is_array ? CLASS_array : CLASS_var;
        AstNode *sym = symboltable_declare(p->cs->symbol_table, p->cs, pname, param->lineno, pcls);
        if (sym) {
            sym->type_ = param->type_;
            sym->u.id.declared = true;
        }
    }

    /* Note: function name is NOT re-declared in the body scope.
     * The parent scope CLASS_function entry is visible via scope chain lookup,
     * enabling recursive calls. Return value assignment (funcname = expr) works
     * because Python's LET handler recognizes CLASS_function as valid LHS. */

    /* Push function level (for GOSUB check and other function-scope tracking) */
    vec_push(p->cs->function_level, id_node);

    /* Parse body */
    skip_newlines(p);
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
        if (stmt) ast_add_child(p->cs, body, stmt);
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}
    }

    /* Pop function level */
    if (p->cs->function_level.len > 0)
        vec_pop(p->cs->function_level);

    /* Exit scope */
    symboltable_exit_scope(p->cs->symbol_table);

    /* Create function declaration node (id_node was declared before entering scope) */
    AstNode *decl = ast_new(p->cs, AST_FUNCDECL, lineno);

    ast_add_child(p->cs, decl, id_node);
    ast_add_child(p->cs, decl, params);
    ast_add_child(p->cs, decl, body);
    decl->type_ = ret_type;

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
    advance(p); /* prime the first token */
}

AstNode *parser_parse(Parser *p) {
    AstNode *program = make_block_node(p, 1);

    while (!check(p, BTOK_EOF)) {
        /* Skip blank lines */
        if (match(p, BTOK_NEWLINE)) continue;

        AstNode *stmt = parse_statement(p);
        if (stmt && stmt->tag != AST_NOP) {
            ast_add_child(p->cs, program, stmt);
        }

        /* Consume end of statement */
        while (match(p, BTOK_NEWLINE) || match(p, BTOK_CO)) {}

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
