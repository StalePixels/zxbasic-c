/*
 * IC (intermediate code) substrate — the Quad 3-address unit.
 *
 * Port of src/arch/interface/quad.py (the Quad dataclass) and the
 * string-valued opcode names from src/arch/z80/backend/icinstruction.py.
 *
 * Faithfulness note: Python's ICInstruction is a *string subclass*, not an
 * Enum ("Do not use Enums here. They cannot be subclassed" —
 * icinstruction.py). Backend._QUAD_TABLE is keyed by that string value and
 * quad.instr is looked up directly. The C port mirrors that: an opcode is a
 * string, a Quad carries the opcode string plus string-coerced args.
 *
 * The opcode vocabulary grows per codegen sprint as each emitter lands;
 * S5.2 (the foundation/calibration slice) only emits "end" and "inline".
 */
#ifndef ZXBC_IC_H
#define ZXBC_IC_H

#include "arena.h"
#include "vec.h"

/* IC opcode strings. Add more as later S5.x emitters port them. */
#define IC_END    "end"     /* icinstruction.py:233  END    */
#define IC_INLINE "inline"  /* icinstruction.py:238  INLINE */
#define IC_VAR    "var"     /* icinstruction.py:328  VAR    */

/*
 * A Quad: one 3-address IC instruction. Mirrors interface/quad.py:
 *   instr: ICInstruction          (opcode string)
 *   args:  tuple[str, ...]        (each arg str()-coerced at construction)
 * Arena-owned — no individual free (matches the AST/compiler convention).
 */
typedef struct Quad {
    const char *instr; /* arena-copied opcode string */
    char      **args;  /* nargs arena-copied strings (NULL if nargs == 0) */
    int         nargs;
} Quad;

/* MEMORY: the ordered Quad stream. Python Backend.MEMORY: list[Quad]. */
typedef VEC(Quad *) QuadVec;

/*
 * Construct a Quad, copying instr and every arg into the arena. Python's
 * Quad.__init__ str()-coerces each arg (using .t for Symbols); C call sites
 * already pass strings (an AstNode.t, or a literal), so a NULL arg is
 * normalised to "" rather than coerced.
 */
Quad *quad_new(Arena *a, const char *instr, int nargs, const char *const *args);

#endif /* ZXBC_IC_H */
