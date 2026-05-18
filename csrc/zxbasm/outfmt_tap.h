/*
 * outfmt_tap.h — Faithful .tap (TAP tape) emitter (C port)
 *
 * Byte-for-byte port of src/outfmt/tap.py (the TAP override of TZX) +
 * the relevant TZX primitives in src/outfmt/tzx.py, loader-less path.
 *
 * S6.3a: loader-less core only.
 * S6.3b: adds the PROGRAM-block loader path (tzx.py:105-122,123-143)
 *        — save_program("loader", loader_bytes, line=1) emitted before
 *        save_code() when a loader is present. Aux blocks still out of
 *        scope (separate bridge plumbing).
 */
#ifndef OUTFMT_TAP_H
#define OUTFMT_TAP_H

/*
 * Write a loader-less TAP file faithful to Python's TAP.emit():
 *   save_code(program_name, entry_point, program_bytes)  then dump.
 *
 *   filename      output file path (opened "wb")
 *   program_name  program title (basename[:10] of output); space-padded
 *                 to 10 chars by the header builder
 *   entry_point   CODE start address (Python's AUTORUN_ADDR, == org if
 *                 no autorun was set)
 *   program_bytes the assembled code bytes
 *   program_len   number of code bytes (may be 0 — still emit faithful
 *                 header + empty-data blocks, matching Python)
 *
 * Returns 0 on success, -1 on file-open failure.
 */
int outfmt_tap_write(const char *filename,
                     const char *program_name,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len);

/*
 * Loader-aware TAP writer — faithful to Python's TAP.emit()
 * (tzx.py:123-143):
 *
 *   if loader_bytes is not None:
 *       save_program("loader", loader_bytes, line=1)
 *   save_code(program_name, entry_point, program_bytes)
 *   dump(output_filename)
 *
 *   loader_bytes  the tokenised BASIC loader program bytes, OR NULL
 *                 (and/or loader_len < 0) to mean "no loader" — in
 *                 which case this is byte-identical to S6.3a's
 *                 outfmt_tap_write (no PROGRAM block emitted at all).
 *   loader_len    number of loader bytes.
 *
 * Other parameters are exactly as outfmt_tap_write. The PROGRAM header
 * autostart parameter is fixed at line=1 (Python's save_program(...,
 * line=1) — "Put line 0 to protect against MERGE"), independent of the
 * BASIC line numbers inside loader_bytes.
 *
 * Returns 0 on success, -1 on file-open / allocation failure.
 */
int outfmt_tap_write_loader(const char *filename,
                            const char *program_name,
                            int entry_point,
                            const unsigned char *loader_bytes,
                            int loader_len,
                            const unsigned char *program_bytes,
                            int program_len);

/*
 * Shared TAP/TZX emit core — faithful (TZX|TAP).emit() (tzx.py:123-143,
 * no aux blocks). The Python design is `class TAP(TZX)`: TZX is the base
 * and TAP overrides EXACTLY two things (empty output; standard_block
 * without the 0x10 block-id / 1000ms pause). This single core carries
 * the whole shared machinery; `is_tzx` selects that two-point variance:
 *
 *   is_tzx == 0  -> TAP: empty output, standard_block = LH+payload+cksum.
 *                   Byte-identical to the pre-S6.4 outfmt_tap_* path.
 *   is_tzx == 1  -> TZX: 10-byte "ZXTape!" 1A 01 15 preamble, and
 *                   standard_block prefixed with 0x10 + LH(1000).
 *
 * Used by outfmt_tap_write_loader (is_tzx=0) and the outfmt_tzx_* entry
 * points in outfmt_tzx.c (is_tzx=1). Other parameters are exactly as
 * outfmt_tap_write_loader. Returns 0 on success, -1 on failure.
 */
int outfmt_tape_emit(int is_tzx,
                     const char *filename,
                     const char *program_name,
                     int entry_point,
                     const unsigned char *loader_bytes,
                     int loader_len,
                     const unsigned char *program_bytes,
                     int program_len);

#endif /* OUTFMT_TAP_H */
