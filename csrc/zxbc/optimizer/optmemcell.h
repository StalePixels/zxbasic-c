/*
 * optmemcell.h — Full port of src/arch/z80/optimizer/memcell.py (MemCell),
 * the surface the O3 BasicBlock / flow-graph / CPUState path needs:
 *
 *   __init__ / asm setter        (memcell.py:27-41)
 *   code / inst / is_label       (memcell.py:43-91)
 *   is_ender                     (memcell.py:79-81)
 *   condition_flag               (memcell.py:93-101)
 *   opers                        (memcell.py:103-106)
 *   branch_arg                   (memcell.py:108-113)
 *   destroys                     (memcell.py:115-182)
 *   requires                     (memcell.py:184-321 — reuses the exact
 *                                  branch-for-branch logic already proven
 *                                  in peephole/memcell.c via z80asm.c)
 *   used_labels                  (memcell.py:343-366 — asmlex ID scan)
 *   replace_label                (memcell.py:368-373 — \bword\b re.sub)
 *
 * bytes / sizeof / max_tstates are reached ONLY by main.py:218's
 * __DEBUG__(..., 1) (debug_level default 0 — never emitted; the codegen
 * meter never sets it). They are documented as debug-only and not
 * computed — exactly Python's observable behaviour at the default
 * debug level (no asm-output effect). The cache-invalidation hooks that
 * Python ties to them (_bytes/_sizeof/_max_tstates = None) therefore
 * have no observable counterpart and are intentionally absent.
 */
#ifndef ZXBC_OPT_MEMCELL_H
#define ZXBC_OPT_MEMCELL_H

#include <stdbool.h>
#include "arena.h"
#include "z80asm.h"

typedef struct OMemCell {
    int         addr;
    Z80Asm     *asm_;       /* self.asm */
    char       *inst;       /* MemCell.inst (label-stripped) */
    bool        is_label;   /* MemCell.is_label */
    bool        is_ender;   /* inst in BLOCK_ENDERS */
    char       *cond;       /* condition_flag (NULL == None) */
    Z80StrList  opers;      /* MemCell.opers */
    char       *branch_arg; /* MemCell.branch_arg (NULL == None) */
    Z80StrList  requires_;  /* MemCell.requires */
    Z80StrList  destroys_;  /* MemCell.destroys */
    Arena      *a;          /* owning arena (for replace_label rebuild) */
} OMemCell;

OMemCell *omemcell_new(Arena *a, const char *instr, int addr);

/* MemCell.code — self.__instr.asm */
#define omemcell_code(c) ((c)->asm_->asm_)

/* MemCell.used_labels — asmlex ID tokens (memcell.py:343-366). */
Z80StrList omemcell_used_labels(Arena *a, const OMemCell *c);

/* MemCell.replace_label(old,new): re.sub(r"\bold\b", new, code); rebuild
 * self.asm = Asm(...). Mutates the cell in place (Python sets self.asm,
 * which re-derives inst/opers/cond — we rebuild the whole cell's
 * derived fields, faithful to the Asm setter). */
void omemcell_replace_label(OMemCell *c, const char *old_label,
                            const char *new_label);

/* Set self.asm = Asm(s) and re-derive (the Python `cell.asm = "..."`
 * setter path used by cleanup_local_labels / the jump-over-jump rewrite). */
void omemcell_set_asm(OMemCell *c, const char *s);

#endif /* ZXBC_OPT_MEMCELL_H */
