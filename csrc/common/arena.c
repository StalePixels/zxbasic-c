/*
 * Arena memory allocator implementation.
 */
#include "arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGNMENT 8

static ArenaBlock *block_new(size_t min_size)
{
    ArenaBlock *b = malloc(sizeof(ArenaBlock) + min_size);
    if (!b) {
        fprintf(stderr, "arena: out of memory (requested %zu bytes)\n", min_size);
        exit(1);
    }
    b->next = NULL;
    b->size = min_size;
    b->used = 0;
    return b;
}

void arena_init(Arena *a, size_t block_size)
{
    a->default_block_size = block_size ? block_size : ARENA_DEFAULT_BLOCK_SIZE;
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t n)
{
    n = ALIGN_UP(n, ALIGNMENT);
    if (n == 0)
        n = ALIGNMENT;

    /* Check if current block has space */
    if (a->head && (a->head->size - a->head->used) >= n) {
        void *ptr = a->head->data + a->head->used;
        a->head->used += n;
        return ptr;
    }

    /* Need a new block */
    size_t block_size = a->default_block_size;
    if (n > block_size)
        block_size = n;

    ArenaBlock *b = block_new(block_size);
    b->next = a->head;
    a->head = b;

    void *ptr = b->data;
    b->used = n;
    return ptr;
}

void *arena_calloc(Arena *a, size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = arena_alloc(a, total);
    memset(ptr, 0, total);
    return ptr;
}

char *arena_strdup(Arena *a, const char *s)
{
    size_t len = strlen(s);
    char *dup = arena_alloc(a, len + 1);
    memcpy(dup, s, len + 1);
    return dup;
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *dup = arena_alloc(a, len + 1);
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

void arena_destroy(Arena *a)
{
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

void arena_reset(Arena *a)
{
    arena_destroy(a);
}
