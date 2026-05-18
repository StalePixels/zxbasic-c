/*
 * outfmt_sna.c — Faithful 48K .sna snapshot emitter (C port)
 *
 * Verbatim port of:
 *   src/outfmt/sna.py          SnaEmitter.generate / emit
 *
 * The shared 48K memory-image builder (GenSnapshot, gensnapshot.py:99-256)
 * lives in gensnapshot.{c,h} so the .sna and .z80 emitters can share it.
 * This translation unit owns only the .sna-specific emission: it builds
 * the image via gensnapshot_init(), then assembles the 27-byte .SNA
 * register header, appends the memory image, pokes PC onto the stack and
 * writes the file. Do not alter the header tuple order / SP computation /
 * PC-poke offsets — they are byte-load-bearing.
 */
#include "outfmt_sna.h"

#include "gensnapshot.h"

#include <stdio.h>
#include <string.h>

/*
 * SnaEmitter.generate / emit (sna.py). loader_bytes / program_name /
 * aux blocks are ignored for .sna.
 */
int outfmt_sna_write(const char *filename,
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

    /* SNA stores the start address in the stack, so SP is adjusted. */
    int SP = ((gs.SPH << 8) | gs.SPL) - 2;

    /* The 27-byte .sna register header, in exact order. */
    unsigned char header[27];
    header[0] = (unsigned char)gs.I;
    header[1] = (unsigned char)gs.L2;
    header[2] = (unsigned char)gs.H2;
    header[3] = (unsigned char)gs.E2;
    header[4] = (unsigned char)gs.D2;
    header[5] = (unsigned char)gs.C2;
    header[6] = (unsigned char)gs.B2;
    header[7] = (unsigned char)gs.F2;
    header[8] = (unsigned char)gs.A2;
    header[9] = (unsigned char)gs.L;
    header[10] = (unsigned char)gs.H;
    header[11] = (unsigned char)gs.E;
    header[12] = (unsigned char)gs.D;
    header[13] = (unsigned char)gs.C;
    header[14] = (unsigned char)gs.B;
    header[15] = (unsigned char)gs.IYL;
    header[16] = (unsigned char)gs.IYH;
    header[17] = (unsigned char)gs.IXL;
    header[18] = (unsigned char)gs.IXH;
    header[19] = (unsigned char)(gs.IFF1 ? 4 : 0);
    header[20] = (unsigned char)gs.R;
    header[21] = (unsigned char)gs.F;
    header[22] = (unsigned char)gs.A;
    header[23] = (unsigned char)(SP & 0xFF);
    header[24] = (unsigned char)((SP >> 8) & 0xFF);
    header[25] = (unsigned char)gs.IM;
    header[26] = (unsigned char)(gs.outFE & 7);

    const int snaHeaderLen = 27;

    /* Poke PC onto the stack inside the memory image.
     * sna_data[SP - 16384 + 0 + 27] = PCL
     * sna_data[SP - 16384 + 1 + 27] = PCH
     * Both indices are into the FULL sna_data; since the header is the
     * first 27 bytes, the offsets within mem[] are (SP - 16384) and
     * (SP - 16384 + 1). */
    {
        long idx = (long)SP - 16384;
        if (idx < 0 || idx + 1 >= 49152) {
            return -1;
        }
        gs.mem[idx + 0] = (unsigned char)gs.PCL;
        gs.mem[idx + 1] = (unsigned char)gs.PCH;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open output file: %s\n", filename);
        return -1;
    }
    if (fwrite(header, 1, (size_t)snaHeaderLen, f) != (size_t)snaHeaderLen ||
        fwrite(gs.mem, 1, sizeof(gs.mem), f) != sizeof(gs.mem)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
