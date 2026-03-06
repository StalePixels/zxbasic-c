/*
 * String-keyed hash map implementation.
 * Open addressing with linear probing and FNV-1a hash.
 */
#include "hashmap.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define HASHMAP_INIT_CAP 32
#define HASHMAP_LOAD_FACTOR 0.75

/* FNV-1a hash */
static unsigned int hash_string(const char *s)
{
    unsigned int h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void hashmap_resize(HashMap *m, int new_cap);

void hashmap_init(HashMap *m)
{
    hashmap_init_cap(m, HASHMAP_INIT_CAP);
}

void hashmap_init_cap(HashMap *m, int cap)
{
    if (cap < 8) cap = 8;
    m->entries = calloc((size_t)cap, sizeof(HashEntry));
    if (!m->entries) {
        fprintf(stderr, "hashmap: out of memory\n");
        exit(1);
    }
    m->count = 0;
    m->capacity = cap;
}

void hashmap_free(HashMap *m)
{
    if (!m->entries) return;
    for (int i = 0; i < m->capacity; i++) {
        if (m->entries[i].occupied) {
            free(m->entries[i].key);
        }
    }
    free(m->entries);
    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
}

static int find_slot(const HashEntry *entries, int cap, const char *key)
{
    unsigned int h = hash_string(key);
    int idx = (int)(h % (unsigned int)cap);
    while (entries[idx].occupied) {
        if (strcmp(entries[idx].key, key) == 0)
            return idx;
        idx = (idx + 1) % cap;
    }
    return idx;
}

static void hashmap_resize(HashMap *m, int new_cap)
{
    HashEntry *old = m->entries;
    int old_cap = m->capacity;

    m->entries = calloc((size_t)new_cap, sizeof(HashEntry));
    if (!m->entries) {
        fprintf(stderr, "hashmap: out of memory during resize\n");
        exit(1);
    }
    m->capacity = new_cap;
    m->count = 0;

    for (int i = 0; i < old_cap; i++) {
        if (old[i].occupied) {
            int slot = find_slot(m->entries, new_cap, old[i].key);
            m->entries[slot] = old[i];
            m->count++;
        }
    }
    free(old);
}

void *hashmap_set(HashMap *m, const char *key, void *value)
{
    if ((double)(m->count + 1) > (double)m->capacity * HASHMAP_LOAD_FACTOR)
        hashmap_resize(m, m->capacity * 2);

    int slot = find_slot(m->entries, m->capacity, key);
    if (m->entries[slot].occupied) {
        void *old_val = m->entries[slot].value;
        m->entries[slot].value = value;
        return old_val;
    }

    m->entries[slot].key = strdup(key);
    m->entries[slot].value = value;
    m->entries[slot].occupied = true;
    m->count++;
    return NULL;
}

void *hashmap_get(const HashMap *m, const char *key)
{
    if (!m->entries || m->count == 0) return NULL;
    int slot = find_slot(m->entries, m->capacity, key);
    if (m->entries[slot].occupied)
        return m->entries[slot].value;
    return NULL;
}

bool hashmap_has(const HashMap *m, const char *key)
{
    if (!m->entries || m->count == 0) return false;
    int slot = find_slot(m->entries, m->capacity, key);
    return m->entries[slot].occupied;
}

void *hashmap_remove(HashMap *m, const char *key)
{
    if (!m->entries || m->count == 0) return NULL;
    int slot = find_slot(m->entries, m->capacity, key);
    if (!m->entries[slot].occupied)
        return NULL;

    void *val = m->entries[slot].value;
    free(m->entries[slot].key);
    m->entries[slot].key = NULL;
    m->entries[slot].value = NULL;
    m->entries[slot].occupied = false;
    m->count--;

    /* Re-insert any entries that may have been displaced past this slot */
    int idx = (slot + 1) % m->capacity;
    while (m->entries[idx].occupied) {
        HashEntry tmp = m->entries[idx];
        m->entries[idx].key = NULL;
        m->entries[idx].value = NULL;
        m->entries[idx].occupied = false;
        m->count--;

        int new_slot = find_slot(m->entries, m->capacity, tmp.key);
        m->entries[new_slot] = tmp;
        m->count++;

        idx = (idx + 1) % m->capacity;
    }

    return val;
}

void hashmap_foreach(const HashMap *m, hashmap_iter_fn fn, void *userdata)
{
    if (!m->entries) return;
    for (int i = 0; i < m->capacity; i++) {
        if (m->entries[i].occupied) {
            if (!fn(m->entries[i].key, m->entries[i].value, userdata))
                return;
        }
    }
}
