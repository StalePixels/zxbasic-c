/*
 * Backend common state — port of src/arch/z80/backend/common.py.
 *
 *   common.init()     (common.py:243-258) — the per-compilation reset
 *   new_ASMID()       (common.py:486-492) — "##ASM<n>", post-increment
 *
 * Note common.init() resets ASMCOUNT/FLAG_end_emitted and clears
 * REQUIRES/INITS/ASMS/AT_END but does NOT clear MEMORY; Backend.init()
 * (main.py:613-635) does `common.init(); self.MEMORY.clear()`. That split
 * is reproduced here: backend_common_reset() == common.init(),
 * backend_init() == the Backend.init() M2 subset (reset + MEMORY clear).
 */
#include "backend.h"

#include <stdio.h>

void backend_common_reset(Backend *b) {
    b->asmcount = 0;
    b->flag_end_emitted = false;
    b->flag_use_function_exit = false; /* common.init (common.py:257) */

    /* HashMap has no clear(); free + re-init is the empty-set equivalent.
     * Stored values (ASMS bodies) are arena-owned, not freed here. */
    hashmap_free(&b->asms);
    hashmap_init(&b->asms);
    hashmap_free(&b->requires_);
    hashmap_init(&b->requires_);
    hashmap_free(&b->inits);
    hashmap_init(&b->inits);

    /* tmp_labels.reset() (common.py:248): LABEL_COUNTER=0, TMP_LABELS clear */
    b->label_counter = 0;
    hashmap_free(&b->tmp_labels);
    hashmap_init(&b->tmp_labels);

    vec_clear(b->at_end);
}

void backend_init(Backend *b, Arena *arena) {
    b->arena = arena;
    vec_init(b->memory);
    vec_init(b->at_end);
    hashmap_init(&b->asms);
    hashmap_init(&b->requires_);
    hashmap_init(&b->inits);
    hashmap_init(&b->tmp_labels);
    b->label_counter = 0;

    backend_common_reset(b); /* common.init() */
    vec_clear(b->memory);    /* Backend.init(): self.MEMORY.clear() */

    /* OPTIONS the emitter reads — defaults per options.c / backend.init()
     * (Backend.init OPTIONS.org default 32768). The driver overrides
     * these from cs->opts before emit. */
    b->org = 32768;
    b->headerless = false;
    b->autorun = false;
    b->opt_level = 2;
    b->opt_strategy = 1;   /* OPT_STRATEGY_SPEED — config.py default */
}

char *backend_new_asmid(Backend *b) {
    char buf[32];
    snprintf(buf, sizeof(buf), "##ASM%d", b->asmcount);
    b->asmcount++;
    return arena_strdup(b->arena, buf);
}

void backend_free(Backend *b) {
    vec_free(b->memory);
    vec_free(b->at_end);
    hashmap_free(&b->asms);
    hashmap_free(&b->requires_);
    hashmap_free(&b->inits);
    hashmap_free(&b->tmp_labels);
}
