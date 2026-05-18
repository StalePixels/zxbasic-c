/*
 * outfmt_tzx.h — Faithful .tzx (TZX tape) emitter (C port)
 *
 * Byte-for-byte port of src/outfmt/tzx.py (the TZX base class). In the
 * Python, `class TAP(TZX)`: TZX is the BASE and TAP overrides EXACTLY
 * two things. So the entire shared machinery (LH, save_header,
 * standard_bytes_header, standard_program_header, save_code,
 * save_program, emit, and the basic.Basic() loader build) is the same
 * code already ported in outfmt_tap.c — TZX just selects the
 * preamble + the 0x10/pause standard_block form via the `is_tzx` mode
 * threaded through outfmt_tape_emit (see outfmt_tap.h).
 *
 * These entry points parallel outfmt_tap_write / outfmt_tap_write_loader
 * exactly, but produce TZX bytes (10-byte "ZXTape!" 1A 01 15 preamble
 * and standard blocks prefixed with 0x10 + LH(1000)).
 */
#ifndef OUTFMT_TZX_H
#define OUTFMT_TZX_H

/*
 * Write a loader-less TZX file faithful to Python's TZX.emit()
 * (tzx.py:123-143) with loader_bytes is None and no aux blocks:
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
 * Returns 0 on success, -1 on file-open / allocation failure.
 */
int outfmt_tzx_write(const char *filename,
                     const char *program_name,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len);

/*
 * Loader-aware TZX writer — faithful to Python's TZX.emit()
 * (tzx.py:123-143):
 *
 *   if loader_bytes is not None:
 *       save_program("loader", loader_bytes, line=1)
 *   save_code(program_name, entry_point, program_bytes)
 *   dump(output_filename)
 *
 *   loader_bytes  the tokenised BASIC loader program bytes, OR NULL
 *                 (and/or loader_len < 0) to mean "no loader" — in
 *                 which case this is byte-identical to outfmt_tzx_write
 *                 (no PROGRAM block emitted at all).
 *   loader_len    number of loader bytes.
 *
 * Other parameters are exactly as outfmt_tzx_write. The PROGRAM header
 * autostart parameter is fixed at line=1 (Python's save_program(...,
 * line=1) — "Put line 0 to protect against MERGE").
 *
 * Returns 0 on success, -1 on file-open / allocation failure.
 */
int outfmt_tzx_write_loader(const char *filename,
                            const char *program_name,
                            int entry_point,
                            const unsigned char *loader_bytes,
                            int loader_len,
                            const unsigned char *program_bytes,
                            int program_len);

#endif /* OUTFMT_TZX_H */
