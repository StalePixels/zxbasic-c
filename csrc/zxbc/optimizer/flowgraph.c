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
#include <stdint.h>

static char *fg_strdup(Arena *a, const char *s) { return arena_strdup(a, s); }

/* ------------------------------------------------------------------ *
 * CPython set iteration order for JUMP_LABELS (PYTHONHASHSEED=0).
 *
 * main.py:229 iterates `for label in self.JUMP_LABELS` (a Python set) and
 * MUTATES the flow graph per label (jump-over-jump simplification), so the
 * final ASM depends on the set's iteration order. Under PYTHONHASHSEED=0
 * that order is fully deterministic: str hashes via siphash13 with a
 * zero key, and the set's table layout follows setobject.c. The earlier
 * insertion-order vec gave a different fixpoint than the oracle (e.g. the
 * `while` fixture rewrote `jp z, _20` to `jp z, _10`). We reproduce the
 * oracle's order exactly: build the local set (incremental add+resize,
 * flow_graph.py:113-139 _get_jump_labels), then JUMP_LABELS.clear() +
 * .update(local) which CPython performs as set_merge (presize + clean
 * insert) — flow_graph.py:147-148. Iterating the merged table in slot
 * order yields the byte-identical label order.
 * ------------------------------------------------------------------ */

/* CPython Python/pyhash.c pysiphash (siphash13), key (k0,k1)=(0,0) for a
 * zeroed _Py_HashSecret (PYTHONHASHSEED=0). Verified byte-identical to
 * CPython hash() for the relevant label strings. */
#define FG_ROTATE(x,b) (uint64_t)(((x)<<(b))|((x)>>(64-(b))))
#define FG_HALF_ROUND(a,b,c,d,s,t) \
    a+=b; c+=d; b=FG_ROTATE(b,s)^a; d=FG_ROTATE(d,t)^c; a=FG_ROTATE(a,32);
#define FG_SINGLE_ROUND(v0,v1,v2,v3) \
    FG_HALF_ROUND(v0,v1,v2,v3,13,16); FG_HALF_ROUND(v2,v1,v0,v3,17,21);

static uint64_t fg_le64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1]<<8) | ((uint64_t)p[2]<<16) |
           ((uint64_t)p[3]<<24) | ((uint64_t)p[4]<<32) | ((uint64_t)p[5]<<40) |
           ((uint64_t)p[6]<<48) | ((uint64_t)p[7]<<56);
}
static uint64_t fg_siphash13(const void *src, size_t n) {
    uint64_t b = (uint64_t)n << 56;
    const uint8_t *in = (const uint8_t *)src;
    uint64_t v0 = 0x736f6d6570736575ULL, v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL, v3 = 0x7465646279746573ULL;
    while (n >= 8) {
        uint64_t mi = fg_le64(in); in += 8; n -= 8;
        v3 ^= mi; FG_SINGLE_ROUND(v0,v1,v2,v3); v0 ^= mi;
    }
    uint64_t t = 0; uint8_t *pt = (uint8_t *)&t;
    switch (n) {
        case 7: pt[6]=in[6]; /* fall through */
        case 6: pt[5]=in[5]; /* fall through */
        case 5: pt[4]=in[4]; /* fall through */
        case 4: pt[3]=in[3]; /* fall through */
        case 3: pt[2]=in[2]; /* fall through */
        case 2: pt[1]=in[1]; /* fall through */
        case 1: pt[0]=in[0]; /* fall through */
        case 0: break;
    }
    b |= t;
    v3 ^= b; FG_SINGLE_ROUND(v0,v1,v2,v3); v0 ^= b;
    v2 ^= 0xff;
    FG_SINGLE_ROUND(v0,v1,v2,v3);
    FG_SINGLE_ROUND(v0,v1,v2,v3);
    FG_SINGLE_ROUND(v0,v1,v2,v3);
    return (v0 ^ v1) ^ (v2 ^ v3);
}
/* CPython pyhash: len 0 -> 0; result -1 mapped to -2. We keep the full
 * 64-bit value (h & mask uses the low bits; sign is irrelevant to slot). */
static uint64_t fg_str_hash(const char *s) {
    size_t n = strlen(s);
    if (n == 0) return 0;
    uint64_t h = fg_siphash13(s, n);
    if (h == (uint64_t)-1) h = (uint64_t)-2;
    return h;
}

/* CPython setobject.c model. PySet_MINSIZE=8, LINEAR_PROBES=9, perturb>>5. */
#define FG_MINSIZE 8
#define FG_LINEAR_PROBES 9
typedef struct { const char *key; uint64_t hash; } FgSlot;
typedef struct { FgSlot *t; size_t mask; size_t used; size_t fill; } FgSet;

static void fgset_init(FgSet *s, size_t size) {
    s->t = (FgSlot *)calloc(size, sizeof(FgSlot));
    s->mask = size - 1; s->used = 0; s->fill = 0;
}
static void fgset_free(FgSet *s) { free(s->t); s->t = NULL; }

/* set_add_entry: returns true if a new entry was inserted. */
static bool fgset_add_entry(FgSet *s, const char *key, uint64_t h) {
    size_t mask = s->mask;
    size_t i = (size_t)h & mask;
    uint64_t perturb = h;
    for (;;) {
        FgSlot *e = &s->t[i];
        if (e->key == NULL) { e->key = key; e->hash = h; return true; }
        if (e->hash == h && strcmp(e->key, key) == 0) return false;
        if (i + FG_LINEAR_PROBES <= mask) {
            for (size_t j = 1; j <= FG_LINEAR_PROBES; j++) {
                FgSlot *e2 = &s->t[i + j];
                if (e2->key == NULL) { e2->key = key; e2->hash = h; return true; }
                if (e2->hash == h && strcmp(e2->key, key) == 0) return false;
            }
        }
        perturb >>= 5;
        i = (i * 5 + 1 + perturb) & mask;
    }
}
/* set_insert_clean: key guaranteed absent (used during resize/merge). */
static void fgset_insert_clean(FgSet *s, const char *key, uint64_t h) {
    size_t mask = s->mask;
    size_t i = (size_t)h & mask;
    uint64_t perturb = h;
    for (;;) {
        if (s->t[i].key == NULL) {
            s->t[i].key = key; s->t[i].hash = h; s->fill++; s->used++; return;
        }
        if (i + FG_LINEAR_PROBES <= mask) {
            for (size_t j = 1; j <= FG_LINEAR_PROBES; j++) {
                if (s->t[i + j].key == NULL) {
                    s->t[i + j].key = key; s->t[i + j].hash = h;
                    s->fill++; s->used++; return;
                }
            }
        }
        perturb >>= 5;
        i = (i * 5 + 1 + perturb) & mask;
    }
}
/* set_table_resize(so, minused): newsize = next pow2 > minused (>=MINSIZE). */
static void fgset_table_resize(FgSet *s, size_t minused) {
    size_t newsize = FG_MINSIZE;
    while (newsize <= minused) newsize <<= 1;
    FgSlot *old = s->t; size_t oldsize = s->mask + 1;
    s->t = (FgSlot *)calloc(newsize, sizeof(FgSlot));
    s->mask = newsize - 1; s->fill = 0; s->used = 0;
    for (size_t i = 0; i < oldsize; i++)
        if (old[i].key != NULL) fgset_insert_clean(s, old[i].key, old[i].hash);
    free(old);
}
/* set.add: insert + GROWTH_RATE check (fill*5 >= mask*3 -> resize used*4). */
static void fgset_add(FgSet *s, const char *key) {
    uint64_t h = fg_str_hash(key);
    if (fgset_add_entry(s, key, h)) {
        s->fill++; s->used++;
        if (s->fill * 5 >= (s->mask + 1) * 3) {
            size_t minused = s->used > 50000 ? s->used * 2 : s->used * 4;
            fgset_table_resize(s, minused);
        }
    }
}
/* set_merge(dst, src) with dst freshly cleared: presize then clean-insert
 * each src entry in src-table slot order. CPython condition:
 * (dst->fill + src->used)*5 >= dst->mask*3 -> resize((dst->used+src->used)*2). */
static void fgset_merge(FgSet *dst, const FgSet *src) {
    if ((dst->fill + src->used) * 5 >= dst->mask * 3)
        fgset_table_resize(dst, (dst->used + src->used) * 2);
    for (size_t i = 0; i <= src->mask; i++)
        if (src->t[i].key != NULL)
            fgset_insert_clean(dst, src->t[i].key, src->t[i].hash);
}

/* Reorder `out` (a deduped scan-order JUMP_LABELS list) into CPython set
 * iteration order: local set (incremental adds) -> fresh set merge. The
 * keys are the same arena strings already in `out`; we only reorder. */
static void fg_pyset_order_inplace(Z80StrList *out) {
    if (out->len <= 1) return;
    FgSet local; fgset_init(&local, FG_MINSIZE);
    for (int i = 0; i < out->len; i++) fgset_add(&local, out->data[i]);
    FgSet merged; fgset_init(&merged, FG_MINSIZE);
    fgset_merge(&merged, &local);
    int w = 0;
    for (size_t i = 0; i <= merged.mask; i++)
        if (merged.t[i].key != NULL) out->data[w++] = (char *)merged.t[i].key;
    /* w must equal out->len (merge preserves the element count). */
    out->len = w;
    fgset_free(&local); fgset_free(&merged);
}

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
    /* JUMP_LABELS is a Python set; reproduce its PYTHONHASHSEED=0 iteration
     * order (the jump-over-jump pass at main.py:229 is order-sensitive). */
    fg_pyset_order_inplace(JL);

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
