/*
 * Arena memory allocator.
 *
 * Allocates memory in large blocks and hands out pointers into them.
 * All memory is freed at once when the arena is destroyed — no individual
 * free() calls needed. This matches the compiler use case: allocate
 * everything during compilation, free everything at the end.
 */
#ifndef ZXBASIC_ARENA_H
#define ZXBASIC_ARENA_H

#include <stddef.h>

#define ARENA_DEFAULT_BLOCK_SIZE (64 * 1024) /* 64 KiB blocks */

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t size;
    size_t used;
    char data[];  /* flexible array member */
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *head;         /* current block */
    size_t default_block_size;
} Arena;

/* Initialize an arena with the given block size (0 = default) */
void arena_init(Arena *a, size_t block_size);

/* Allocate n bytes from the arena (8-byte aligned) */
void *arena_alloc(Arena *a, size_t n);

/* Convenience: allocate and zero-fill */
void *arena_calloc(Arena *a, size_t count, size_t size);

/* Duplicate a string into the arena */
char *arena_strdup(Arena *a, const char *s);

/* Duplicate n bytes of a string into the arena (null-terminated) */
char *arena_strndup(Arena *a, const char *s, size_t n);

/* Free all memory in the arena */
void arena_destroy(Arena *a);

/* Reset the arena (free blocks but keep the arena struct usable) */
void arena_reset(Arena *a);

#endif /* ZXBASIC_ARENA_H */
