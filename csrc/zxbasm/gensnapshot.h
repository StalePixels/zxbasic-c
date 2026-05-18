/*
 * gensnapshot.h — Shared 48K ZX Spectrum memory-image builder (C port)
 *
 * Verbatim port of:
 *   src/outfmt/gensnapshot.py  GenSnapshot.__init__  (memory image)
 *
 * The builder reproduces gensnapshot.py:99-256 exactly: a full 48K RAM
 * image (0x4000..0xFFFF) plus the Z80 register / state fields a snapshot
 * format needs to emit. Factored out of outfmt_sna.c so the .sna and
 * .z80 emitters can share one byte-faithful image builder.
 *
 * The patch ORDER inside gensnapshot_init() is byte-load-bearing: later
 * patchAddr() calls overwrite earlier ones. Do not reorder or "tidy".
 */
#ifndef GENSNAPSHOT_H
#define GENSNAPSHOT_H

/* ----------------------------------------------------------------
 * Snapshot state — mirrors the GenSnapshot object fields that a
 * snapshot emitter reads.
 * ---------------------------------------------------------------- */
typedef struct GenSnapshot {
    unsigned char mem[49152]; /* the 48K RAM image (0x4000..0xFFFF) */

    /* Z80 registers / state (only the ones a snapshot reads matter for
     * the output; W/Z/cycles/halted/eilast are intentionally omitted as
     * they do not affect the snapshot bytes). */
    int A, A2, B, B2, C, C2, D, D2, E, E2, H, H2, L, L2, F, F2;
    int R, IXL, IXH, IYH, IYL, I;
    int IFF1, IFF2;
    int PCH, PCL;
    int SPH, SPL;
    int IM;
    int outFE;
} GenSnapshot;

/*
 * GenSnapshot.__init__(loader_bytes=None, clear_addr, mc_addr, mc_bytes)
 *
 * Builds the full 48K memory image and register/state fields into *gs.
 *
 * Returns 0 on success, -1 if a patch went out of range or the
 * `assert mc_addr + len(mc_bytes) <= 65536` was violated.
 */
int gensnapshot_init(GenSnapshot *gs, int clear_addr, int mc_addr,
                     const unsigned char *mc_bytes, int mc_len);

#endif /* GENSNAPSHOT_H */
