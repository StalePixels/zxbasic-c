/*
 * opthelpers.h — Port of the src/arch/z80/optimizer/helpers.py functions
 * the O3 path (cpustate / full MemCell / basicblock) needs and that are
 * NOT already covered by csrc/zxbc/z80asm.{c,h}.
 *
 * Already in z80asm.{c,h} (reused, not re-ported): new_tmp_val,
 * helpers_init, is_unknown, is_mem_access, is_number, valnum,
 * single_registers, LO16/HI16, LO16_val/HI16_val, the 8/16-bit register
 * predicates. This file adds the remaining helpers.py surface:
 *   helpers.py:124-131  new_tmp_val16 / new_tmp_val16_from_label
 *   helpers.py:149-166  is_unknown8 / is_unknown16
 *   helpers.py:169-211  get_orig_label_from_unknown16 / get_L/H_from_unknown
 *   helpers.py:247-248  is_label
 *   helpers.py:276-285  to_int
 *   helpers.py:288-332  simplify_arg / simplify_asm_args
 *   helpers.py:444-460  idx_args  (patterns.RE_IDX)
 *   helpers.py:496-505  dict_intersection (over OMap)
 *   cpustate.py:39      RE_OFFSET matcher
 */
#ifndef ZXBC_OPT_HELPERS_H
#define ZXBC_OPT_HELPERS_H

#include <stdbool.h>
#include "arena.h"
#include "z80asm.h"   /* Z80StrList + the reused helpers */
#include "omap.h"

/* helpers.HL_SEP */
#define OPT_HL_SEP "|"

/* helpers.new_tmp_val16() : "<u8>|<u8>" (two fresh unknowns). */
char *opt_new_tmp_val16(Arena *a);
/* helpers.new_tmp_val16_from_label(label):
 *   "*UNKNOWN_H_<label>|*UNKNOWN_L_<label>" */
char *opt_new_tmp_val16_from_label(Arena *a, const char *label);

/* helpers.is_unknown8 / is_unknown16. NULL == Python None. */
bool opt_is_unknown8(const char *x);
bool opt_is_unknown16(const char *x);

/* helpers.get_L_from_unknown_value / get_H_from_unknown_value.
 * (Python asserts is_unknown; the optimizer never violates it — caller
 * guarantees, same as Python.) */
char *opt_get_L_from_unknown_value(Arena *a, const char *tmp_val);
char *opt_get_H_from_unknown_value(Arena *a, const char *tmp_val);

/* helpers.get_orig_label_from_unknown16(x) : the label or NULL. */
char *opt_get_orig_label_from_unknown16(Arena *a, const char *x);

/* helpers.is_label(x): str(x)[:1] in "._" */
bool opt_is_label(const char *x);

/* helpers.to_int(x): valnum or abort-equivalent. The optimizer only
 * calls it where Python guarantees a number; returns false if not (the
 * single caller, Memory._get_hl_addr, guards with is_number first). */
bool opt_to_int(const char *x, long *out);

/* helpers.simplify_arg / simplify_asm_args. */
char *opt_simplify_arg(Arena *a, const char *arg);
char *opt_simplify_asm_args(Arena *a, const char *asm_);

/* helpers.idx_args(x): RE_IDX = ^(ix|iy)[ ]*([-+])[ \t]*(.*)$ (i flag).
 * Returns true and fills reg/sign/args (arena) on match; false == None. */
bool opt_idx_args(Arena *a, const char *x,
                  const char **reg, const char **sign, const char **args);

/* helpers.dict_intersection(a,b): {k:v for k,v in a if k in b and b[k]==v}.
 * NULL values compare equal only to NULL (Python None == None). */
void opt_dict_intersection(Arena *ar, OMap *out,
                           const OMap *da, const OMap *db);

/* cpustate.py:39 RE_OFFSET = (^[*._a-zA-Z0-9]+(?:[+-]\d+)*)([+-]\d+)$
 * On match fills *base / *off (off is the trailing signed int). */
bool opt_re_offset(Arena *a, const char *addr, const char **base, long *off);

#endif /* ZXBC_OPT_HELPERS_H */
