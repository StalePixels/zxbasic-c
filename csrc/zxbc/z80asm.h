/*
 * z80asm.h — Port of the Z80 optimizer's Asm class + helpers register vocab.
 *
 * Faithful port of:
 *   src/arch/z80/optimizer/asm.py      — class Asm (instruction/opers/
 *                                         condition/is_label, synthetic
 *                                         operand rules push/pop/call->sp,
 *                                         exx->*,bc,de,hl,..., etc.)
 *   src/arch/z80/optimizer/helpers.py  — single_registers, the 8/16-bit
 *                                         register predicates, LO16/HI16,
 *                                         LO16_val/HI16_val, new_tmp_val
 *                                         (stateful counter), is_mem_access,
 *                                         is_number, valnum
 *   src/api/utils.py:parse_int         — reused via common/utils.h parse_int
 *   src/arch/z80/backend/common.py:is_int        — Asm.is_int analogue
 *   src/arch/z80/backend/_16bit.py:Bits16.int16  — int16
 *
 * Shared by the peephole NEEDS/IS_LABEL/OP1/OP2/INSTR predicates (via
 * memcell.c) and (later) the emitter's remove_unused_labels / Bits16.get_oper.
 *
 * Memory model: results that the Python caches on the instance are computed
 * here into an arena-owned Asm. The "*" cdr split + the per-instruction
 * synthetic operands are reproduced exactly (verified against live Python).
 */
#ifndef ZXBC_Z80ASM_H
#define ZXBC_Z80ASM_H

#include <stdbool.h>
#include "arena.h"
#include "vec.h"

typedef VEC(char *) Z80StrList;

/* Port of optimizer.asm.Asm. Slots: inst, oper, asm, cond, is_label.
 * (bytes / max_tstates / result are NOT ported: the peephole subsystem
 * never reads them — only .inst/.oper/.asm/.cond/.is_label are used via
 * MemCell. Their absence is documented, not stubbed: no peephole code
 * path reaches them.) */
typedef struct Z80Asm {
    char       *inst;      /* Asm.instruction(asm) — lowercased iff a known mnemonic */
    Z80StrList  oper;      /* Asm.opers(asm) — incl. synthetic operands */
    char       *asm_;      /* Asm.asm — "{inst} {rest}".strip() */
    char       *cond;      /* Asm.condition(asm) — NULL == Python None */
    bool        is_label;  /* asm.inst[-1] == ':' */
} Z80Asm;

/* Asm(asm_str): asserts non-empty (matches Python `assert asm`). */
Z80Asm *z80asm_new(Arena *a, const char *asm_str);

/* Static-method ports (operate on a raw string, arena for any allocation). */
char       *z80asm_instruction(Arena *a, const char *asm_str);
Z80StrList  z80asm_opers(Arena *a, const char *asm_str);   /* heap VEC of arena strs */
char       *z80asm_condition(Arena *a, const char *asm_str); /* NULL == None */

/* helpers.py register predicates (case-insensitive, exactly as Python). */
bool z80h_is_8bit_normal_register(const char *x);
bool z80h_is_8bit_idx_register(const char *x);
bool z80h_is_8bit_oper_register(const char *x);
bool z80h_is_16bit_normal_register(const char *x);
bool z80h_is_16bit_idx_register(const char *x);
bool z80h_is_16bit_composed_register(const char *x);
bool z80h_is_16bit_oper_register(const char *x);

/* helpers.LO16 / HI16. Assert (abort) on sp / non-16bit, matching Python
 * `assert`. Returned strings are arena-owned. */
const char *z80h_LO16(Arena *a, const char *x);
const char *z80h_HI16(Arena *a, const char *x);

/* helpers.single_registers(op): sorted unique list of single registers.
 * `op` is a list of tokens. Result is a freshly arena-built sorted list
 * (heap VEC, arena strings). */
Z80StrList z80h_single_registers(Arena *a, const Z80StrList *op);
/* Convenience for the common single-token call. */
Z80StrList z80h_single_registers1(Arena *a, const char *tok);

/* helpers.new_tmp_val() — stateful global counter ("*UNKNOWN_<n>").
 * Faithful: shares the module counter with helpers.init() reset. */
char *z80h_new_tmp_val(Arena *a);
void  z80h_helpers_init(void);  /* helpers.init(): _RAND_COUNT = 0 */

/* helpers.LO16_val / HI16_val (int|str|None semantics; None == NULL in). */
char *z80h_LO16_val(Arena *a, const char *x);
char *z80h_HI16_val(Arena *a, const char *x);

/* helpers.is_mem_access / is_number / valnum (faithful for the operand
 * tokens the peephole/requires path produces — see z80asm.c notes). */
bool z80h_is_mem_access(const char *arg);
bool z80h_is_number(const char *x);
/* valnum: writes *out and returns true; returns false == Python None. */
bool z80h_valnum(const char *x, long *out);

/* helpers.is_unknown (needed by LO16_val/HI16_val). */
bool z80h_is_unknown(const char *x);

/* backend.common.is_int(op): True iff Python int(op) succeeds. */
bool z80h_is_int(const char *op);
/* Bits16.int16(op): int(op) & 0xFFFF (Python int() parse). */
bool z80h_int16(const char *op, int *out);
/* backend.common.is_float(op): True iff Python float(op) succeeds. */
bool z80h_is_float(const char *op);
/* float(op): Python float() value (caller ensures z80h_is_float). */
bool z80h_float(const char *op, double *out);
/* Bits32.int32(op): (int(op) & 0xFFFFFFFF) -> *de=hi16, *hl=lo16. */
bool z80h_int32(const char *op, unsigned *de, unsigned *hl);
/* src/api/fp.py immediate_float(x): the ZX 40-bit FP (C, ED, LH) hex
 * operand triple ("0XXh"/"0XXXXh"). Each buffer must hold >= 8 bytes. */
void z80h_immediate_float(double x, char *C, char *ED, char *LH);
/* Python's repr/str(float) — shortest decimal that round-trips through
 * float(). Buffer must hold >= 32 bytes. */
void z80h_pyfloat_repr(double v, char *buf, int sz);

#endif /* ZXBC_Z80ASM_H */
