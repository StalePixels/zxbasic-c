/*
 * parser.c — BASIC parser for ZX BASIC compiler
 *
 * Hand-written recursive descent parser with Pratt expression parsing.
 * Ported from src/zxbc/zxbparser.py.
 */
#include "parser.h"
#include "errmsg.h"
#include "utils.h"      /* parse_int — same int coercion as the 'org' config arm (args.c:103) */

#include <ctype.h>      /* tolower — Python bool-coercion uses value.lower() (options.py:131) */
#include <math.h>
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
static AstNode *parse_arglist(Parser *p);

/* Forward decls used by the SymbolARRAYACCESS.offset port (computed at
 * ARRAYACCESS-node construction, defined alongside the BOUND helpers). */
static bool zxbc_eval_to_num(const AstNode *n, double *out);
static void compute_arrayaccess_offset(Parser *p, AstNode *acc,
                                       AstNode *entry, AstNode *arglist);

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

    /* p_expr_lbound_expr (zxbparser.py:3335-3378) — LBOUND/UBOUND with an
     * explicit dimension: `LBOUND(arr, expr)` / `UBOUND(arr, expr)`. The C
     * grammar funnels these through this generic builtin builder, so the
     * production's array-descriptor side effect must be reproduced here.
     *
     * Python: num = make_typecast(uinteger, expr). If is_number(num) and
     * the array scope is local/global, the dimension constant-propagates
     * (val==0 -> #dims; LBOUND -> bounds[val-1].lower; UBOUND -> .upper)
     * and NO flag is set. Otherwise (a non-constant dimension) it sets
     *     entry.ref.lbound_used = True   (LBOUND)
     *     entry.ref.ubound_used = True   (UBOUND)
     * which is exactly the gate VarTranslator.visit_ARRAYDECL
     * (var_translator.py:58/61) reads to emit the
     * `<mangled>.__LBOUND__` / `.__UBOUND__` descriptor slot + trailing
     * bound table for a non-zero-based global array (var_translator.c
     * :452-455 / :539-562 already consume these flags).
     *
     * Faithful narrow port: only the flag side effect is reproduced. The
     * constant-propagation fold of the result value (the LBOUND/UBOUND
     * runtime builtin itself) is a separate, not-yet-ported subsystem
     * (translator.c:1479); the C corpus's const-dim fixtures
     * (lbound0/1/3, bound00/01) reach byte-identical output through the
     * unused-LET DCE path and must stay untouched — so the flag is set
     * ONLY on the non-number dimension, exactly Python's else-branch. The
     * typecast is applied (as Python does) so check_is_number sees the
     * folded numeric form for a literal dimension. child[0] is the shared
     * array ID entry (parse_primary resolves a bare ARRAY_ID to the
     * symbol-table node — the same node the ARRAYDECL carries). */
    if ((kw == BTOK_LBOUND || kw == BTOK_UBOUND) && n->child_count >= 2) {
        AstNode *arr = n->children[0];
        if (arr && arr->tag == AST_ID &&
            arr->u.id.class_ == CLASS_array) {
            AstNode *dim = n->children[1];
            AstNode *num = make_typecast(
                p->cs,
                p->cs->symbol_table->basic_types[TYPE_uinteger],
                dim, lineno);
            if (num) n->children[1] = num;
            /* Python's const-prop branch additionally requires the array
             * scope to be local/global; a parameter-scope array always
             * takes the flag-setting else-branch. SCOPE_global / _local
             * arrays with a constant dimension fold (no flag). */
            bool const_dim =
                num && (arr->u.id.scope == SCOPE_global ||
                        arr->u.id.scope == SCOPE_local) &&
                check_is_number(num);
            if (!const_dim) {
                if (kw == BTOK_LBOUND)
                    arr->u.id.lbound_used = true;
                else
                    arr->u.id.ubound_used = true;
            }
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
            /* p_expr_usr (zxbparser.py:3272-3282): if the argument is a
             * string, the builtin is USR_STR (keep the STRING child, no
             * typecast); otherwise USR. Both yield TYPE.uinteger. The
             * else-branch's make_typecast(uinteger, arg) is pre-existing
             * absent in the C port (out of scope; left as-is). */
            n->type_ = st->basic_types[TYPE_uinteger];
            if (arg && arg->type_ &&
                type_is_string(arg->type_)) {
                n->u.builtin.fname =
                    arena_strdup(&p->cs->arena, "USR_STR");
            }
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
        case BTOK_LBOUND:
        case BTOK_UBOUND:
            /* LBOUND/UBOUND are always uinteger regardless of the array's
             * element type — Python zxbparser.py:3330/3332/3378
             * (make_builtin/make_number type_=TYPE.uinteger). Without this
             * they fall to default → arg->type_ (the element type). */
            n->type_ = st->basic_types[TYPE_uinteger];
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
        AstNode *entry = symboltable_access_id_noexplicit(
                             p->cs->symbol_table, p->cs, name, lineno,
                             NULL, CLASS_unknown);
        if (entry == NULL)
            return NULL;

        entry->u.id.has_address = true;          /* :2682 */
        /* mark_entry_as_accessed (zxbparser.py:167-174): in global scope
         * (or non-FUNCTION token) -> entry.accessed = True. For a label
         * the LabelRef.accessed setter cascades to scope_owner; model
         * that via mark_label_accessed. (A label may not yet be class_
         * == CLASS_label here — the later `<name>:` def's
         * label_capture_scope_owner re-fires the cascade.) */
        if (!(p->cs->function_level.len > 0 &&
              entry->u.id.class_ == CLASS_function))
            mark_label_accessed(entry);          /* :171 (+ cascade) */

        AstNode *n = make_unary_node(p->cs, "ADDRESS", entry, lineno);
        if (n)
            n->type_ = p->cs->symbol_table->basic_types[TYPE_uinteger];

        /* p[0] = result if is_dynamic(entry) else make_constexpr(result).
         * A global scalar of a basic non-string type (a LABEL's ref
         * type_ is PTR_TYPE==uinteger, scope global) is NOT dynamic ->
         * CONSTEXPR-wrap so it folds to the static label at translate
         * time (no runtime ld/push). */
        if (n && !check_is_dynamic(entry)) {
            AstNode *ce = ast_new(p->cs, AST_CONSTEXPR, lineno);
            ast_add_child(p->cs, ce, n);
            ce->type_ = n->type_;
            return ce;
        }
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
            /* Function with 0 args — treat as call (matching Python p_id_expr) */
            if (entry->u.id.class_ == CLASS_function) {
                AstNode *call = ast_new(p->cs, AST_FUNCCALL, lineno);
                AstNode *args = ast_new(p->cs, AST_ARGLIST, lineno);
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

/* Parse function call or array access: name(...) */
static AstNode *parse_call_or_array(Parser *p, const char *name, int lineno, bool expr_context, bool addressof_ctx) {
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
            type_is_string(entry->type_) && !check(p, BTOK_LP);

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

    if (entry && entry->u.id.class_ == CLASS_var && type_is_string(entry->type_)) {
        /* String slicing: name(expr).  S5.8a: STRSLICE-node construction
         * does NOT mark the string accessed in Python.  The substring
         * *lvalue* write-target productions p_substr_assignment_no_let
         * (zxbparser.py:1221-1245), p_substr_assignment (:1248-1305) and
         * p_str_assign (:1308-1335) resolve the target only via
         * SYMBOL_TABLE.access_call / access_var, and access_id /
         * access_var / access_call (src/api/symboltable/symboltable.py
         * :326 / :368 / :434) NEVER set .accessed — only the explicit
         * mark_entry_as_accessed (zxbparser.py:167) does, and none of the
         * three productions call it.  So a string used only as a
         * substring lvalue stays accessed=False in Python, and
         * OptimizerVisitor.visit_LETSUBSTR (optimize.py:360-365, O>1 +
         * not accessed → NOP) + VarTranslator.visit_VARDECL DCE prune it
         * to the bare wrapper + W150.  The C port's already-correct
         * ports of those gates (passes/optimizer.c opt_visit_letsubstr,
         * var_translator.c vt_visit_vardecl) were defeated only by this
         * branch unconditionally marking accessed at parse time.  Gate it
         * by expr_context exactly like the CLASS_array branch above
         * (true on every read/@-address path parser.c:517,555; false on
         * the substring-lvalue write target) so a write-only string's
         * accessed is False — byte-identical to Python.  Resolves R2
         * (lvalsubstr_nolet / opt2_letsubstr_not_used / sys_letsubstr0);
         * STRSLICE-node-local, mirrors the S5.6 array-gate precedent. */
        AstNode *n = ast_new(p->cs, AST_STRSLICE, lineno);
        if (expr_context)
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
            /* Array access or function call on expression result —
             * always a READ here (postfix on an expression value) =>
             * Python sym.ARRAYLOAD. */
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
static AstNode *parse_sub_or_func_decl(Parser *p, bool is_function, bool is_declare);

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
        /* symboltable.access_label scope_owner capture
         * (symboltable.py:621-623): if gl.FUNCTION_LEVEL,
         * entry.ref.scope_owner = list(gl.FUNCTION_LEVEL). The
         * LabelRef.scope_owner setter (labelref.py:42-45) refreshes
         * `accessed` — so a label DEFINED inside a SUB whose address was
         * already taken (`@label` earlier -> entry.accessed) now
         * cascades accessed onto the enclosing SUB(s), keeping them from
         * O>1 prune.  Order-independent: if `@label` comes AFTER the
         * def, mark_label_accessed there walks this same scope_owner. */
        if (label_node)
            label_capture_scope_owner(p->cs, label_node);
        /* S5.8d — make_label's DATA_LABELS write (zxbparser.py:457):
         * EVERY declared label records data_labels[id]=data_ptr_current
         * ("This label points to the current DATA block index"); how
         * RESTORE label resolves (visit_RESTORE:491). Strictly additive
         * to the existing p_label path; no AST/parse-surface effect. */
        if (label_node)
            hashmap_set(&p->cs->data_labels, label_text,
                        p->cs->data_ptr_current ? p->cs->data_ptr_current
                                                : "");

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
         * (The error itself matches Python p_data:1743; the C parser
         * already emitted it pre-S5.8d — keep that behaviour but follow
         * Python's early return: no DATAS append inside a function.) */
        if (p->cs->function_level.len > 0) {
            zxbc_error(p->cs, ln,
                       "DATA not allowed within Functions nor Subs");
            return NULL;
        }

        /* :1747-1768  per-child static/FUNCPTR split. */
        DataItem *items = arena_alloc(&p->cs->arena,
            (size_t)s->child_count * sizeof(DataItem));
        int nitems = 0;
        for (int ci = 0; ci < s->child_count; ci++) {
            AstNode *value = s->children[ci];

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

            /* :1754-1756  make_func_declaration(new_lbl, lineno,
             *   type_=value.type_, class_=CLASS.function).
             * declare_func on a fresh unique name -> a new FUNCTION
             * entry; symboltable_declare is the faithful generic creator
             * (mangled = "_"+name == "___DATA__FUNCPTR__N", scope global
             * since DATA is top-level only). */
            AstNode *func =
                symboltable_declare(p->cs->symbol_table, p->cs,
                                    fnbuf, ln, CLASS_function);
            if (!func)
                continue;
            func->u.id.declared = true;          /* FUNCDECL.make_node */
            func->type_ = value->type_;          /* entry.type_ = value.type_ */

            /* :1759  func.ref.convention = CONVENTION.fastcall */
            func->u.id.convention = CONV_fastcall;
            /* :1760-1762  empty local scope -> locals_size 0, no locals. */
            func->u.id.local_size = 0;
            func->u.id.param_size = 0;
            func->u.id.local_entries = NULL;
            func->u.id.local_entries_count = 0;

            /* :1765-1766  sent = make_sentence("RETURN", func, value);
             *             func.ref.body = make_block(sent)
             * tr_visit_return's len==2 path reads child[0]=func ref
             * (.mangled -> "<mangled>__leave") and child[1]=value. */
            AstNode *ret = make_sentence_node(p, "RETURN", ln);
            ast_add_child(p->cs, ret, func);
            ast_add_child(p->cs, ret, value);
            AstNode *body = make_block_node(p, ln);
            ast_add_child(p->cs, body, ret);

            /* The FUNCDECL carrier the C FunctionTranslator drains
             * (child[0]=ID, child[1]=PARAMLIST, child[2]=body), exactly
             * the shape parse_sub_or_func_decl produces. */
            AstNode *decl = ast_new(p->cs, AST_FUNCDECL, ln);
            AstNode *params = ast_new(p->cs, AST_PARAMLIST, ln);
            ast_add_child(p->cs, decl, func);
            ast_add_child(p->cs, decl, params);
            ast_add_child(p->cs, decl, body);
            decl->type_ = value->type_;
            func->u.id.body = body;

            /* :1764  gl.DATA_FUNCTIONS.append(func) — the carrier the
             * codegen merge feeds to FunctionTranslator. */
            vec_push(p->cs->data_functions, decl);

            /* :1767  datas_.append(entry) — the FUNCDECL is the item. */
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

        return NULL;
    }

    /* READ */
    if (match(p, BTOK_READ)) {
        int ln = p->previous.lineno;
        p->cs->data_is_used = true;  /* Track that READ is used (matches Python) */
        AstNode *block = make_block_node(p, ln);
        do {
            AstNode *s = make_sentence_node(p, "READ", ln);
            AstNode *target = parse_expression(p, PREC_NONE + 1);
            if (target) {
                /* Validate READ target is a variable or array element */
                if (target->tag == AST_ID && target->u.id.class_ == CLASS_array) {
                    zxbc_error(p->cs, ln, "Cannot read '%s'. It's an array", target->u.id.name);
                } else if (target->tag != AST_ID && target->tag != AST_ARRAYACCESS) {
                    zxbc_error(p->cs, ln, "Syntax error. Can only read a variable or an array element");
                }
                ast_add_child(p->cs, s, target);
            }
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
        /* Skip the required library name for now */
        if (check(p, BTOK_STRC)) advance(p);
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

            if (raw != NULL) {
                /* --- Type tables: EXACT Python-registered names --------- *
                 * bool options: config.py EXPLICIT/STRICT/STRICT_BOOL/
                 *   CHECK_MEMORY/CHECK_ARRAYS/ENABLE_BREAK/CASE_INS/
                 *   DEFAULT_BYREF/USE_BASIC_LOADER/AUTORUN/FORCE_ASM_BRACKET/
                 *   ASM_ZXNEXT/EMIT_BACKEND/HIDE_WARNING_CODES, "sinclair";
                 *   backend "headerless".
                 * int options: config.py DEBUG(debug_level)/O_LEVEL/
                 *   ARRAY_BASE/STR_BASE/MAX_SYN_ERRORS/EXPECTED_WARNINGS;
                 *   backend org/heap_size/heap_address.
                 * str options: config.py MEMORY_MAP/OUTPUT_FILE_TYPE/
                 *   INCLUDE_PATH/ARCH/OUTPUT_FILENAME/INPUT_FILENAME/
                 *   STDERR_FILENAME/PROJECT_FILENAME.
                 * Registered-but-no-C-field (heap_*_label str, opt_strategy,
                 * stdin/out/err, __DEFINES dict): still KNOWN to Python, so
                 * NOT an unknown pragma — accept without warning; no owned
                 * fixture sets them so there is no observable asm effect. */
                #define NAME_IS(s) (strcmp(opt_name, (s)) == 0)

                /* Python bool coercion of a str (options.py:121-131),
                 * value.lower() keyed. Returns 1/0, or -1 if not a key
                 * (KeyError -> InvalidValueError path). */
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
                /* Registered, known to Python, but no observable C field:
                 * accept silently (NOT an unknown pragma). */
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
                    /* Python int(str): all-decimal-digits per the INTEGER
                     * regex; a non-numeric str -> ValueError -> uncaught
                     * InvalidValueError -> exit 1.  parse_int mirrors the
                     * 'org' config arm (args.c:103). */
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
                 * (parser.c:978-983) so ast-dump stays identical. */
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
                if (expr) {
                    /* Faithful port of the const-vector element reject in
                     * src/zxbc/zxbparser.py p_const_vector_elem_list:891
                     * (first elem) and p_const_vector_elem_list_list:913
                     * (subsequent elems): for each element e,
                     *     if not is_static(e):
                     *         if isinstance(e, sym.UNARY): make_constexpr(...)
                     *         else: errmsg.syntax_error_not_constant(...)
                     * i.e. a non-static, non-UNARY element ->
                     * "Initializer expression is not constant."  The C
                     * do/while collects BOTH the first and subsequent
                     * elements, so this single per-element check mirrors
                     * both Python productions.
                     *
                     * CONFINEMENT (S5.10d): narrowed to the only
                     * C-modellable shape that hits Python's exact reject
                     * set without the upstream p_addr_of_id:2683
                     * is_dynamic-conditional CONSTEXPR-wrap the C parser
                     * does NOT model — a *bare AST_ID resolving to a
                     * CLASS_var runtime variable*.  check_is_static is
                     * Python's is_static analogue (CONSTEXPR/NUMBER/CONST);
                     * a CLASS_const id is is_static-true via check_is_const
                     * so dim_const0's {xx,xx} is NOT rejected (stays rc=0
                     * like Python).  The check deliberately does NOT touch
                     * AST_UNARY (Python's make_constexpr branch) nor
                     * AST_BINARY (the @a+1 shape) — arrlabels10/2/3/10b are
                     * Python-ACCEPTED there and a broad is_static port
                     * would FALSE_POS them (the proven S5.6 over-reach).
                     * Lineno: Python emits at p.lexer.lineno / p.lineno(2)
                     * == the const-vector's line == init->lineno (the
                     * '{'); array11 -> line 4, matching the .err oracle. */
                    if (!check_is_static(expr)
                        && expr->tag == AST_ID
                        && expr->u.id.class_ == CLASS_var) {
                        err_not_constant(p->cs, init->lineno);
                    }
                    ast_add_child(p->cs, init, expr);
                }
            }
        } while (match(p, BTOK_COMMA));
    }
    consume(p, BTOK_RBRACE, "Expected '}'");
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
        if (match(p, BTOK_RIGHTARROW)) {
            init = parse_array_initializer(p);
        } else if (match(p, BTOK_EQ)) {
            if (check(p, BTOK_LBRACE)) {
                init = parse_array_initializer(p);
            } else {
                init = parse_expression(p, PREC_NONE + 1);
            }
        }

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
        /* declare_array reconciliation (symboltable.py:677-... -> to_vararray,
         * _id.py:165): a pre-existing CLASS_unknown entry — e.g. one
         * implicitly created by a forward `@a` (p_addr_of_id's access_id,
         * default_class=CLASS.unknown) appearing in this DIM's own
         * initializer `{@a, ...}` — is converted to the array here.
         * symboltable_declare returns the shared entry as-is; without
         * this it stays CLASS_unknown and a later `a(i)` access fails
         * with "neither an array nor a function". (Previously the
         * @-operand was a detached node so no entry pre-existed; the
         * faithful shared-entry @-path now requires this Python step.) */
        if (id_node->u.id.class_ == CLASS_unknown)
            id_node->u.id.class_ = CLASS_array;
        id_node->type_ = type;
        /* Port C support — the declared bound list is the faithful
         * analogue of Python entry.bounds (symboltable.declare_array),
         * read by arrayaccess.make_node's dim-count check at every
         * array-access site.  symboltable_access_array returns this
         * same shared symbol node, so stamp the boundlist on the entry
         * for ALL arrays (the SCOPE_local block below additionally
         * needs arr_init/is_zero_based; this stamp is strictly additive
         * u.id metadata — not serialised by zxbc-ast-dump). */
        id_node->u.id.arr_boundlist = bounds;
        ast_add_child(p->cs, decl, id_node);
        ast_add_child(p->cs, decl, bounds);
        if (arr_at_expr) ast_add_child(p->cs, decl, arr_at_expr);
        if (init) ast_add_child(p->cs, decl, init);
        decl->type_ = type;
        /* S5.7d — a LOCAL array (inside a SUB/FUNCTION) is allocated on
         * the stack frame; the FunctionTranslator :58-116 walk needs its
         * geometry (bounds + init image + is_zero_based). The global
         * VarTranslator path keeps reading these off the ARRAYDECL node
         * (unchanged); stamp them on the ID only for the local case so
         * compute_offsets' memsize (arrayref.py:230-233) and the
         * :68-100 ic_larrd branch can run after the parse-time scope pop.
         * is_dynamically_accessed / lbound_used / ubound_used stay false
         * for the zero-based numeric corpus, exactly as the global path
         * (var_translator.c:459-463). */
        if (id_node->u.id.scope == SCOPE_local) {
            /* arr_boundlist already stamped above for all arrays. */
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
        if (init_expr) {
            /* p_var_decl_ini (zxbparser.py:707-711):
             *   value  = make_typecast(typedef, expr)
             *   defval = value if is_static(expr) and value.type_ != string
             *   declare_variable(..., default_value=defval)
             * is_static() tests the ORIGINAL expr; defval is the cast value.
             * (Implicit-typedef→expr.type_ already applied above, :2215.) */
            AstNode *value = make_typecast(p->cs, type, init_expr, lineno);
            bool stat = check_is_static(init_expr);
            bool is_str = value && type_is_string(value->type_);
            if (stat && !is_str)
                id_node->u.id.default_value_expr = value;
        }
        if (at_expr) {
            /* p_var_decl_at (zxbparser.py:679):
             *   entry.addr = make_typecast(PTR_TYPE, expr)  (static expr)
             * S5.3 corpus is initializer-only; AT recorded faithfully via
             * the same data_ast path (entry.addr drives ic_deflabel). */
            TypeInfo *ptr_t =
                p->cs->symbol_table->basic_types[TYPE_uinteger]; /* gl.PTR_TYPE */
            AstNode *av = make_typecast(p->cs, ptr_t, at_expr, lineno);
            id_node->u.id.addr_expr = av;
            id_node->u.id.accessed = true; /* Python mark_entry_as_accessed */
        }
        return NULL;
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
                    had_optional_param = true;
                } else if (had_optional_param) {
                    zxbc_error(p->cs, param_line,
                               "Can't declare mandatory param '%s' after optional param", param_name);
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
    }

    /* Declare function/sub in the CURRENT (parent) scope BEFORE entering body scope.
     * This enables recursive calls — the function name is visible from inside. */
    SymbolClass cls = is_function ? CLASS_function : CLASS_sub;
    AstNode *id_node = symboltable_declare(p->cs->symbol_table, p->cs, func_name, lineno, cls);

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
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Full definition — clear forwarded flag if this was previously declared */
    id_node->u.id.forwarded = false;

    /* Enter function body scope */
    symboltable_enter_scope(p->cs->symbol_table, p->cs);

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
            symboltable_compute_offsets(p->cs->symbol_table, body_scope);
    }

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
