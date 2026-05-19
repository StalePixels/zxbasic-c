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
 * The loader/autorun state is threaded into the assembler before
 * generate_binary, mirroring zxbc.py:215-228 where assemble()/
 * generate_binary() run with OPTIONS.use_basic_loader/OPTIONS.autorun
 * in effect (set from --BASIC/--autorun by args_config.py:104-105).
 * Same field names and lifecycle point as the proven-faithful
 * zxbasm/main.c:174-175 (as.use_basic_loader/as.autorun set right
 * after asm_init, before assembling).
 *
 * asm_assemble returns the assembler's error count (nonzero = errors,
 * mirrors gl.has_errors); asm_generate_binary returns 0 on success or
 * negative on error. Either failure -> return 1 so codegen.c yields
 * rc 5.
 */
#include "asm_bridge.h"
#include "zxbasm.h"

int zxbc_asm_to_binary(const char *asm_text, const char *out_filename,
                       const char *format, bool use_basic_loader,
                       bool autorun,
                       char **binary_files, int binary_files_count,
                       char **headless_binary_files,
                       int headless_binary_files_count) {
    AsmState as;
    asm_init(&as);
    as.use_basic_loader = use_basic_loader;
    as.autorun = autorun;

    int aerr = asm_assemble(&as, asm_text);
    int gerr = 0;
    if (aerr == 0)
        gerr = asm_generate_binary(&as, out_filename, format,
                                   binary_files, binary_files_count,
                                   headless_binary_files,
                                   headless_binary_files_count);

    asm_destroy(&as);
    return (aerr != 0 || gerr != 0) ? 1 : 0;
}
