/*
 * optimizer.h — Port of src/arch/z80/optimizer/{labelinfo,labels_dict,
 * basicblock,flow_graph,main}.py.
 *
 * These five Python modules form one mutually-recursive cluster
 * (BasicBlock <-> LabelInfo <-> LabelsDict <-> Optimizer <-> flow_graph),
 * so they are one C module set (one header, .c per Python file).
 *
 * THE INERTNESS GUARANTEE (S5.9b):
 *   optimizer_optimize() reproduces Optimizer.optimize() (main.py:193-261)
 *   INCLUDING the verbatim early-return:
 *       self._cleanup_mem(initial_memory)
 *       if OPTIONS.optimization_level <= 2:        # main.py:199
 *           return "\n".join(x for x in initial_memory
 *                            if not RE_PRAGMA.match(x))
 *   For O<=2 it calls EXACTLY the pre-existing, byte-proven O<=2 code
 *   (codegen.c's cleanup_mem + RE_PRAGMA join via optimizer_optimize_le2)
 *   and returns — so the -O2 codegen meter is provably unaffected. Only
 *   O>2 runs the basic-block / CPU-state / flow-graph machinery, with
 *   clean_asm_args = OPTIONS.optimization_level > 3 (main.py:202).
 */
#ifndef ZXBC_OPTIMIZER_H
#define ZXBC_OPTIMIZER_H

#include <stdbool.h>
#include "arena.h"
#include "vec.h"
#include "omap.h"
#include "z80asm.h"
#include "optmemcell.h"
#include "cpustate.h"

typedef struct BasicBlock BasicBlock;
typedef struct Optimizer  Optimizer;

typedef VEC(BasicBlock *) BBVec;
typedef VEC(OMemCell *)   CellVec;

/* labelinfo.py — LabelInfo dataclass */
typedef struct LabelInfo {
    char       *label;
    int         addr;
    BasicBlock *basic_block;
    int         position;
    BBVec       used_by;     /* set[BasicBlock] */
} LabelInfo;

/* labels_dict.py — LabelsDict (insertion-ordered name -> LabelInfo*,
 * __setitem__ raises DuplicatedLabelError on an existing key). */
typedef struct LabelsDict {
    OMap idx;   /* key -> (LabelInfo* stored as the val pointer slot) */
    Arena *a;
} LabelsDict;

/* basicblock.py — BasicBlock(Sequence[MemCell]) */
struct BasicBlock {
    Arena      *a;
    Optimizer  *optimizer;
    CellVec     mem;
    BasicBlock *next;
    BasicBlock *prev;
    bool        lock;
    BBVec       comes_from;
    BBVec       goes_to;
    bool        modified;
    BBVec       called_by;
    bool        ignored;
    int         id;
    bool        optimized;
    CPUState   *cpu;
    bool        is_dummy;          /* DummyBasicBlock */
    Z80StrList  dummy_destroys;    /* DummyBasicBlock.__destroys */
    Z80StrList  dummy_requires;    /* DummyBasicBlock.__requires */
};

/* main.py — Optimizer */
struct Optimizer {
    Arena      *a;
    int         PROC_COUNTER;
    LabelsDict  LABELS;
    int         RAND_COUNT;        /* helpers stateful counter mirror */
    Z80StrList  JUMP_LABELS;       /* set[str] */
    CellVec     MEMORY;
    BBVec       BLOCKS;
    bool        clean_asm_args;    /* _BASICBLOCK_TYPE.clean_asm_args */
    int         unique_id;         /* BasicBlock.__UNIQUE_ID */
    int         opt_level;         /* OPTIONS.optimization_level mirror
                                    * (set per-run in optimizer_optimize;
                                    * Python reads the OPTIONS global —
                                    * we thread it through the Optimizer,
                                    * the single owner of the run). */
    int         debug_level;       /* OPTIONS.debug_level mirror — gates
                                    * the main.py:210-219 __DEBUG__ block
                                    * exactly as Python (level > dbg ->
                                    * skip). The codegen meter runs with
                                    * the default 0, so the block (and
                                    * its b.requires()/b.destroys() calls)
                                    * is a faithful no-op there. */
};

/* ---- BasicBlock API (the subset main.py / flow_graph.py drive) ----- */
BasicBlock *bb_new(Arena *a, Optimizer *opt, const char *const *mem, int n);
BasicBlock *bb_new_dummy(Arena *a, Optimizer *opt,
                         const Z80StrList *destroys,
                         const Z80StrList *requires_);
void  bb_set_code(BasicBlock *b, const char *const *mem, int n);
Z80StrList bb_code(Arena *a, BasicBlock *b);    /* [x.code for x in mem] */
int   bb_get_first_partition_idx(BasicBlock *b); /* -1 == None */
void  bb_add_comes_from(BasicBlock *b, BasicBlock *o);
void  bb_add_goes_to(BasicBlock *b, BasicBlock *o);
void  bb_delete_comes_from(BasicBlock *b, BasicBlock *o);
void  bb_delete_goes_to(BasicBlock *b, BasicBlock *o);
void  bb_update_next_block(BasicBlock *b);
bool  bb_is_used(BasicBlock *b, const Z80StrList *regs, int i, int top); /* top<0 == None */
/* BasicBlock.requires(i=0,end_=None) / destroys(i=0) — basicblock.py:
 * 323-369 (+ Dummy overrides 510-514). Used by the main.py:210-219
 * __DEBUG__ block (debug-gated). end_<0 == Python None. */
void  bb_requires(BasicBlock *b, int i, int end_, Z80StrList *out);
void  bb_destroys(BasicBlock *b, int i, Z80StrList *out);
void  bb_compute_cpu_state(BasicBlock *b);
void  bb_optimize(BasicBlock *b);   /* uses the engine's level>=3 list */
OMemCell *bb_get_next_exec_instruction(BasicBlock *b);

/* ---- LabelsDict ---------------------------------------------------- */
void       ld_init(LabelsDict *d, Arena *a);
bool       ld_has(const LabelsDict *d, const char *k);
LabelInfo *ld_get(const LabelsDict *d, const char *k);
/* __setitem__: aborts (Python raise DuplicatedLabelError) on dup. */
void       ld_set(LabelsDict *d, const char *k, LabelInfo *v);
void       ld_pop(LabelsDict *d, const char *k);   /* dict.pop (no raise) */
/* set without the dup-check (used where Python overwrites via .pop+set
 * or assigns a fresh DummyBasicBlock placeholder). */
void       ld_set_force(LabelsDict *d, const char *k, LabelInfo *v);

LabelInfo *labelinfo_new(Arena *a, const char *label, int addr,
                         BasicBlock *bb, int position);

/* ---- flow_graph.py ------------------------------------------------- */
BBVec get_basic_blocks(BasicBlock *block);

/* ---- main.py Optimizer -------------------------------------------- */
void  optimizer_init(Optimizer *opt, Arena *a);

/* Optimizer.optimize(initial_memory) — main.py:193-261. `mem` is the
 * post-_cleanup_mem... no: this takes the RAW StrVec (it runs
 * _cleanup_mem itself, faithfully). opt_level / debug gating exactly
 * mirror OPTIONS. Returns the joined asm string (arena). The O<=2 path
 * delegates to the caller-provided byte-proven le2 routine so its
 * output is unchanged. */
typedef char *(*OptLe2Fn)(Arena *a, Z80StrList mem);
char *optimizer_optimize(Optimizer *opt, Arena *a, Z80StrList initial_memory,
                         int opt_level, OptLe2Fn le2);

#endif /* ZXBC_OPTIMIZER_H */
