/*
 * Expression tree: creation and evaluation.
 * Mirrors src/zxbasm/expr.py
 */
#include "zxbasm.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

Expr *expr_int(AsmState *as, int64_t val, int lineno)
{
    Expr *e = arena_alloc(&as->arena, sizeof(Expr));
    e->kind = EXPR_INT;
    e->lineno = lineno;
    e->u.ival = val;
    return e;
}

Expr *expr_label(AsmState *as, Label *lbl, int lineno)
{
    Expr *e = arena_alloc(&as->arena, sizeof(Expr));
    e->kind = EXPR_LABEL;
    e->lineno = lineno;
    e->u.label = lbl;
    return e;
}

Expr *expr_unary(AsmState *as, char op, Expr *operand, int lineno)
{
    Expr *e = arena_alloc(&as->arena, sizeof(Expr));
    e->kind = EXPR_UNARY;
    e->lineno = lineno;
    e->u.unary.op = op;
    e->u.unary.operand = operand;
    return e;
}

Expr *expr_binary(AsmState *as, int op, Expr *left, Expr *right, int lineno)
{
    Expr *e = arena_alloc(&as->arena, sizeof(Expr));
    e->kind = EXPR_BINARY;
    e->lineno = lineno;
    e->u.binary.op = op;
    e->u.binary.left = left;
    e->u.binary.right = right;
    return e;
}

/* Internal evaluation. Returns true if resolved. */
static bool eval_impl(AsmState *as, Expr *e, int64_t *result, bool ignore)
{
    if (!e) return false;

    switch (e->kind) {
    case EXPR_INT:
        *result = e->u.ival;
        return true;

    case EXPR_LABEL: {
        Label *lbl = e->u.label;
        if (lbl->defined) {
            /* Mirror expr.py:88-89: if isinstance(item.value, Expr):
             *   return item.value.try_eval()
             * A label whose value is itself an Expr (a deferred forward
             * EQU) recurses into that Expr — yielding its value when
             * resolvable and DEFERRING (false) when not, never 0.
             * Pass `ignore` through unchanged: Python's Expr.ignore is a
             * single class flag shared by the recursive try_eval calls. */
            if (lbl->value_expr)
                return eval_impl(as, lbl->value_expr, result, ignore);
            *result = lbl->value;
            return true;
        }
        if (!ignore) {
            asm_error(as, e->lineno, "Undefined label '%s'", lbl->name);
        }
        return false;
    }

    case EXPR_UNARY: {
        int64_t v;
        if (!eval_impl(as, e->u.unary.operand, &v, ignore))
            return false;
        if (e->u.unary.op == '-')
            *result = -v;
        else
            *result = v;
        return true;
    }

    case EXPR_BINARY: {
        int64_t l, r;
        if (!eval_impl(as, e->u.binary.left, &l, ignore))
            return false;
        if (!eval_impl(as, e->u.binary.right, &r, ignore))
            return false;

        switch (e->u.binary.op) {
        case '+': *result = l + r; break;
        case '-': *result = l - r; break;
        case '*': *result = l * r; break;
        case '/':
            if (r == 0) {
                /* Python's Expr.try_eval (src/zxbasm/expr.py:111-114)
                 * unconditionally emits "Division by 0" on ZeroDivisionError
                 * — Expr.ignore gates label lookups, NOT arithmetic.
                 * Mirror that: report even from the try-eval (ignore=true)
                 * path so `LD A, 10/0` fails the same way Python does. */
                asm_error(as, e->lineno, "Division by 0");
                return false;
            }
            /* Python-style integer division: floor division */
            if ((l < 0) != (r < 0) && l % r != 0)
                *result = l / r - 1;
            else
                *result = l / r;
            break;
        case '%':
            if (r == 0) {
                asm_error(as, e->lineno, "Division by 0");
                return false;
            }
            *result = l % r;
            /* Python-style modulo: result has sign of divisor */
            if (*result != 0 && ((*result < 0) != (r < 0)))
                *result += r;
            break;
        case '^': {
            /* Integer power, matching Python's ** */
            int64_t base = l;
            int64_t exp = r;
            if (exp < 0) {
                *result = 0; /* integer division: x**(-n) = 0 for |x|>1 */
                return true;
            }
            int64_t res = 1;
            while (exp > 0) {
                if (exp & 1) res *= base;
                base *= base;
                exp >>= 1;
            }
            *result = res;
            break;
        }
        case '&': *result = l & r; break;
        case '|': *result = l | r; break;
        case '~': *result = l ^ r; break;  /* XOR in this assembler */
        case EXPR_OP_LSHIFT: *result = l << r; break;
        case EXPR_OP_RSHIFT: *result = l >> r; break;
        default:
            return false;
        }
        return true;
    }
    }

    return false;
}

bool expr_eval(AsmState *as, Expr *e, int64_t *result, bool ignore_errors)
{
    return eval_impl(as, e, result, ignore_errors);
}

bool expr_try_eval(AsmState *as, Expr *e, int64_t *result)
{
    return eval_impl(as, e, result, true);
}
