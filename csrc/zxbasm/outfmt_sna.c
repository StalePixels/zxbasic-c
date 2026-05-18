/*
 * outfmt_sna.c — Faithful 48K .sna snapshot emitter (C port)
 *
 * Verbatim port of:
 *   src/outfmt/gensnapshot.py  GenSnapshot.__init__  (memory image)
 *   src/outfmt/sna.py          SnaEmitter.generate / emit
 *
 * The patch ORDER below is byte-load-bearing: later patchAddr() calls
 * overwrite earlier ones. The sequence (and every literal byte block)
 * reproduces gensnapshot.py:99-256 exactly. Do not reorder or "tidy".
 */
#include "outfmt_sna.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Snapshot state — mirrors the GenSnapshot object fields that
 * SnaEmitter.generate reads.
 * ---------------------------------------------------------------- */
typedef struct GenSnapshot {
    unsigned char mem[49152]; /* the 48K RAM image (0x4000..0xFFFF) */

    /* Z80 registers / state (only the ones .sna reads matter for the
     * output; W/Z/cycles/halted/eilast are intentionally omitted as
     * they do not affect the .sna bytes). */
    int A, A2, B, B2, C, C2, D, D2, E, E2, H, H2, L, L2, F, F2;
    int R, IXL, IXH, IYH, IYL, I;
    int IFF1, IFF2;
    int PCH, PCL;
    int SPH, SPL;
    int IM;
    int outFE;
} GenSnapshot;

/* word(x) -> 2 little-endian bytes (x % 256, x >> 8) */
static void gs_word(int x, unsigned char out[2])
{
    out[0] = (unsigned char)(((unsigned)x) % 256);
    out[1] = (unsigned char)(((unsigned)x) >> 8);
}

/*
 * patchAddr(addr, data, len): mem[addr-16384 : addr-16384+len] = data.
 *
 * In Python the bytearray is fixed at 49152 and slice assignment of an
 * equal-length region cannot grow it (the asserts guarantee len stays
 * 49152). For all inputs the caller can produce here the destination
 * window stays within [0, 49152) — but we bounds-check defensively and
 * return non-zero on any out-of-range write so a spec violation fails
 * loudly rather than corrupting memory.
 */
static int gs_patch(GenSnapshot *gs, int addr, const unsigned char *data,
                    int len)
{
    long start = (long)addr - 16384;
    if (len < 0) {
        return -1;
    }
    if (start < 0 || start + (long)len > 49152) {
        return -1;
    }
    if (len > 0) {
        memcpy(gs->mem + start, data, (size_t)len);
    }
    return 0;
}

/* gs_patch with a single repeated fill byte (Python b"\xNN" * count). */
static int gs_patch_fill(GenSnapshot *gs, int addr, unsigned char fill,
                         int count)
{
    long start = (long)addr - 16384;
    if (count < 0) {
        return -1;
    }
    if (start < 0 || start + (long)count > 49152) {
        return -1;
    }
    if (count > 0) {
        memset(gs->mem + start, fill, (size_t)count);
    }
    return 0;
}

/* Verbatim copy of gensnapshot.py:104-167 — the system-variables block
 * patched at 0x5C00. */
static const unsigned char SYSVARS[] = {
    0xff, 0x00, 0x00, 0x00,
    0x0d, 0x02, 0x20, 0x0d, /* KSTATE */
    0x0d,                   /* LAST_K */
    0x23,                   /* REPDEL */
    0x05,                   /* REPPER */
    0x00, 0x00, 0x00, 0x00, 0x00, /* DEFADD, K_DATA, TVDATA */
    0x01, 0x00, 0x06, 0x00, 0x0b, 0x00, 0x01, 0x00, 0x01, 0x00, 0x06, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* STRMS (38 bytes total) */
    0x00, 0x3c,             /* CHARS */
    0x40, 0x00,             /* RASP, PIP */
    0xff,                   /* ERR_NR */
    0xcc,                   /* FLAGS */
    0x00,                   /* TV_FLAG */
    0x00, 0x00,             /* ERR_SP (to be patched with clear addr - 3) */
    0x00, 0x00,             /* LIST_SP (overwritten by ROM) */
    0x00,                   /* MODE */
    0x00, 0x00, 0x00,       /* NEWPPC, NSPPC (at start of BASIC) */
    0xfe, 0xff, 0x01,       /* PPC, SUBPPC (at line -2, edit line) */
    0x38,                   /* BORDCR */
    0x00, 0x00,             /* E_PPC */
    0x00, 0x00,             /* VARS (patched later, depends on prog length) */
    0x00, 0x00,             /* DEST */
    0xb6, 0x5c,             /* CHANS */
    0xb6, 0x5c,             /* CURCHL */
    0xcb, 0x5c,             /* PROG */
    0x00, 0x00,             /* NXTLIN (overwritten by ROM) */
    0xca, 0x5c,             /* DATADD */
    0x00, 0x00,             /* E_LINE (patched later) */
    0x00, 0x00,             /* K_CUR (overwritten by ROM) */
    0x00, 0x00,             /* CH_ADD (overwritten by ROM) */
    0x00, 0x00,             /* X_PTR */
    0x00, 0x00,             /* WORKSP (patched later) */
    0x00, 0x00,             /* STKBOT (patched later) */
    0x00, 0x00,             /* STKEND (patched later) */
    0x00,                   /* BREG */
    0x92, 0x5c,             /* MEM */
    0x10,                   /* FLAGS2 */
    0x02,                   /* DF_SZ */
    0x00, 0x00, 0x00, 0x00, 0x00, /* S_TOP, OLDPPC, OSPPC */
    0x00, 0x00, 0x00,       /* FLAGX, STRLEN */
    0x00, 0x00,             /* T_ADDR (overwritten by ROM) */
    0x00, 0x00,             /* SEED */
    0x00, 0x00, 0x00,       /* FRAMES */
    0x58, 0xff,             /* UDG */
    0x00, 0x00,             /* COORDS */
    0x21,                   /* P_POSN */
    0x00, 0x5b,             /* PR_CC */
    0x21, 0x17,             /* ECHO_E */
    0x00, 0x40,             /* DF_CC */
    0xe0, 0x50,             /* DFCCL */
    0x21, 0x18,             /* S_POSN */
    0x21, 0x17,             /* SPOSNL */
    0x01,                   /* SCR_CT */
    0x38, 0x00, 0x38, 0x00, /* ATTR_P, MASK_P, ATTR_T, MASK_T */
    0x00,                   /* P_FLAG */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* MEMBOT (30 bytes) */
    0x00, 0x00,             /* NMIADD */
    0x00, 0x00,             /* RAMTOP (patched later) */
    0xff, 0xff              /* P_RAMT */
};

/* Verbatim copy of gensnapshot.py:169-180 — the CHANS data. */
static const unsigned char CHANS_DATA[] = {
    0xf4, 0x09, 0xa8, 0x10,
    'K',                    /* PRINT_OUT, KEY_INPUT, "K" */
    0xf4, 0x09, 0xc4, 0x15,
    'S',                    /* PRINT_OUT, REPORT_J, "S" */
    0x81, 0x0f, 0xc4, 0x15,
    'R',                    /* ADD_CHAR, REPORT_J, "R" */
    0xf4, 0x09, 0xc4, 0x15,
    'P',                    /* PRINT_OUT, REPORT_J, "P" */
    0x80                    /* Terminator */
};

/* Verbatim copy of gensnapshot.py:240-252 — the 168-byte UDG glyph
 * table patched at 65368. */
static const unsigned char UDG_TABLE[] = {
    0x00, 0x3c, 0x42, 0x42, 0x7e, 0x42, 0x42, 0x00, 0x00, 0x7c, 0x42, 0x7c, 0x42, 0x42, 0x7c, 0x00,
    0x00, 0x3c, 0x42, 0x40, 0x40, 0x42, 0x3c, 0x00, 0x00, 0x78, 0x44, 0x42, 0x42, 0x44, 0x78, 0x00,
    0x00, 0x7e, 0x40, 0x7c, 0x40, 0x40, 0x7e, 0x00, 0x00, 0x7e, 0x40, 0x7c, 0x40, 0x40, 0x40, 0x00,
    0x00, 0x3c, 0x42, 0x40, 0x4e, 0x42, 0x3c, 0x00, 0x00, 0x42, 0x42, 0x7e, 0x42, 0x42, 0x42, 0x00,
    0x00, 0x3e, 0x08, 0x08, 0x08, 0x08, 0x3e, 0x00, 0x00, 0x02, 0x02, 0x02, 0x42, 0x42, 0x3c, 0x00,
    0x00, 0x44, 0x48, 0x70, 0x48, 0x44, 0x42, 0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7e, 0x00,
    0x00, 0x42, 0x66, 0x5a, 0x42, 0x42, 0x42, 0x00, 0x00, 0x42, 0x62, 0x52, 0x4a, 0x46, 0x42, 0x00,
    0x00, 0x3c, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00, 0x00, 0x7c, 0x42, 0x42, 0x7c, 0x40, 0x40, 0x00,
    0x00, 0x3c, 0x42, 0x42, 0x52, 0x4a, 0x3c, 0x00, 0x00, 0x7c, 0x42, 0x42, 0x7c, 0x44, 0x42, 0x00,
    0x00, 0x3c, 0x40, 0x3c, 0x02, 0x42, 0x3c, 0x00, 0x00, 0xfe, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00,
    0x00, 0x42, 0x42, 0x42, 0x42, 0x42, 0x3c, 0x00
};

/*
 * GenSnapshot.__init__(loader_bytes=None, clear_addr, mc_addr, mc_bytes)
 *
 * Returns 0 on success, -1 if a patch went out of range or the
 * `assert mc_addr + len(mc_bytes) <= 65536` was violated.
 */
static int gs_init(GenSnapshot *gs, int clear_addr, int mc_addr,
                   const unsigned char *mc_bytes, int mc_len)
{
    unsigned char w[2];
    int SP;

    /* Registers / state */
    gs->A = gs->A2 = gs->B = gs->B2 = gs->C = gs->C2 = gs->D = gs->D2 =
        gs->E = gs->E2 = gs->H = gs->H2 = gs->L = gs->L2 = gs->F = gs->F2 =
            gs->R = gs->IXL = gs->IXH = 0;

    gs->IYH = 0x5C;
    gs->IYL = 0x3A; /* 0x5C3A is the normal value of IY for ROM use */
    gs->I = 0x3F;
    gs->IFF1 = 1;
    gs->IFF2 = 1;
    gs->PCH = 0x1B;
    gs->PCL = 0x9E; /* Entry point: 1B9E, LINE_NEW */
    SP = clear_addr - 3;
    gs->SPH = (SP >> 8) & 0xFF;
    gs->SPL = SP & 0xFF;
    gs->IM = 1;
    gs->outFE = 0x0F; /* Border 7, input enabled, speaker disabled */

    /* Build a valid memory image from scratch: 49152 zeros */
    memset(gs->mem, 0, sizeof(gs->mem));

    /* Screen Attributes — all Paper 7 / Ink 0 */
    if (gs_patch_fill(gs, 0x5800, 0x38, 768) != 0) return -1;

    /* System Variables */
    if (gs_patch(gs, 0x5C00, SYSVARS, (int)sizeof(SYSVARS)) != 0) return -1;

    /* CHANS data starts at 23734 */
    if (gs_patch(gs, 23734, CHANS_DATA, (int)sizeof(CHANS_DATA)) != 0)
        return -1;

    /* BASIC start (usually 23755 in absence of Interface 1) */
    int BasicStart = 23734 + (int)sizeof(CHANS_DATA);

    /* loader_bytes is None: create a single line —
     *   10 IF USR <mc_addr> THEN
     *
     * Python:
     *   loader = bytearray(b"\0\x0a\0\0\xfa\xc0")
     *   loader.extend(b"%05d\x0e\0\0\0\0\0" % mc_addr)
     *   loader[-3:-1] = word(mc_addr)
     *   loader.extend(b"\xcb\x0d")
     *   loader[2:4] = word(len(loader) - 4)
     */
    unsigned char loader[32];
    int L = 0;
    loader[L++] = 0x00;
    loader[L++] = 0x0a;            /* BASIC big endian line num */
    loader[L++] = 0x00;
    loader[L++] = 0x00;            /* BASIC little endian line length (patched below) */
    loader[L++] = 0xfa;
    loader[L++] = 0xc0;            /* BASIC IF USR */
    /* b"%05d\x0e\0\0\0\0\0" % mc_addr : 5-digit zero-padded DECIMAL
     * ASCII of mc_addr, then 0x0E,0,0,0,0,0 (11 bytes total). Python's
     * %05d emits at least 5 digits; mc_addr here is a 16-bit address so
     * it is always exactly 5 digits. */
    {
        char dec[16];
        int n = snprintf(dec, sizeof(dec), "%05d", mc_addr);
        if (n < 0) return -1;
        for (int i = 0; i < n; i++) {
            loader[L++] = (unsigned char)dec[i];
        }
        loader[L++] = 0x0e;
        loader[L++] = 0x00;
        loader[L++] = 0x00;
        loader[L++] = 0x00;
        loader[L++] = 0x00;
        loader[L++] = 0x00;
    }
    /* loader[-3:-1] = word(mc_addr) — overwrite the two bytes ending
     * three from the end (the third & second from last bytes). */
    gs_word(mc_addr, w);
    loader[L - 3] = w[0];
    loader[L - 2] = w[1];
    /* loader.extend(b"\xcb\x0d") — THEN + final newline */
    loader[L++] = 0xcb;
    loader[L++] = 0x0d;
    /* loader[2:4] = word(len(loader) - 4) — line length */
    gs_word(L - 4, w);
    loader[2] = w[0];
    loader[3] = w[1];

    int BasicLength = L;
    int BasicEnd = BasicStart + BasicLength;

    /* Clear everything from the channel variables to the UDG start */
    if (gs_patch_fill(gs, BasicStart, 0x00, 65368 - BasicStart) != 0)
        return -1;

    /* Patch ERR_SP */
    gs_word(clear_addr - 3, w);
    if (gs_patch(gs, 23613, w, 2) != 0) return -1;
    /* Patch VARS */
    gs_word(BasicEnd, w);
    if (gs_patch(gs, 23627, w, 2) != 0) return -1;
    /* Patch E_LINE */
    gs_word(BasicEnd + 1, w);
    if (gs_patch(gs, 23641, w, 2) != 0) return -1;
    /* Patch WORKSP */
    gs_word(BasicEnd + 4, w);
    if (gs_patch(gs, 23649, w, 2) != 0) return -1;
    /* Patch STKBOT */
    gs_word(BasicEnd + 4, w);
    if (gs_patch(gs, 23651, w, 2) != 0) return -1;
    /* Patch STKEND */
    gs_word(BasicEnd + 4, w);
    if (gs_patch(gs, 23653, w, 2) != 0) return -1;
    /* Patch RAMTOP */
    gs_word(clear_addr, w);
    if (gs_patch(gs, 23730, w, 2) != 0) return -1;

    /* Patch BASIC program */
    if (gs_patch(gs, BasicStart, loader, BasicLength) != 0) return -1;

    /* Patch variables area, edit line and calculator stack */
    {
        static const unsigned char editline[] = { 0x80, 0xf7, 0x0d, 0x80 };
        if (gs_patch(gs, BasicEnd, editline, 4) != 0) return -1;
    }

    /* Patch stack */
    {
        static const unsigned char stk[] = {
            0x03, 0x13, /* Error resume routine (ERR_SP points here): MAIN_4 */
            0x00, 0x3e  /* GOSUB stack end marker */
        };
        if (gs_patch(gs, clear_addr - 3, stk, 4) != 0) return -1;
    }

    /* UDG area (might be overwritten by compiled code) */
    if (gs_patch(gs, 65368, UDG_TABLE, (int)sizeof(UDG_TABLE)) != 0)
        return -1;

    /* Finally, patch compiled code in */
    if (mc_addr + mc_len > 65536) {
        /* Python: assert mc_addr + len(mc_bytes) <= 65536 */
        return -1;
    }
    if (gs_patch(gs, mc_addr, mc_bytes, mc_len) != 0) return -1;

    return 0;
}

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

    if (gs_init(&gs, clear_addr, mc_addr, mc_bytes, mc_len) != 0) {
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
