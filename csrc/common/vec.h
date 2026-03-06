/*
 * Type-safe dynamic array (vector) via macros.
 *
 * Usage:
 *   VEC(int) numbers;
 *   vec_init(numbers);
 *   vec_push(numbers, 42);
 *   printf("%d\n", numbers.data[0]);
 *   vec_free(numbers);
 */
#ifndef ZXBASIC_VEC_H
#define ZXBASIC_VEC_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define VEC(T) struct { T *data; int len; int cap; }

#define vec_init(v) do { (v).data = NULL; (v).len = 0; (v).cap = 0; } while(0)

#define vec_free(v) do { free((v).data); (v).data = NULL; (v).len = (v).cap = 0; } while(0)

#define vec_clear(v) do { (v).len = 0; } while(0)

#define vec_grow(v, need) do { \
    if ((need) > (v).cap) { \
        int _nc = (v).cap ? (v).cap : 8; \
        while (_nc < (need)) _nc *= 2; \
        (v).data = realloc((v).data, (size_t)_nc * sizeof(*(v).data)); \
        if (!(v).data) { fprintf(stderr, "vec: out of memory\n"); exit(1); } \
        (v).cap = _nc; \
    } \
} while(0)

#define vec_push(v, item) do { \
    vec_grow(v, (v).len + 1); \
    (v).data[(v).len++] = (item); \
} while(0)

#define vec_pop(v) ((v).data[--(v).len])

#define vec_last(v) ((v).data[(v).len - 1])

#define vec_foreach(v, iter) \
    for (int _i = 0; _i < (v).len && ((iter) = (v).data[_i], 1); _i++)

#endif /* ZXBASIC_VEC_H */
