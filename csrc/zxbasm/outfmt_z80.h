/*
 * outfmt_z80.h — Faithful 48K v1 RLE-compressed .z80 snapshot emitter
 *                (C port)
 *
 * Byte-for-byte port of src/outfmt/z80.py (Z80Emitter) +
 * src/outfmt/gensnapshot.py (GenSnapshot). The .z80 path IGNORES the
 * BASIC loader, the program name, and the aux blocks entirely
 * (Z80Emitter.emit() calls self.generate(None, entry_point - 1,
 * entry_point, program_bytes)) — it builds a full 48K memory image
 * from scratch via the shared gensnapshot builder, prepends the
 * 30-byte v1 .z80 register header, then appends the RLE-compressed
 * 49152-byte memory stream and the unconditional 00 ED ED 00 end
 * marker.
 *
 * Only ever emits the v1 format with header bit5 set: the memory is
 * ALWAYS the RLE form (no "compress only if smaller" / no
 * uncompressed fallback).
 */
#ifndef OUTFMT_Z80_H
#define OUTFMT_Z80_H

/*
 * Write a 48K v1 .z80 file faithful to Python's Z80Emitter.emit()
 * (z80.py:169-185) with program_name / loader_bytes / aux blocks
 * ignored:
 *   generate(None, entry_point - 1, entry_point, program_bytes)
 *   open(filename, "wb").write(output)
 *
 *   filename       output file path (opened "wb")
 *   entry_point    CODE start address (Python's AUTORUN_ADDR, == org
 *                  if no autorun was set). clear_addr = entry_point-1,
 *                  mc_addr = entry_point.
 *   program_bytes  the assembled code bytes (mc_bytes)
 *   program_len    number of code bytes
 *
 * Returns 0 on success, -1 on file-open failure OR if the Python
 * `assert mc_addr + len(mc_bytes) <= 65536` is violated
 * (gensnapshot_init OOB).
 */
int outfmt_z80_write(const char *filename,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len);

#endif /* OUTFMT_Z80_H */
