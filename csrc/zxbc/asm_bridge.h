/*
 * asm_bridge — minimal plain-C seam between the zxbc codegen and the
 * zxbasm assembler library.
 *
 * Header isolation: this header exposes ONLY a plain-C declaration so
 * codegen.c never has to include zxbasm.h. Only asm_bridge.c pulls in
 * the assembler types.
 *
 * Faithful to zxbc.py:215-228 (every non-asm output path): assemble the
 * rendered asm text, then generate the output file in the requested
 * container format.
 */
#ifndef ZXBC_ASM_BRIDGE_H
#define ZXBC_ASM_BRIDGE_H

#include <stdbool.h>

/* Assemble `asm_text` and write the output to `out_filename` in the
 * container format `format` (bin/tap/tzx/sna/z80), mirroring Python's
 *   asmparse.generate_binary(OPTIONS.output_filename,
 *                            OPTIONS.output_file_type, ...)
 * (the format is passed through, never hardcoded).
 *
 * `use_basic_loader`/`autorun` thread the zxbc loader/autorun state into
 * the assembler before generate_binary, mirroring zxbc.py:215-228 where
 * generate_binary runs with OPTIONS.use_basic_loader/OPTIONS.autorun in
 * effect (set from --BASIC/--autorun by args_config.py:104-105).
 *
 * `binary_files`/`headless_binary_files` (each a list of filename
 * strings + count) thread the zxbc --append-binary /
 * --append-headless-binary lists through to asm_generate_binary,
 * mirroring zxbc.py:220-226 forwarding options.append_binary /
 * options.append_headless_binary to asmparse.generate_binary
 * (asmparse.py:1013-1044). NULL/0 means no appended blocks. Only the
 * TAP/TZX path consumes them (Python only appends for tap/tzx).
 *
 * Returns 0 on success, nonzero if the assembler or binary dump reported
 * any error (caller maps nonzero -> zxbc rc 5, matching Python's
 * `if gl.has_errors: return 5`).
 *
 * `arch` (e.g. "zx48k") and `include_path` (the colon-separated -I list,
 * or NULL/"") let the assembler resolve INCBIN files against the same
 * search path the preprocessor uses — the executable-anchored
 * src/lib/arch/<arch>/{stdlib,runtime} dirs plus the -I dirs — mirroring
 * Python's INCBIN -> zxbpp.search_filename (src/zxbasm/asmparse.py:393,
 * zxbpp.INCLUDEPATH). */
int zxbc_asm_to_binary(const char *asm_text, const char *out_filename,
                       const char *format, bool use_basic_loader,
                       bool autorun, bool zxnext,
                       char **binary_files, int binary_files_count,
                       char **headless_binary_files,
                       int headless_binary_files_count,
                       const char *arch, const char *include_path);

#endif /* ZXBC_ASM_BRIDGE_H */
