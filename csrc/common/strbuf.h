/*
 * Dynamic string buffer.
 *
 * Growable byte buffer for building strings incrementally.
 * Optionally backed by an arena allocator for automatic cleanup.
 */
#ifndef ZXBASIC_STRBUF_H
#define ZXBASIC_STRBUF_H

#include <stddef.h>
#include <stdarg.h>
#include "compat.h"

typedef struct StrBuf {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

/* Initialize a string buffer (heap-allocated, caller must call strbuf_free) */
void strbuf_init(StrBuf *sb);

/* Initialize with a given initial capacity */
void strbuf_init_cap(StrBuf *sb, size_t cap);

/* Free the buffer */
void strbuf_free(StrBuf *sb);

/* Reset length to 0 without freeing (reuse buffer) */
void strbuf_clear(StrBuf *sb);

/* Append a string */
void strbuf_append(StrBuf *sb, const char *s);

/* Append n bytes */
void strbuf_append_n(StrBuf *sb, const char *s, size_t n);

/* Append a single character */
void strbuf_append_char(StrBuf *sb, char c);

/* Append formatted string (printf-style) */
void strbuf_printf(StrBuf *sb, const char *fmt, ...) PRINTF_FMT(2, 3);

/* Append formatted string (va_list version) */
void strbuf_vprintf(StrBuf *sb, const char *fmt, va_list ap);

/* Get a null-terminated C string (valid until next mutation) */
const char *strbuf_cstr(const StrBuf *sb);

/* Detach and return the buffer (caller must free). Resets the StrBuf. */
char *strbuf_detach(StrBuf *sb);

#endif /* ZXBASIC_STRBUF_H */
