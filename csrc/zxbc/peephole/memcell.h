/*
 * memcell.h — Port of src/arch/z80/optimizer/memcell.py (MemCell), the
 * subset the peephole NEEDS / CTEST / IS_LABEL predicates require:
 *
 *   MemCell.__init__        (addr + Asm)
 *   MemCell.code / inst / is_label / condition_flag / opers
 *   MemCell.requires        (FULL register-analysis, memcell.py:184-321,
 *                            every branch ported, not just the _end hits)
 *   MemCell.needs(reglist)  (single_registers ∩ requires)
 *
 * `self.code in ASMS` — ASMS is the user-##ASM-block table; instructions
 * the peephole evaluates are never members (they are ordinary emitted
 * asm), so the check is faithfully reproduced as "false" with a clear
 * note (no peephole path inserts into ASMS). destroys/affects/bytes/
 * sizeof/used_labels/replace_label are NOT ported: no peephole predicate
 * reaches them.
 */
#ifndef ZXBC_PEEPHOLE_MEMCELL_H
#define ZXBC_PEEPHOLE_MEMCELL_H

#include <stdbool.h>
#include "arena.h"
#include "z80asm.h"

typedef struct MemCell {
    int      addr;
    Z80Asm  *asm_;        /* self.asm */
    /* cached_property analogues (computed at construction, faithful to
     * the cached_property compute — single instruction, immutable). */
    char       *inst;     /* MemCell.inst */
    bool        is_label; /* MemCell.is_label */
    char       *cond;     /* MemCell.condition_flag (NULL == None) */
    Z80StrList  requires_; /* MemCell.requires (sorted set) */
} MemCell;

MemCell *memcell_new(Arena *a, const char *instr, int addr);

/* MemCell.needs(reglist): single_registers(reglist) ∩ requires. */
bool memcell_needs(Arena *a, const MemCell *c, const Z80StrList *reglist);

#endif /* ZXBC_PEEPHOLE_MEMCELL_H */
