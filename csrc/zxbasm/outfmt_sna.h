/*
 * outfmt_sna.h — Faithful 48K .sna snapshot emitter (C port)
 *
 * Byte-for-byte port of src/outfmt/sna.py (SnaEmitter) +
 * src/outfmt/gensnapshot.py (GenSnapshot). The .sna path IGNORES the
 * BASIC loader, the program name, and the aux blocks entirely
 * (SnaEmitter.emit() calls self.generate(None, entry_point - 1,
 * entry_point, program_bytes)) — it builds a full 48K memory image
 * from scratch, patches the system variables / synthetic BASIC loader
 * line / compiled code, then prepends the 27-byte .sna register header
 * and pokes PC onto the stack.
 *
 * Total output is always exactly 27 + 49152 = 49179 bytes.
 */
#ifndef OUTFMT_SNA_H
#define OUTFMT_SNA_H

/*
 * Write a 48K .sna file faithful to Python's SnaEmitter.emit()
 * (sna.py:91-107) with program_name / loader_bytes / aux blocks
 * ignored:
 *   generate(None, entry_point - 1, entry_point, program_bytes)
 *   open(filename, "wb").write(sna_data)
 *
 *   filename       output file path (opened "wb")
 *   entry_point    CODE start address (Python's AUTORUN_ADDR, == org
 *                  if no autorun was set). clear_addr = entry_point-1,
 *                  mc_addr = entry_point.
 *   program_bytes  the assembled code bytes (mc_bytes)
 *   program_len    number of code bytes
 *
 * Returns 0 on success, -1 on file-open failure OR if the Python
 * `assert mc_addr + len(mc_bytes) <= 65536` is violated.
 */
int outfmt_sna_write(const char *filename,
                     int entry_point,
                     const unsigned char *program_bytes,
                     int program_len);

#endif /* OUTFMT_SNA_H */
