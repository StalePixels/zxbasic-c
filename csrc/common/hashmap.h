/*
 * Simple string-keyed hash map.
 *
 * Uses open addressing with linear probing. Keys are strings,
 * values are void pointers. Suitable for symbol tables, macro
 * definition tables, include-once tracking, etc.
 */
#ifndef ZXBASIC_HASHMAP_H
#define ZXBASIC_HASHMAP_H

#include <stddef.h>
#include <stdbool.h>

typedef struct HashEntry {
    char *key;         /* owned copy of key string, NULL = empty slot */
    void *value;
    bool occupied;
} HashEntry;

typedef struct HashMap {
    HashEntry *entries;
    int count;
    int capacity;
} HashMap;

/* Initialize a hash map */
void hashmap_init(HashMap *m);

/* Initialize with a given capacity hint */
void hashmap_init_cap(HashMap *m, int cap);

/* Free all entries and the map itself */
void hashmap_free(HashMap *m);

/* Set key=value. Returns previous value or NULL. Key is copied. */
void *hashmap_set(HashMap *m, const char *key, void *value);

/* Get value for key. Returns NULL if not found. */
void *hashmap_get(const HashMap *m, const char *key);

/* Check if key exists */
bool hashmap_has(const HashMap *m, const char *key);

/* Remove a key. Returns the removed value or NULL. */
void *hashmap_remove(HashMap *m, const char *key);

/* Iteration callback: return false to stop iterating */
typedef bool (*hashmap_iter_fn)(const char *key, void *value, void *userdata);

/* Iterate over all entries */
void hashmap_foreach(const HashMap *m, hashmap_iter_fn fn, void *userdata);

#endif /* ZXBASIC_HASHMAP_H */
