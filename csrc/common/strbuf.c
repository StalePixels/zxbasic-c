/*
 * Dynamic string buffer implementation.
 */
#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRBUF_INIT_CAP 256

static void strbuf_grow(StrBuf *sb, size_t need)
{
    size_t new_cap = sb->cap ? sb->cap : STRBUF_INIT_CAP;
    while (new_cap < need)
        new_cap *= 2;

    sb->data = realloc(sb->data, new_cap);
    if (!sb->data) {
        fprintf(stderr, "strbuf: out of memory (requested %zu bytes)\n", new_cap);
        exit(1);
    }
    sb->cap = new_cap;
}

void strbuf_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void strbuf_init_cap(StrBuf *sb, size_t cap)
{
    sb->data = malloc(cap);
    if (!sb->data && cap > 0) {
        fprintf(stderr, "strbuf: out of memory\n");
        exit(1);
    }
    sb->len = 0;
    sb->cap = cap;
    if (cap > 0)
        sb->data[0] = '\0';
}

void strbuf_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void strbuf_clear(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data && sb->cap > 0)
        sb->data[0] = '\0';
}

void strbuf_append(StrBuf *sb, const char *s)
{
    if (!s) return;
    size_t slen = strlen(s);
    if (slen == 0) return;
    strbuf_append_n(sb, s, slen);
}

void strbuf_append_n(StrBuf *sb, const char *s, size_t n)
{
    size_t need = sb->len + n + 1;
    if (need > sb->cap)
        strbuf_grow(sb, need);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void strbuf_append_char(StrBuf *sb, char c)
{
    size_t need = sb->len + 2;
    if (need > sb->cap)
        strbuf_grow(sb, need);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void strbuf_vprintf(StrBuf *sb, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);

    /* Try to fit in remaining space */
    size_t avail = sb->cap > sb->len ? sb->cap - sb->len : 0;
    int n = vsnprintf(sb->data ? sb->data + sb->len : NULL, avail, fmt, ap);

    if (n < 0) {
        va_end(ap2);
        return; /* encoding error */
    }

    if ((size_t)n >= avail) {
        strbuf_grow(sb, sb->len + (size_t)n + 1);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    }

    sb->len += (size_t)n;
    va_end(ap2);
}

void strbuf_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    strbuf_vprintf(sb, fmt, ap);
    va_end(ap);
}

const char *strbuf_cstr(const StrBuf *sb)
{
    if (!sb->data)
        return "";
    return sb->data;
}

char *strbuf_detach(StrBuf *sb)
{
    char *result = sb->data;
    if (!result) {
        result = malloc(1);
        if (result)
            result[0] = '\0';
    }
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return result;
}
