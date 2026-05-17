/*
 * flowgraph.c — Port of src/arch/z80/optimizer/flow_graph.py
 *   _split_block / _compute_calls / _get_jump_labels / get_basic_blocks
 * line-for-line (flow_graph.py:18-159).
 *
 * Python `set` iteration order is unspecified; the worklist
 * (pending/visited) and used_by/called_by sets are insertion-ordered
 * vecs here — a faithful representation. Unreachable at O<=2
 * (main.py:199 early-return); O3 byte-calibration is S5.9c/S5.10.
 */
#include "optimizer.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

static char *fg_strdup(Arena *a, const char *s) { return arena_strdup(a, s); }

/* set helpers shared shape with basicblock.c (kept local: TU-private). */
static bool bv_has(const BBVec *v, BasicBlock *x) {
    for (int i=0;i<v->len;i++) if (v->data[i]==x) return true; return false;
}
static void bv_add(BBVec *v, BasicBlock *x) { if(!bv_has(v,x)) vec_push(*v,x); }

static bool sl_has(const Z80StrList *v, const char *s) {
    for (int i=0;i<v->len;i++) if (!strcmp(v->data[i],s)) return true; return false;
}

/* _split_block (flow_graph.py:18-46) */
static BasicBlock *split_block(BasicBlock *block, int start_of_new_block,
                               LabelsDict *labels) {
    assert(start_of_new_block >= 0 && start_of_new_block < block->mem.len);
    /* new_block = BasicBlock([], block.optimizer) */
    BasicBlock *nb = bb_new(block->a, block->optimizer, NULL, 0);
    /* new_block.mem = block.mem[start:]; block.mem = block.mem[:start] */
    vec_free(nb->mem); vec_init(nb->mem);
    for (int k=start_of_new_block; k<block->mem.len; k++)
        vec_push(nb->mem, block->mem.data[k]);
    block->mem.len = start_of_new_block;

    nb->next = block->next;
    block->next = nb;
    nb->prev = block;
    if (nb->next != NULL) nb->next->prev = nb;

    /* for blk in list(block.goes_to): delete_goes_to; new.add_goes_to */
    BBVec snap; vec_init(snap);
    for (int k=0;k<block->goes_to.len;k++) vec_push(snap, block->goes_to.data[k]);
    for (int k=0;k<snap.len;k++) {
        bb_delete_goes_to(block, snap.data[k]);
        bb_add_goes_to(nb, snap.data[k]);
    }
    vec_free(snap);

    bb_add_goes_to(block, nb);

    for (int k=0;k<nb->mem.len;k++) {
        OMemCell *mem = nb->mem.data[k];
        if (mem->is_label && ld_has(labels, mem->inst)) {
            LabelInfo *li = ld_get(labels, mem->inst);
            li->basic_block = nb;
            li->position = k;
        }
    }

    OMemCell *blast = block->mem.data[block->mem.len-1];
    if (blast->is_ender) {
        if (blast->cond == NULL)
            bb_delete_goes_to(block, block->next);
    }
    return nb;
}

/* _compute_calls (flow_graph.py:49-103) */
typedef struct CallerBB { BasicBlock *caller, *bb; } CallerBB;
typedef VEC(CallerBB) CallerVec;

static bool pair_in(const CallerVec *v, BasicBlock *c, BasicBlock *bb) {
    for (int i=0;i<v->len;i++)
        if (v->data[i].caller==c && v->data[i].bb==bb) return true;
    return false;
}

static void compute_calls(BBVec *blocks, LabelsDict *labels,
                          const Z80StrList *jump_labels) {
    CallerVec calling; vec_init(calling);  /* dict in insertion order */

    for (int i=0;i<blocks->len;i++) {
        BasicBlock *bb = blocks->data[i];
        OMemCell *last = bb->mem.data[bb->mem.len-1];
        if (last->is_ender && last->branch_arg &&
            ld_has(labels, last->branch_arg))
            bv_add(&ld_get(labels,last->branch_arg)->used_by, bb);
    }
    for (int li=0; li<jump_labels->len; li++) {
        const char *label = jump_labels->data[li];
        LabelInfo *info = ld_get(labels, label);
        if (!info) continue;
        for (int k=0;k<info->used_by.len;k++)
            bb_add_goes_to(info->used_by.data[k], info->basic_block);
    }
    for (int i=0;i<blocks->len;i++) {
        BasicBlock *bb = blocks->data[i];
        OMemCell *last = bb->mem.data[bb->mem.len-1];
        if (strcmp(last->inst,"call")!=0) continue;
        const char *op = last->branch_arg;
        if (op && ld_has(labels, op)) {
            LabelInfo *info = ld_get(labels, op);
            bv_add(&info->basic_block->called_by, bb);
            /* calling_blocks[bb] = info.basic_block (dict: last wins;
             * keyed by bb so duplicates overwrite) */
            bool found=false;
            for (int k=0;k<calling.len;k++)
                if (calling.data[k].caller==bb) {
                    calling.data[k].bb=info->basic_block; found=true; break; }
            if (!found) {
                CallerBB cb = { bb, info->basic_block };
                vec_push(calling, cb);
            }
        }
    }

    CallerVec visited; vec_init(visited);
    CallerVec pending; vec_init(pending);
    for (int i=0;i<calling.len;i++) {
        CallerBB cb = { calling.data[i].caller, calling.data[i].bb };
        vec_push(pending, cb);
    }
    while (pending.len) {
        CallerBB cur = pending.data[pending.len-1];
        pending.len--;                       /* set.pop (arbitrary) */
        BasicBlock *caller = cur.caller, *bb = cur.bb;
        if (pair_in(&visited, caller, bb)) continue;
        { CallerBB v={caller,bb}; vec_push(visited, v); }

        OMemCell *last = bb->mem.data[bb->mem.len-1];
        if (!last->is_ender) {
            if (!pair_in(&pending,caller,bb->next)) {
                CallerBB v={caller,bb->next}; vec_push(pending,v); }
            continue;
        }
        if (!strcmp(last->inst,"ret")||!strcmp(last->inst,"reti")||
            !strcmp(last->inst,"retn")) {
            if (last->cond) {
                if (!pair_in(&pending,caller,bb->next)) {
                    CallerBB v={caller,bb->next}; vec_push(pending,v); }
            }
            bb_add_goes_to(bb, caller->next);
            continue;
        }
        if (!strcmp(last->inst,"call")||!strcmp(last->inst,"rst")) {
            if (last->cond) {
                if (!pair_in(&pending,caller,bb->next)) {
                    CallerBB v={caller,bb->next}; vec_push(pending,v); }
            }
        }
    }
    vec_free(calling); vec_free(visited); vec_free(pending);
}

/* _get_jump_labels (flow_graph.py:105-139) */
static void get_jump_labels(BasicBlock *main_bb, LabelsDict *labels,
                            Z80StrList *out) {
    vec_init(*out);
    for (int i=0;i<main_bb->mem.len;i++) {
        OMemCell *mem = main_bb->mem.data[i];
        if (mem->is_label) {
            ld_pop(labels, mem->inst);
            ld_set(labels, mem->inst,
                   labelinfo_new(main_bb->a, mem->inst, i, main_bb, i));
            continue;
        }
        if (!mem->is_ender) continue;
        const char *lbl = mem->branch_arg;
        if (lbl == NULL) continue;
        if (!sl_has(out, lbl)) vec_push(*out, fg_strdup(main_bb->a, lbl));
        if (!ld_has(labels, lbl)) {
            Z80StrList all; vec_init(all);
            static const char *ALL[] = {"a","b","c","d","e","f","h","l",
                "ixh","ixl","iyh","iyl","r","i","sp"};
            for (size_t k=0;k<sizeof(ALL)/sizeof(ALL[0]);k++)
                vec_push(all, fg_strdup(main_bb->a, ALL[k]));
            BasicBlock *dbb = bb_new_dummy(main_bb->a, main_bb->optimizer,
                                           &all, &all);
            ld_set(labels, lbl,
                   labelinfo_new(main_bb->a, lbl, 0, dbb, 0));
            vec_free(all);
        }
    }
}

/* get_basic_blocks (flow_graph.py:142-159) */
BBVec get_basic_blocks(BasicBlock *block) {
    BBVec result; vec_init(result);
    vec_push(result, block);

    /* block.jump_labels.clear(); .update(_get_jump_labels(...)) */
    Z80StrList *JL = &block->optimizer->JUMP_LABELS;
    vec_free(*JL); vec_init(*JL);
    Z80StrList jl;
    get_jump_labels(block, &block->optimizer->LABELS, &jl);
    for (int i=0;i<jl.len;i++)
        if (!sl_has(JL, jl.data[i])) vec_push(*JL, jl.data[i]);
    vec_free(jl);

    int split_pos = bb_get_first_partition_idx(block);
    while (split_pos != -1) {
        block = split_block(block, split_pos, &block->optimizer->LABELS);
        vec_push(result, block);
        split_pos = bb_get_first_partition_idx(block);
    }

    compute_calls(&result, &block->optimizer->LABELS,
                  &block->optimizer->JUMP_LABELS);
    return result;
}
