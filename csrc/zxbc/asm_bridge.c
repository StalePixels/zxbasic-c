/*
 * asm_bridge — see asm_bridge.h.
 *
 * This is the ONLY zxbc translation unit that includes zxbasm.h, so the
 * assembler's types stay out of codegen.c. Faithful to zxbc.py:215-228:
 *
 *     fout = StringIO(); output(asm_output, fout)
 *     asmparse.assemble(fout.getvalue())
 *     asmparse.generate_binary(OPTIONS.output_filename,
 *                              OPTIONS.output_file_type, ...)
 *     if gl.has_errors: return 5
 *
 * The output_file_type (bin/tap/tzx/sna/z80) is passed through to
 * asm_generate_binary verbatim — never hardcoded — exactly as Python
 * forwards OPTIONS.output_file_type to generate_binary.
 *
 * asm_assemble returns the assembler's error count (nonzero = errors,
 * mirrors gl.has_errors); asm_generate_binary returns 0 on success or
 * negative on error. Either failure -> return 1 so codegen.c yields
 * rc 5.
 */
#include "asm_bridge.h"
#include "zxbasm.h"

int zxbc_asm_to_binary(const char *asm_text, const char *out_filename,
                       const char *format) {
    AsmState as;
    asm_init(&as);

    int aerr = asm_assemble(&as, asm_text);
    int gerr = 0;
    if (aerr == 0)
        gerr = asm_generate_binary(&as, out_filename, format);

    asm_destroy(&as);
    return (aerr != 0 || gerr != 0) ? 1 : 0;
}
