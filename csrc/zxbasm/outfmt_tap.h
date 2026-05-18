/*
 * outfmt_tap.h — Faithful .tap (TAP tape) emitter (C port)
 *
 * Byte-for-byte port of src/outfmt/tap.py (the TAP override of TZX) +
 * the relevant TZX primitives in src/outfmt/tzx.py, loader-less path.
 *
 * S6.3a: loader-less core only. No basic.Basic() loader, no aux blocks
 * (those are S6.3b — separate sprint with bridge plumbing).
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

#endif /* OUTFMT_TAP_H */
