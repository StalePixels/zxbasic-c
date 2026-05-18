/*
 * outfmt_tap.c — Faithful .tap (TAP tape) emitter (C port)
 *
 * Byte-for-byte port of:
 *   - src/outfmt/tap.py        (TAP class: empty output, standard_block override)
 *   - src/outfmt/tzx.py        (LH, out, save_header, standard_bytes_header,
 *                               save_code, emit — loader-less path)
 *
 * S6.3a is the LOADER-LESS CORE ONLY: no basic.Basic() loader and no aux
 * blocks (aux_bin_blocks / aux_headless_bin_blocks need bridge plumbing —
 * that is the separate S6.3b sprint). Python's TAP.emit() with
 * loader_bytes is None and no aux blocks reduces to exactly:
 *
 *     save_code(program_name, entry_point, program_bytes)
 *     dump(output_filename)
 *
 * Self-contained: a local growable byte buffer (malloc/realloc), freed
 * before return. No new shared dependencies.
 */
#include "outfmt_tap.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ZX Spectrum block / header type constants (tzx.py:22-32). */
#define TAP_BLOCK_TYPE_HEADER 0x00 /* BLOCK_TYPE_HEADER */
#define TAP_BLOCK_TYPE_DATA   0xFF /* BLOCK_TYPE_DATA */
#define TAP_HEADER_TYPE_BASIC 0x00 /* HEADER_TYPE_BASIC (PROGRAM) */
#define TAP_HEADER_TYPE_CODE  0x03 /* HEADER_TYPE_CODE */

/* ----------------------------------------------------------------
 * Growable byte buffer (self-contained — no shared deps).
 * Mirrors Python's self.output = bytearray().
 * ---------------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    int oom; /* sticky out-of-memory flag */
} TapBuf;

static void tapbuf_init(TapBuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
}

static void tapbuf_free(TapBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static void tapbuf_push(TapBuf *b, unsigned char byte)
{
    if (b->oom) return;
    if (b->len == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 256;
        unsigned char *nd = (unsigned char *)realloc(b->data, ncap);
        if (!nd) {
            b->oom = 1;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
    b->data[b->len++] = byte;
}

/* ----------------------------------------------------------------
 * Primitives (faithful to tzx.py / tap.py)
 * ---------------------------------------------------------------- */

/* LH(value) -> 2 bytes little-endian: [ value & 0xFF, (value & 0xFF00) >> 8 ]
 * (tzx.py:40-45). Emitted directly into the buffer. */
static void tap_out_LH(TapBuf *b, int value)
{
    tapbuf_push(b, (unsigned char)(value & 0x00FF));
    tapbuf_push(b, (unsigned char)((value & 0xFF00) >> 8));
}

/* TAP standard_block override (tap.py:21-32):
 *   out(LH(len(payload) + 1))            ; + 1 for CHECKSUM byte
 *   checksum = 0
 *   for i in payload: checksum ^= i & 0xFF; out(i)
 *   out(checksum)
 * NOTE: the TAP override does NOT emit the TZX 0x10 block id / 1000ms
 * pause that tzx.py:54-65 does — that is TZX-only. */
static void tap_standard_block(TapBuf *b,
                               const unsigned char *payload,
                               size_t payload_len)
{
    tap_out_LH(b, (int)(payload_len + 1)); /* + 1 for CHECKSUM byte */

    unsigned char checksum = 0;
    for (size_t i = 0; i < payload_len; i++) {
        unsigned char v = (unsigned char)(payload[i] & 0xFF);
        checksum ^= v;
        tapbuf_push(b, v);
    }
    tapbuf_push(b, checksum);
}

/* save_header(_type, title, length, param1, param2) (tzx.py:72-99):
 *   title = (title + "          ")[:10]   ; pad to >=10 then truncate to 10
 *   payload = [BLOCK_TYPE_HEADER, _type] + [ord(c) for c in title10]
 *             + LH(length) + LH(param1) + LH(param2)
 *   standard_block(payload)               ; payload length = 2 + 10 + 6 = 18
 * The 10-char title window is space-padded: Python does (title + 10*" ")[:10],
 * so any char position past the C string's NUL becomes a space (0x20). */
static void tap_save_header(TapBuf *b, unsigned char _type,
                            const char *title, int length,
                            int param1, int param2)
{
    unsigned char payload[18];
    size_t n = 0;

    payload[n++] = TAP_BLOCK_TYPE_HEADER;
    payload[n++] = _type;

    /* Exactly 10 title bytes, space-padded (Python (title + "          ")[:10]).
     * Stop copying real chars at the first NUL; pad the remainder with ' '. */
    int hit_end = 0;
    for (int i = 0; i < 10; i++) {
        unsigned char c;
        if (!hit_end && title && title[i] != '\0') {
            c = (unsigned char)title[i];
        } else {
            hit_end = 1;
            c = (unsigned char)' ';
        }
        payload[n++] = c;
    }

    /* LH(length) + LH(param1) + LH(param2) — inline (buffer is exact size). */
    payload[n++] = (unsigned char)(length & 0x00FF);
    payload[n++] = (unsigned char)((length & 0xFF00) >> 8);
    payload[n++] = (unsigned char)(param1 & 0x00FF);
    payload[n++] = (unsigned char)((param1 & 0xFF00) >> 8);
    payload[n++] = (unsigned char)(param2 & 0x00FF);
    payload[n++] = (unsigned char)((param2 & 0xFF00) >> 8);

    /* n == 18 here. */
    tap_standard_block(b, payload, n);
}

/* standard_bytes_header(title, addr, length) (tzx.py:101-103):
 *   save_header(HEADER_TYPE_CODE, title, length, param1=addr, param2=32768) */
static void tap_standard_bytes_header(TapBuf *b, const char *title,
                                      int addr, int length)
{
    tap_save_header(b, TAP_HEADER_TYPE_CODE, title, length,
                    /*param1=*/addr, /*param2=*/32768);
}

/* save_code(title, addr, code_bytes) (tzx.py:109-115):
 *   standard_bytes_header(title, addr, len(code_bytes))
 *   payload = [BLOCK_TYPE_DATA] + [b & 0xFF for b in code_bytes]
 *   standard_block(payload) */
static void tap_save_code(TapBuf *b, const char *title, int addr,
                          const unsigned char *code_bytes, int code_len)
{
    tap_standard_bytes_header(b, title, addr, code_len);

    size_t payload_len = (size_t)code_len + 1; /* +1 for BLOCK_TYPE_DATA */
    unsigned char *payload = (unsigned char *)malloc(payload_len);
    if (!payload) {
        b->oom = 1;
        return;
    }
    payload[0] = TAP_BLOCK_TYPE_DATA;
    for (int i = 0; i < code_len; i++) {
        payload[i + 1] = (unsigned char)(code_bytes[i] & 0xFF);
    }
    tap_standard_block(b, payload, payload_len);
    free(payload);
}

/* standard_program_header(title, length, line) (tzx.py:105-107):
 *   save_header(HEADER_TYPE_BASIC, title, length, param1=line, param2=length)
 * NOTE param2 = length here (NOT 32768 like the CODE header). */
static void tap_standard_program_header(TapBuf *b, const char *title,
                                        int length, int line)
{
    tap_save_header(b, TAP_HEADER_TYPE_BASIC, title, length,
                    /*param1=*/line, /*param2=*/length);
}

/* save_program(title, prog_bytes, line) (tzx.py:117-121):
 *   standard_program_header(title, len(prog_bytes), line)
 *   payload = [BLOCK_TYPE_DATA] + [b & 0xFF for b in prog_bytes]
 *   standard_block(payload) */
static void tap_save_program(TapBuf *b, const char *title,
                             const unsigned char *prog_bytes,
                             int prog_len, int line)
{
    tap_standard_program_header(b, title, prog_len, line);

    size_t payload_len = (size_t)prog_len + 1; /* +1 for BLOCK_TYPE_DATA */
    unsigned char *payload = (unsigned char *)malloc(payload_len);
    if (!payload) {
        b->oom = 1;
        return;
    }
    payload[0] = TAP_BLOCK_TYPE_DATA;
    for (int i = 0; i < prog_len; i++) {
        payload[i + 1] = (unsigned char)(prog_bytes[i] & 0xFF);
    }
    tap_standard_block(b, payload, payload_len);
    free(payload);
}

/* ----------------------------------------------------------------
 * Public entry — faithful TAP.emit() (tzx.py:123-143, no aux blocks):
 *   if loader_bytes is not None:
 *       save_program("loader", loader_bytes, line=1)
 *   save_code(program_name, entry_point, program_bytes)
 *   dump(output_filename)
 * ---------------------------------------------------------------- */
int outfmt_tap_write_loader(const char *filename,
                            const char *program_name,
                            int entry_point,
                            const unsigned char *loader_bytes,
                            int loader_len,
                            const unsigned char *program_bytes,
                            int program_len)
{
    TapBuf b;
    tapbuf_init(&b); /* TAP.__init__: self.output = bytearray() (empty) */

    if (program_len < 0) program_len = 0;

    /* loader_bytes is not None  -> save_program("loader", ..., line=1).
     * NULL (or negative length) means Python's loader_bytes is None. */
    if (loader_bytes != NULL && loader_len >= 0) {
        tap_save_program(&b, "loader", loader_bytes, loader_len, /*line=*/1);
    }

    tap_save_code(&b, program_name, entry_point, program_bytes, program_len);

    if (b.oom) {
        tapbuf_free(&b);
        return -1;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        tapbuf_free(&b);
        return -1;
    }
    if (b.len > 0) {
        fwrite(b.data, 1, b.len, f);
    }
    fclose(f);

    tapbuf_free(&b);
    return 0;
}

/* ----------------------------------------------------------------
 * Public entry — faithful loader-less TAP.emit() (tzx.py:123-143
 * with loader_bytes is None and no aux blocks). Byte-identical to
 * S6.3a: delegates to the loader-aware path with no loader.
 * ---------------------------------------------------------------- */
int outfmt_tap_write(const char *filename,
                     const char *program_name,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len)
{
    return outfmt_tap_write_loader(filename, program_name, entry_point,
                                   /*loader_bytes=*/NULL, /*loader_len=*/-1,
                                   program_bytes, program_len);
}
