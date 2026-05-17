/* omap.c — see omap.h. */
#include "omap.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void omap_init(OMap *m) { m->data = NULL; m->len = 0; m->cap = 0; }

void omap_free(OMap *m) {
    free(m->data);
    m->data = NULL; m->len = 0; m->cap = 0;
}

static int omap_idx(const OMap *m, const char *key) {
    for (int i = 0; i < m->len; i++)
        if (strcmp(m->data[i].key, key) == 0) return i;
    return -1;
}

static void omap_grow(OMap *m) {
    if (m->len < m->cap) return;
    int nc = m->cap ? m->cap * 2 : 16;
    OMapEnt *nd = (OMapEnt *)realloc(m->data, (size_t)nc * sizeof(OMapEnt));
    if (!nd) { fprintf(stderr, "omap: OOM\n"); exit(1); }
    m->data = nd; m->cap = nc;
}

void omap_set(Arena *a, OMap *m, const char *key, const char *val) {
    int i = omap_idx(m, key);
    char *vc = val ? arena_strdup(a, val) : NULL;
    if (i >= 0) { m->data[i].val = vc; return; }
    omap_grow(m);
    m->data[m->len].key = arena_strdup(a, key);
    m->data[m->len].val = vc;
    m->len++;
}

const char *omap_get(const OMap *m, const char *key) {
    int i = omap_idx(m, key);
    return i >= 0 ? m->data[i].val : NULL;
}

bool omap_has(const OMap *m, const char *key) {
    return omap_idx(m, key) >= 0;
}

void omap_del(OMap *m, const char *key) {
    int i = omap_idx(m, key);
    if (i < 0) return;
    for (int j = i; j < m->len - 1; j++) m->data[j] = m->data[j + 1];
    m->len--;
}

void omap_copy(OMap *dst, const OMap *src) {
    omap_init(dst);
    if (src->len == 0) return;
    dst->cap = src->cap ? src->cap : src->len;
    dst->data = (OMapEnt *)malloc((size_t)dst->cap * sizeof(OMapEnt));
    if (!dst->data) { fprintf(stderr, "omap: OOM\n"); exit(1); }
    memcpy(dst->data, src->data, (size_t)src->len * sizeof(OMapEnt));
    dst->len = src->len;
}

void omap_update(Arena *a, OMap *dst, const OMap *src) {
    for (int i = 0; i < src->len; i++)
        omap_set(a, dst, src->data[i].key, src->data[i].val);
}
