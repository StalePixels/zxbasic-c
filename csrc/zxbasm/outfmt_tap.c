/*
 * outfmt_tap.c — Faithful .tap (TAP tape) AND .tzx (TZX tape) emitter
 *                shared core (C port)
 *
 * Byte-for-byte port of:
 *   - src/outfmt/tzx.py        (TZX base class: "ZXTape!" preamble,
 *                               standard_block with 0x10 id + 1000ms pause,
 *                               LH, out, save_header, standard_bytes_header,
 *                               standard_program_header, save_code,
 *                               save_program, emit)
 *   - src/outfmt/tap.py        (TAP(TZX) subclass: overrides EXACTLY two
 *                               things — empty output (no preamble), and a
 *                               standard_block with no 0x10 id / no pause)
 *
 * Python design: `class TAP(TZX)`. TZX is the BASE class; TAP overrides
 * only __init__ (empty output) and standard_block (LH+payload+checksum,
 * no block-id / no pause). EVERYTHING ELSE IS INHERITED AND SHARED.
 *
 * This file therefore carries the WHOLE shared machinery once, with the
 * TAP/TZX variance localised to exactly two points, gated by an `is_tzx`
 * mode flag threaded through the buffer:
 *
 *   (1) init     — TZX seeds the buffer with the 10-byte preamble
 *                   "ZXTape!" 0x1A 0x01 0x15; TAP starts empty.
 *   (2) standard_block — TZX prepends 0x10 + LH(1000); TAP does not.
 *
 * The TAP path (is_tzx == 0) executes byte-for-byte the same code as
 * before this refactor: tapbuf_init with is_tzx==0 writes nothing, and
 * tap_standard_block with is_tzx==0 skips the 0x10/pause prefix — so the
 * S6.3 .tap output is provably unchanged.
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

/* TZX-only constants (tzx.py:16-22). Only consulted when is_tzx != 0;
 * the TAP path never reads these, keeping .tap byte-identical. */
#define TZX_VERSION_MAJOR  1    /* tzx.py:16 */
#define TZX_VERSION_MINOR  21   /* tzx.py:17 */
#define TZX_BLOCK_STANDARD 0x10 /* tzx.py:22 BLOCK_STANDARD */
#define TZX_STANDARD_PAUSE 1000 /* tzx.py:57 LH(1000) — 1000 ms pause */

/* ----------------------------------------------------------------
 * Growable byte buffer (self-contained — no shared deps).
 * Mirrors Python's self.output = bytearray().
 * ---------------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
    int oom;    /* sticky out-of-memory flag */
    int is_tzx; /* 0 == TAP (tap.py overrides), 1 == TZX (tzx.py base) */
} TapBuf;

static void tapbuf_push(TapBuf *b, unsigned char byte); /* fwd decl */

/* TAP.__init__ (tap.py:16-19): super().__init__() then self.output =
 * bytearray() — i.e. the TZX preamble is built then DISCARDED, so the
 * buffer is empty. TZX.__init__ (tzx.py:34-38): self.output =
 * bytearray(b"ZXTape!"); out(0x1A); out([VERSION_MAJOR, VERSION_MINOR]).
 *
 * is_tzx == 0 writes nothing here (byte-identical to the pre-refactor
 * TAP init). is_tzx == 1 writes the 10-byte TZX preamble:
 *   5A 58 54 61 70 65 21 1A 01 15
 */
static void tapbuf_init(TapBuf *b, int is_tzx)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = 0;
    b->is_tzx = is_tzx;

    if (is_tzx) {
        /* bytearray(b"ZXTape!") */
        static const unsigned char zxtape[7] = {
            0x5A, 0x58, 0x54, 0x61, 0x70, 0x65, 0x21 /* "ZXTape!" */
        };
        for (size_t i = 0; i < sizeof(zxtape); i++) {
            tapbuf_push(b, zxtape[i]);
        }
        tapbuf_push(b, 0x1A);                              /* out(0x1A) */
        tapbuf_push(b, (unsigned char)TZX_VERSION_MAJOR);  /* 0x01 */
        tapbuf_push(b, (unsigned char)TZX_VERSION_MINOR);  /* 0x15 */
    }
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

/* standard_block — the ONE method TAP overrides from TZX, here unified
 * with an is_tzx gate so the two forms share one body:
 *
 *   TZX.standard_block (tzx.py:54-65):
 *     out(BLOCK_STANDARD)              ; 0x10 standard block id
 *     out(LH(1000))                    ; 1000 ms standard pause
 *     out(LH(len(payload) + 1))        ; + 1 for CHECKSUM byte
 *     checksum = 0
 *     for i in payload: checksum ^= i & 0xFF; out(i)
 *     out(checksum)
 *
 *   TAP.standard_block (tap.py:21-32) — overrides, dropping the first
 *   two lines (no 0x10, no pause):
 *     out(LH(len(payload) + 1))        ; + 1 for CHECKSUM byte
 *     checksum = 0
 *     for i in payload: checksum ^= i & 0xFF; out(i)
 *     out(checksum)
 *
 * is_tzx == 0 reproduces the pre-refactor TAP body EXACTLY (the 0x10 /
 * pause prefix is skipped), so .tap output is provably unchanged. */
static void tap_standard_block(TapBuf *b,
                               const unsigned char *payload,
                               size_t payload_len)
{
    if (b->is_tzx) {
        tapbuf_push(b, (unsigned char)TZX_BLOCK_STANDARD); /* 0x10 */
        tap_out_LH(b, TZX_STANDARD_PAUSE);                 /* LH(1000) */
    }

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
 * Shared emit core — faithful (TZX|TAP).emit() (tzx.py:123-143, no aux
 * blocks; aux_bin_blocks / aux_headless_bin_blocks remain out of scope
 * for both formats — separate bridge plumbing):
 *
 *   if loader_bytes is not None:
 *       save_program("loader", loader_bytes, line=1)
 *   save_code(program_name, entry_point, program_bytes)
 *   dump(output_filename)
 *
 * is_tzx selects the two-point TAP/TZX variance (preamble in init,
 * 0x10/pause in standard_block); EVERYTHING ELSE is the shared TZX
 * machinery, inherited by TAP in the Python. The is_tzx == 0 call path
 * is byte-identical to the pre-S6.4 TAP emitter.
 *
 * Declared in outfmt_tap.h so outfmt_tzx.c can call it with is_tzx==1.
 * ---------------------------------------------------------------- */
int outfmt_tape_emit(int is_tzx,
                     const char *filename,
                     const char *program_name,
                     int entry_point,
                     const unsigned char *loader_bytes,
                     int loader_len,
                     const unsigned char *program_bytes,
                     int program_len)
{
    TapBuf b;
    tapbuf_init(&b, is_tzx); /* TAP: empty; TZX: "ZXTape!" preamble. */

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
 * Public entry — faithful TAP.emit() (tzx.py:123-143, no aux blocks).
 * Thin wrapper over the shared core with is_tzx == 0 — byte-identical
 * to the pre-S6.4 implementation.
 * ---------------------------------------------------------------- */
int outfmt_tap_write_loader(const char *filename,
                            const char *program_name,
                            int entry_point,
                            const unsigned char *loader_bytes,
                            int loader_len,
                            const unsigned char *program_bytes,
                            int program_len)
{
    return outfmt_tape_emit(/*is_tzx=*/0, filename, program_name, entry_point,
                            loader_bytes, loader_len,
                            program_bytes, program_len);
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
