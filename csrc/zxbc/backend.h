/*
 * Z80 backend — shared state + label vocabulary.
 *
 * Port of src/arch/z80/backend/common.py (constants + init reset) and the
 * Backend container from src/arch/z80/backend/main.py. The struct is defined
 * once here; backend_common.c implements the common-state half (labels,
 * common.init reset, ASMS / new_ASMID), backend.c the emitter half
 * (_QUAD_TABLE dispatch, _end/_inline, emit_prologue/epilogue) in M4.
 *
 * Label constants are verified against live Python:
 *   global_.py:129  CORE_NAMESPACE = ".core"
 *   common.py:84-91 START/END/CALL_BACK/MAIN_LABEL, DATA_LABEL
 *   global_.py:144  ZXBASIC_USER_DATA = f"{CORE_NAMESPACE}.ZXBASIC_USER_DATA"
 */
#ifndef ZXBC_BACKEND_H
#define ZXBC_BACKEND_H

#include "arena.h"
#include "vec.h"
#include "hashmap.h"
#include "ic.h"

/* NAMESPACE = global_.CORE_NAMESPACE (".core"). C string-literal
 * concatenation reproduces common.py's f-string label constants exactly. */
#define ZXBC_NAMESPACE   ".core"
#define LBL_START        ZXBC_NAMESPACE ".__START_PROGRAM"
#define LBL_END          ZXBC_NAMESPACE ".__END_PROGRAM"
#define LBL_CALLBACK     ZXBC_NAMESPACE ".__CALL_BACK__"
#define LBL_MAIN         ZXBC_NAMESPACE ".__MAIN_PROGRAM__"
#define LBL_DATA         ZXBC_NAMESPACE ".ZXBASIC_USER_DATA"
#define LBL_DATA_END     ZXBC_NAMESPACE ".ZXBASIC_USER_DATA_END"

typedef VEC(char *) StrVec; /* an ordered list of asm lines */

/*
 * Backend container. Mirrors the live-reset state of common.py + the
 * Backend.MEMORY list. requires_/inits/at_end are empty for the S5.2
 * calibration (no runtime library pulled in); they exist so the M4 emit
 * paths iterate them faithfully (empty -> nothing), populated by later S5.x.
 */
typedef struct Backend {
    Arena  *arena;            /* &cs->arena — Quad/string allocation */
    QuadVec memory;           /* Python Backend.MEMORY: list[Quad] */
    bool    flag_end_emitted; /* common.FLAG_end_emitted */
    HashMap asms;             /* "##ASMn" -> StrVec* (common.ASMS) */
    int     asmcount;         /* common.ASMCOUNT */
    HashMap requires_;        /* set of #include-once libs (common.REQUIRES) */
    HashMap inits;            /* set of #init call routines (common.INITS) */
    StrVec  at_end;           /* common.AT_END */
} Backend;

/* common.init() reset (ASMCOUNT=0, FLAG_end_emitted=False, REQUIRES/INITS/
 * ASMS/AT_END cleared) + MEMORY clear, binding the arena. */
void backend_init(Backend *b, Arena *arena);

/* common.init() reset only (does not rebind the arena / re-clear MEMORY). */
void backend_common_reset(Backend *b);

/* new_ASMID() -> "##ASM<n>", n = post-increment of asmcount. */
char *backend_new_asmid(Backend *b);

/* Free the heap-backed containers (Quads/strings are arena-owned). */
void backend_free(Backend *b);

#endif /* ZXBC_BACKEND_H */
