/*
 * IC substrate implementation. See ic.h.
 *
 * Port of interface/quad.py Quad.__init__:
 *     args = tuple(str(x.t) if isinstance(x, Symbol) else str(x) for x in args)
 * The C call sites pass already-stringified operands (AstNode.t, or a
 * literal), so the coercion reduces to a copy; a NULL operand becomes "".
 */
#include "ic.h"

#include <stddef.h>

Quad *quad_new(Arena *a, const char *instr, int nargs, const char *const *args) {
    Quad *q = arena_alloc(a, sizeof(Quad));
    q->instr = arena_strdup(a, instr); /* instr may be computed (e.g. "storeu8") */
    q->nargs = nargs;
    if (nargs > 0) {
        q->args = arena_alloc(a, (size_t)nargs * sizeof(char *));
        for (int i = 0; i < nargs; i++)
            q->args[i] = arena_strdup(a, (args && args[i]) ? args[i] : "");
    } else {
        q->args = NULL;
    }
    return q;
}
