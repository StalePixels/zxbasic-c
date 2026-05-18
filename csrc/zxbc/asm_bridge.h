/*
 * asm_bridge — minimal plain-C seam between the zxbc codegen and the
 * zxbasm assembler library.
 *
 * Header isolation: this header exposes ONLY a plain-C declaration so
 * codegen.c never has to include zxbasm.h. Only asm_bridge.c pulls in
 * the assembler types.
 *
 * Faithful to zxbc.py:215-228 (the `.bin` output path): assemble the
 * rendered asm text, then generate the binary file.
 */
#ifndef ZXBC_ASM_BRIDGE_H
#define ZXBC_ASM_BRIDGE_H

/* Assemble `asm_text` and write the binary to `out_filename`.
 * Returns 0 on success, nonzero if the assembler or binary dump
 * reported any error (caller maps nonzero -> zxbc rc 5, matching
 * Python's `if gl.has_errors: return 5`). */
int zxbc_asm_to_bin(const char *asm_text, const char *out_filename);

#endif /* ZXBC_ASM_BRIDGE_H */
