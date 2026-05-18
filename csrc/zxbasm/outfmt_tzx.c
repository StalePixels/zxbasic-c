/*
 * outfmt_tzx.c — Faithful .tzx (TZX tape) emitter (C port)
 *
 * Byte-for-byte port of src/outfmt/tzx.py (the TZX base class).
 *
 * Python design (tap.py:13 `class TAP(TZX)`): TZX is the BASE class and
 * TAP overrides EXACTLY two things — __init__ (empty output, no preamble)
 * and standard_block (no 0x10 block-id, no 1000ms pause). EVERYTHING ELSE
 * (LH, out, save_header, standard_bytes_header, standard_program_header,
 * save_code, save_program, emit, and the basic.Basic() loader build) is
 * inherited by TAP from TZX — i.e. it is the SAME machinery already
 * ported once in outfmt_tap.c.
 *
 * Rather than duplicate that machinery, this file is a thin shim over
 * the shared outfmt_tape_emit(is_tzx, ...) core (outfmt_tap.c /
 * outfmt_tap.h). Passing is_tzx == 1 selects the two TZX-specific
 * points (the 10-byte "ZXTape!" 1A 01 15 preamble and the
 * 0x10 + LH(1000)-prefixed standard_block); everything else is the
 * shared, identical code path. Passing is_tzx == 0 (the TAP entry
 * points) is provably byte-identical to the pre-S6.4 .tap emitter.
 *
 * No basic.Basic() duplication: the loader is built once by the caller
 * (asm_core.c) using basic.{c,h} and passed in as loader_bytes — the
 * exact same path the TAP branch uses.
 */
#include "outfmt_tzx.h"
#include "outfmt_tap.h" /* outfmt_tape_emit — the shared TAP/TZX core */

#include <stddef.h> /* NULL */

/* ----------------------------------------------------------------
 * Public entry — loader-aware faithful TZX.emit() (tzx.py:123-143):
 *   if loader_bytes is not None:
 *       save_program("loader", loader_bytes, line=1)
 *   save_code(program_name, entry_point, program_bytes)
 *   dump(output_filename)
 * Delegates to the shared core with is_tzx == 1 (TZX preamble +
 * 0x10/pause standard_block) and ZERO aux blocks — byte-identical to
 * the pre-S6.7a emitter (the aux loops never execute). Signature
 * UNCHANGED so asm_core.c is untouched.
 * ---------------------------------------------------------------- */
int outfmt_tzx_write_loader(const char *filename,
                            const char *program_name,
                            int entry_point,
                            const unsigned char *loader_bytes,
                            int loader_len,
                            const unsigned char *program_bytes,
                            int program_len)
{
    return outfmt_tape_emit(/*is_tzx=*/1, filename, program_name, entry_point,
                            loader_bytes, loader_len,
                            program_bytes, program_len,
                            /*aux_bin=*/NULL, /*n_aux_bin=*/0,
                            /*aux_headless=*/NULL, /*n_aux_headless=*/0);
}

/* ----------------------------------------------------------------
 * Public entry — aux-aware faithful TZX.emit() (tzx.py:123-143
 * INCLUDING the aux_bin_blocks / aux_headless_bin_blocks tail). The TZX
 * analogue of outfmt_tap_write_full; the future asm_bridge format-wiring
 * (carried gate) calls this. Delegates to the shared core with
 * is_tzx == 1, threading the 4 aux params straight through.
 * n_aux_bin == 0 && n_aux_headless == 0 is byte-identical to
 * outfmt_tzx_write_loader.
 * ---------------------------------------------------------------- */
int outfmt_tzx_write_full(const char *filename,
                          const char *program_name,
                          int entry_point,
                          const unsigned char *loader_bytes,
                          int loader_len,
                          const unsigned char *program_bytes,
                          int program_len,
                          const OutfmtAuxBin *aux_bin,
                          int n_aux_bin,
                          const OutfmtAuxHeadless *aux_headless,
                          int n_aux_headless)
{
    return outfmt_tape_emit(/*is_tzx=*/1, filename, program_name, entry_point,
                            loader_bytes, loader_len,
                            program_bytes, program_len,
                            aux_bin, n_aux_bin,
                            aux_headless, n_aux_headless);
}

/* ----------------------------------------------------------------
 * Public entry — loader-less faithful TZX.emit() (tzx.py:123-143 with
 * loader_bytes is None and no aux blocks). Delegates to the loader-aware
 * path with no loader.
 * ---------------------------------------------------------------- */
int outfmt_tzx_write(const char *filename,
                     const char *program_name,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len)
{
    return outfmt_tzx_write_loader(filename, program_name, entry_point,
                                   /*loader_bytes=*/NULL, /*loader_len=*/-1,
                                   program_bytes, program_len);
}
