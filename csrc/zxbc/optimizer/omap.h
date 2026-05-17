/*
 * omap.h — Insertion-ordered string->string map.
 *
 * The Python optimizer relies on dict iteration order (Python 3.7+
 * dicts preserve insertion order): CPUState.regs / CPUState.mem
 * (cpustate.py:254-289), Memory.__missing__ insertion (cpustate.py:123),
 * dict_intersection (helpers.py:496-505) all observe it. A plain
 * open-addressing HashMap does NOT preserve insertion order, so this
 * faithful port uses an ordered map: keys kept in first-insertion order,
 * value-overwrite does not reorder (matches Python `d[k] = v`).
 *
 * Arena-owned keys/values; the map's own arrays are heap (realloc),
 * freed via omap_free — mirroring the existing csrc VEC convention.
 */
#ifndef ZXBC_OPT_OMAP_H
#define ZXBC_OPT_OMAP_H

#include <stdbool.h>
#include "arena.h"

typedef struct OMapEnt {
    char *key;   /* arena-owned */
    char *val;   /* arena-owned (may be NULL == Python None) */
} OMapEnt;

typedef struct OMap {
    OMapEnt *data;
    int      len;
    int      cap;
} OMap;

void  omap_init(OMap *m);
void  omap_free(OMap *m);
/* d[key] = val (val copied into arena). Insertion order preserved;
 * overwriting an existing key keeps its original position. */
void  omap_set(Arena *a, OMap *m, const char *key, const char *val);
/* d.get(key) -> val or NULL (key absent). */
const char *omap_get(const OMap *m, const char *key);
bool  omap_has(const OMap *m, const char *key);
/* del d[key] (no-op if absent; preserves order of the rest). */
void  omap_del(OMap *m, const char *key);
/* shallow copy (arena strings shared; arrays duplicated). */
void  omap_copy(OMap *dst, const OMap *src);
/* d.update(other): for k in other (insertion order): d[k]=other[k]. */
void  omap_update(Arena *a, OMap *dst, const OMap *src);

#endif /* ZXBC_OPT_OMAP_H */
