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
 * STRING_LABELS dedup store — port of src/api/string_labels.py
 * (STRING_LABELS = defaultdict(tmp_labels.tmp_label)). An insertion-ordered
 * map keyed by the EXACT string bytes (NUL-safe: keyed by stored length, not
 * strlen). A miss mints a label via the SHARED tmp-label counter
 * (backend_tmp_label) — the same monotonic LABEL_COUNTER as loop/jump
 * labels — so a constant string folds to one label and several identical
 * constants share an address. emit_strings (translator_visitor.py:155-158)
 * drains it in insertion order. */
typedef struct StringLabelEntry {
    char *bytes;   /* arena-owned copy of the string's exact bytes */
    int   len;     /* byte length (NUL-safe key half) */
    char *label;   /* the minted ".LABEL.__LABEL<n>" */
} StringLabelEntry;

typedef struct StringLabels {
    StringLabelEntry *items;
    int               count;
    int               cap;
} StringLabels;

/*
 * Backend container. Mirrors the live-reset state of common.py + the
 * Backend.MEMORY list. requires_/inits/at_end are empty for the S5.2
 * calibration (no runtime library pulled in); they exist so the M4 emit
 * paths iterate them faithfully (empty -> nothing), populated by later S5.x.
 */
/* An ASMS body: the line list an inline-asm quad expands to (common.ASMS
 * value). Arena-owned; M6's driver joins lines with "\n" (zxbc.py:174-180). */
typedef struct AsmsBody {
    char **lines;
    int    n;
} AsmsBody;

typedef struct Backend {
    Arena  *arena;            /* &cs->arena — Quad/string allocation */
    QuadVec memory;           /* Python Backend.MEMORY: list[Quad] */
    bool    flag_end_emitted; /* common.FLAG_end_emitted */
    bool    flag_use_function_exit; /* common.FLAG_use_function_exit (_leave) */
    HashMap asms;             /* "##ASMn" -> AsmsBody* (common.ASMS) */
    int     asmcount;         /* common.ASMCOUNT */
    HashMap requires_;        /* set of #include-once libs (common.REQUIRES) */
    HashMap inits;            /* set of #init call routines (common.INITS) */
    StrVec  at_end;           /* common.AT_END */

    /* src/api/tmp_labels.py — module-global LABEL_COUNTER + TMP_LABELS set,
     * reset by common.init() (common.py:248). tmp_label() emits
     * ".LABEL.__LABEL<n>" and records it so remove_unused_labels' TMP_LABELS
     * membership (main.py:715) is faithful. */
    int     label_counter;    /* tmp_labels.LABEL_COUNTER */
    HashMap tmp_labels;       /* tmp_labels.TMP_LABELS (set of str) */

    /* src/api/string_labels.py — STRING_LABELS defaultdict, reset by
     * TranslatorVisitor.reset() -> string_labels.reset()
     * (translator_visitor.py:62). Insertion-ordered, exact-bytes keyed. */
    StringLabels string_labels;

    /* OPTIONS the emitter reads (Python reads the OPTIONS global; the C
     * port threads them onto the Backend — the driver sets these from
     * cs->opts before emit). Defaults match options.c / backend.init(). */
    int  org;        /* OPTIONS.org (default 32768) */
    bool headerless; /* OPTIONS.headerless (default false) */
    bool autorun;    /* OPTIONS.autorun (default false) */
    int  opt_level;  /* OPTIONS.optimization_level (default 2) */
    int  opt_strategy; /* OPTIONS.opt_strategy (OptStrategy; common.normalize_boolean) */
} Backend;

/* common.init() reset (ASMCOUNT=0, FLAG_end_emitted=False, REQUIRES/INITS/
 * ASMS/AT_END cleared) + MEMORY clear, binding the arena. */
void backend_init(Backend *b, Arena *arena);

/* common.init() reset only (does not rebind the arena / re-clear MEMORY). */
void backend_common_reset(Backend *b);

/* new_ASMID() -> "##ASM<n>", n = post-increment of asmcount. */
char *backend_new_asmid(Backend *b);

/* tmp_labels.tmp_label() (src/api/tmp_labels.py:16-25): the next
 * ".LABEL.__LABEL<N>" (single monotonic counter, no zero-pad), recorded in
 * TMP_LABELS. Arena-owned. Used by the S5.5 control-flow visitors. */
char *backend_tmp_label(Backend *b);

/* string_labels.add_string_label (src/api/string_labels.py:24-31): fold the
 * given bytes (NUL-safe via len) to a unique label; identical bytes return
 * the same label. A miss mints via backend_tmp_label (the SHARED counter).
 * Arena-owned return. THE ONLY caller is translator.c tr_visit_string. */
char *backend_add_string_label(Backend *b, const char *bytes, int len);

/* string_labels.reset (src/api/string_labels.py:19-21): STRING_LABELS.clear().
 * Wired where the translator reset analogue runs. */
void backend_string_labels_reset(Backend *b);

/* Free the heap-backed containers (Quads/strings are arena-owned). */
void backend_free(Backend *b);

/*
 * M5 emitter half (port of src/arch/z80/backend/main.py + generic.py).
 * Each returns a fresh StrVec (heap VEC of arena-owned strings); the
 * caller owns the StrVec (vec_free) but not the strings.
 */
/* Backend.emit (main.py:766-785): IC->Z80 over backend->memory, inline
 * peephole via _output_join when optimize, remove_unused_labels +
 * re-join at opt_level>1, then sorted REQUIRES #include-once. */
StrVec backend_emit(Backend *b, bool optimize);
/* emit_prologue (main.py:638-681) — the program-start wrapper. */
StrVec backend_emit_prologue(Backend *b);
/* emit_epilogue (main.py:684-697) — bare END (or END START if autorun). */
StrVec backend_emit_epilogue(Backend *b);

#endif /* ZXBC_BACKEND_H */
