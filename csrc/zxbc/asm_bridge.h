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
 * Returns 0 on success, nonzero if the assembler or binary dump reported
 * any error (caller maps nonzero -> zxbc rc 5, matching Python's
 * `if gl.has_errors: return 5`). */
int zxbc_asm_to_binary(const char *asm_text, const char *out_filename,
                       const char *format, bool use_basic_loader,
                       bool autorun);

#endif /* ZXBC_ASM_BRIDGE_H */
