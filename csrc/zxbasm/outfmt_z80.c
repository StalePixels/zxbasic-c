/*
 * outfmt_z80.c — Faithful 48K v1 RLE-compressed .z80 snapshot emitter
 *                (C port)
 *
 * Verbatim port of:
 *   src/outfmt/z80.py          Z80Emitter.generate / emit
 *
 * The shared 48K memory-image builder (GenSnapshot, gensnapshot.py:99-256)
 * lives in gensnapshot.{c,h} so the .sna and .z80 emitters can share it.
 * This translation unit owns only the .z80-specific emission: it builds
 * the image via gensnapshot_init(), then assembles the 30-byte v1 .z80
 * register header (z80.py:97-127), then emits the RLE-compressed 49152
 * byte memory stream (z80.py:131-167 — the byte-critical core, ported
 * line-for-line, do not paraphrase the index/runlength bookkeeping) and
 * the unconditional 00 ED ED 00 end marker. Do not alter the header
 * tuple order, the SP handling (UN-adjusted, unlike .sna), or the RLE
 * loop — they are byte-load-bearing.
 */
#include "outfmt_z80.h"

#include "gensnapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Z80Emitter.generate / emit (z80.py). loader_bytes / program_name /
 * aux blocks are ignored for .z80:
 *   output = self.generate(None, entry_point - 1, entry_point,
 *                          program_bytes)
 *   open(output_filename, "wb").write(output)
 */
int outfmt_z80_write(const char *filename,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len)
{
    GenSnapshot gs;
    int clear_addr = entry_point - 1;
    int mc_addr = entry_point;
    const unsigned char *mc_bytes =
        (program_bytes && program_len > 0) ? program_bytes : NULL;
    int mc_len = (program_bytes && program_len > 0) ? program_len : 0;

    if (gensnapshot_init(&gs, clear_addr, mc_addr, mc_bytes, mc_len) != 0) {
        return -1;
    }

    /* Growable output buffer. The compressed stream can in the worst
     * case expand: a 49152-byte memory of strictly alternating bytes
     * never enters a run, and every other byte being 0xED would emit
     * two bytes per source byte. Worst case is therefore well under
     * 30 + 2*49152 + 4. Allocate generously and never realloc. */
    size_t cap = 30 + (size_t)49152 * 2 + 4 + 16;
    unsigned char *out = (unsigned char *)malloc(cap);
    if (!out) {
        return -1;
    }
    size_t n = 0;

    /*
     * 30-byte v1 header (z80.py:97-127), in exact order.
     *
     * vs .sna: PC is a real header field here (gs.PCL/gs.PCH); SP is
     * written UN-adjusted (gs.SPL/gs.SPH directly, NO -2); IFF emitted
     * as 0/1; IM masked with 3. flags bit5 always set (default 0x2E).
     */
    out[n++] = (unsigned char)gs.A;
    out[n++] = (unsigned char)gs.F;
    out[n++] = (unsigned char)gs.C;
    out[n++] = (unsigned char)gs.B;
    out[n++] = (unsigned char)gs.L;
    out[n++] = (unsigned char)gs.H;
    out[n++] = (unsigned char)gs.PCL;
    out[n++] = (unsigned char)gs.PCH;
    out[n++] = (unsigned char)gs.SPL;
    out[n++] = (unsigned char)gs.SPH;
    out[n++] = (unsigned char)gs.I;
    out[n++] = (unsigned char)(gs.R & 0x7F);
    out[n++] = (unsigned char)(((gs.R & 0x80) >> 7) |
                               ((gs.outFE & 7) << 1) | 0x20);
    out[n++] = (unsigned char)gs.E;
    out[n++] = (unsigned char)gs.D;
    out[n++] = (unsigned char)gs.C2;
    out[n++] = (unsigned char)gs.B2;
    out[n++] = (unsigned char)gs.E2;
    out[n++] = (unsigned char)gs.D2;
    out[n++] = (unsigned char)gs.L2;
    out[n++] = (unsigned char)gs.H2;
    out[n++] = (unsigned char)gs.A2;
    out[n++] = (unsigned char)gs.F2;
    out[n++] = (unsigned char)gs.IYL;
    out[n++] = (unsigned char)gs.IYH;
    out[n++] = (unsigned char)gs.IXL;
    out[n++] = (unsigned char)gs.IXH;
    out[n++] = (unsigned char)(gs.IFF1 ? 1 : 0);
    out[n++] = (unsigned char)(gs.IFF2 ? 1 : 0);
    out[n++] = (unsigned char)(gs.IM & 3);

    /*
     * RLE-compressed 49152-byte memory stream — verbatim port of
     * z80.py:131-167. The index/runlength bookkeeping is byte-critical;
     * this is transcribed line-for-line, not paraphrased.
     */
    {
        int idx = 0;
        int runlength = 0;
        int b = -1;

        while (1) {
            if (idx == 49152) {
                break;
            }

            b = gs.mem[idx];
            idx += 1;
            if (idx != 49152 && b == gs.mem[idx]) {
                /* Repetition found */
                runlength = 1;

                /* Find the end of this run */
                while (idx != 49152 && runlength != 255) {
                    if (b != gs.mem[idx]) {
                        break;
                    }
                    idx += 1;
                    runlength += 1;
                }

                if (runlength < 5 && b != 0xED) {
                    /* Doesn't qualify for compression */
                    int k;
                    for (k = 0; k < runlength; k++) {
                        out[n++] = (unsigned char)b;
                    }
                } else {
                    /* Must compress */
                    out[n++] = 0xED;
                    out[n++] = 0xED;
                    out[n++] = (unsigned char)runlength;
                    out[n++] = (unsigned char)b;
                }
            } else {
                out[n++] = (unsigned char)b;
                /* Store byte after ED and don't consider it for run
                 * length */
                if (b == 0xED && idx != 49152) {
                    out[n++] = (unsigned char)gs.mem[idx];
                    idx += 1;
                }
            }
        }
    }

    /* End marker: 00 ED ED 00 (unconditional). */
    out[n++] = 0x00;
    out[n++] = 0xED;
    out[n++] = 0xED;
    out[n++] = 0x00;

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open output file: %s\n", filename);
        free(out);
        return -1;
    }
    if (fwrite(out, 1, n, f) != n) {
        fclose(f);
        free(out);
        return -1;
    }
    fclose(f);
    free(out);
    return 0;
}
