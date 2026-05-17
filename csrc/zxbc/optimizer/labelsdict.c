/*
 * labelsdict.c — Port of src/arch/z80/optimizer/labelinfo.py +
 * labels_dict.py.
 *
 * LabelInfo: the dataclass (label, addr, basic_block, position,
 *            used_by=set()). labelinfo.py:17-28.
 * LabelsDict(UserDict): an insertion-ordered name->LabelInfo map whose
 *   __setitem__ raises DuplicatedLabelError if the key already exists
 *   (labels_dict.py:14-21). The optimizer relies on .pop() (no raise),
 *   `in` (ld_has) and [] (ld_get); the raising __setitem__ is reproduced
 *   as an abort with the same message shape (a duplicated label in the
 *   emitted asm is a hard compiler invariant violation, exactly as in
 *   Python where the unhandled exception aborts the run).
 */
#include "optimizer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

LabelInfo *labelinfo_new(Arena *a, const char *label, int addr,
                         BasicBlock *bb, int position) {
    LabelInfo *li = (LabelInfo *)arena_alloc(a, sizeof(LabelInfo));
    li->label = arena_strdup(a, label);
    li->addr = addr;
    li->basic_block = bb;
    li->position = position;
    vec_init(li->used_by);
    return li;
}

/* LabelsDict is an OMap of name -> LabelInfo* (pointer kept in the val
 * slot; we never let omap copy it as a string — a dedicated tiny
 * pointer-valued ordered map mirrors UserDict semantics). */
typedef struct LDEnt { char *key; LabelInfo *val; } LDEnt;

/* We piggy-back on OMap's array by storing the pointer's bit pattern is
 * fragile; instead use a parallel pointer array indexed identically.
 * Simpler: keep our own growable array here. */
typedef struct LDImpl { LDEnt *data; int len, cap; } LDImpl;

void ld_init(LabelsDict *d, Arena *a) {
    d->a = a;
    LDImpl *im = (LDImpl *)arena_alloc(a, sizeof(LDImpl));
    im->data = NULL; im->len = 0; im->cap = 0;
    /* stash the impl pointer in the OMap struct's data slot (unused
     * otherwise); keeps the public header type stable. */
    d->idx.data = (OMapEnt *)im;
    d->idx.len = 0;
    d->idx.cap = 0;
}

static LDImpl *ld_impl(const LabelsDict *d) { return (LDImpl *)d->idx.data; }

static int ld_find(const LabelsDict *d, const char *k) {
    LDImpl *im = ld_impl(d);
    for (int i = 0; i < im->len; i++)
        if (strcmp(im->data[i].key, k) == 0) return i;
    return -1;
}

bool ld_has(const LabelsDict *d, const char *k) { return ld_find(d, k) >= 0; }

LabelInfo *ld_get(const LabelsDict *d, const char *k) {
    int i = ld_find(d, k);
    return i >= 0 ? ld_impl(d)->data[i].val : NULL;
}

static void ld_grow(LabelsDict *d) {
    LDImpl *im = ld_impl(d);
    if (im->len < im->cap) return;
    int nc = im->cap ? im->cap * 2 : 32;
    LDEnt *nd = (LDEnt *)realloc(im->data, (size_t)nc * sizeof(LDEnt));
    if (!nd) { fprintf(stderr, "labelsdict: OOM\n"); exit(1); }
    im->data = nd; im->cap = nc;
}

void ld_set_force(LabelsDict *d, const char *k, LabelInfo *v) {
    int i = ld_find(d, k);
    if (i >= 0) { ld_impl(d)->data[i].val = v; return; }
    ld_grow(d);
    LDImpl *im = ld_impl(d);
    im->data[im->len].key = arena_strdup(d->a, k);
    im->data[im->len].val = v;
    im->len++;
}

void ld_set(LabelsDict *d, const char *k, LabelInfo *v) {
    if (ld_find(d, k) >= 0) {
        /* Python: raise DuplicatedLabelError(key) — unhandled in the
         * optimizer path, aborts the process. Same observable result. */
        fprintf(stderr, "Duplicated label: %s\n", k);
        exit(1);
    }
    ld_set_force(d, k, v);
}

void ld_pop(LabelsDict *d, const char *k) {
    int i = ld_find(d, k);
    if (i < 0) return;     /* dict.pop in _get_jump_labels guards with
                            * try/except-free .pop only on present keys */
    LDImpl *im = ld_impl(d);
    for (int j = i; j < im->len - 1; j++) im->data[j] = im->data[j + 1];
    im->len--;
}
