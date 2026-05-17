/*
 * cpustate.h — Port of src/arch/z80/optimizer/cpustate.py
 *
 *   Flags                (cpustate.py:42-47)
 *   Memory               (UserDict[str,str] + __missing__ auto-unknown,
 *                          read/write 8/16-bit, _get_hl_addr) :50-125
 *   CPUState             (regs/stack/mem/_flags/ix_ptr; the C/Z/P/S
 *                          property setters; reset; set/get/getv/eq/_is;
 *                          inc/dec/rr/rl/rrc/rlc; set_flag; execute) :128-896
 *
 * Tri-state flag model: Python `None | 0 | 1`. None is the "unknown"
 * sentinel; the setters assert is_unknown(val) or val in (0,1). Modelled
 * as `int` with FLAG_NONE == -1. `regs`/`mem` are insertion-ordered
 * (OMap) because Python dict order is observable (reset key order,
 * Memory.__missing__ insertion, dict_intersection).
 */
#ifndef ZXBC_OPT_CPUSTATE_H
#define ZXBC_OPT_CPUSTATE_H

#include <stdbool.h>
#include "arena.h"
#include "omap.h"
#include "z80asm.h"   /* Z80StrList */

#define FLAG_NONE (-1)

typedef struct CPUFlags {
    int C, Z, P, S;   /* FLAG_NONE | 0 | 1 */
} CPUFlags;

/* ix_ptr: a set of (reg, sign, offset) triples (idx_args results). */
typedef struct IxPtr {
    char *reg, *sign, *off;
} IxPtr;
typedef VEC(IxPtr) IxPtrVec;

typedef struct CPUState {
    Arena    *a;
    OMap      regs;       /* dict[str,str] */
    Z80StrList stack;     /* list[str] */
    OMap      mem;        /* Memory: dict[str,str] (+ __missing__) */
    CPUFlags  flags0;     /* _flags[0] (the live flags) */
    CPUFlags  flags1;     /* _flags[1] (never read by ported paths) */
    IxPtrVec  ix_ptr;
} CPUState;

CPUState *cpustate_new(Arena *a);

/* reset(regs=None, mems=None). regs/mems may be NULL (== {}). */
void cpustate_reset(CPUState *s, const OMap *regs, const OMap *mems);
void cpustate_reset_flags(CPUState *s);

/* execute(asm_code): the big dispatch (cpustate.py:643-893). */
void cpustate_execute(CPUState *s, const char *asm_code);

/* get/getv (cpustate.py:437-482). get returns NULL == Python None.
 * getv: returns true + *out on int, false == Python None. */
const char *cpustate_get(CPUState *s, const char *r);
bool cpustate_getv(CPUState *s, const char *r, long *out);

/* flag accessors (the C/Z property getters used by basicblock.optimize
 * monkey-patch FLAGVAL). */
int  cpustate_C(const CPUState *s);
int  cpustate_Z(const CPUState *s);

#endif /* ZXBC_OPT_CPUSTATE_H */
