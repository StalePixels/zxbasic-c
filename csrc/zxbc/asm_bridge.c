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
#include "utils.h"      /* get_executable_dir */
#include <limits.h>     /* PATH_MAX */
#include <stdlib.h>     /* realpath */
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Build the INCBIN search path on `as`, identical in source and order to
 * the preprocessor's (codegen.c:596-626 / main.c first pass): the
 * executable-anchored ABSOLUTE src/lib/arch/<arch>/{stdlib,runtime}
 * (stdlib first, runtime second), then the colon-split -I dirs.  Stored
 * in the assembler arena so they outlive this call.  This is the C
 * analogue of zxbpp.INCLUDEPATH, which Python's INCBIN consults via
 * zxbpp.search_filename (src/zxbasm/asmparse.py:393). */
static void asm_set_include_paths(AsmState *as, const char *arch,
                                  const char *include_path) {
    /* Count: up to 2 built-in dirs + however many -I segments. */
    int cap = 2;
    if (include_path && include_path[0]) {
        cap++;
        for (const char *q = include_path; *q; q++)
            if (*q == ':') cap++;
    }
    char **paths = arena_alloc(&as->arena, (size_t)cap * sizeof(char *));
    int n = 0;

    const char *a = (arch && arch[0]) ? arch : "zx48k";
    /* raw_path oversized: joining two PATH_MAX-class strings via
     * "%s/arch/%s/..." trips gcc's -Wformat-truncation. */
    char lib_root[PATH_MAX], raw_path[PATH_MAX * 2 + 64], real_path[PATH_MAX];
    if (get_lib_include_root(NULL, lib_root, sizeof(lib_root))) {
        snprintf(raw_path, sizeof(raw_path),
                 "%s/arch/%s/stdlib", lib_root, a);
        if (realpath(raw_path, real_path))
            paths[n++] = arena_strdup(&as->arena, real_path);
        snprintf(raw_path, sizeof(raw_path),
                 "%s/arch/%s/runtime", lib_root, a);
        if (realpath(raw_path, real_path))
            paths[n++] = arena_strdup(&as->arena, real_path);
    }

    if (include_path && include_path[0]) {
        char ipbuf[PATH_MAX];
        if (strlen(include_path) < sizeof(ipbuf)) {
            strcpy(ipbuf, include_path);
            char *save = NULL;
            for (char *seg = strtok_r(ipbuf, ":", &save);
                 seg != NULL && n < cap;
                 seg = strtok_r(NULL, ":", &save)) {
                if (seg[0])
                    paths[n++] = arena_strdup(&as->arena, seg);
            }
        }
    }

    as->include_paths = paths;
    as->include_paths_count = n;
}

int zxbc_asm_to_binary(const char *asm_text, const char *out_filename,
                       const char *format, bool use_basic_loader,
                       bool autorun, bool zxnext,
                       char **binary_files, int binary_files_count,
                       char **headless_binary_files,
                       int headless_binary_files_count,
                       const char *arch, const char *include_path) {
    AsmState as;
    asm_init(&as);
    as.use_basic_loader = use_basic_loader;
    as.autorun = autorun;
    /* Thread OPTIONS.zxnext into the inline-ASM assembler so an `ASM ... END
     * ASM` block under `#pragma zxnext = TRUE` (or --zxnext) recognises the
     * Z80N opcode set, mirroring Python: asmlex enables the Z80N tokens
     * (asmlex.py:323) and asmparse selects zxnext_parser (asmparse.py:1001)
     * iff OPTIONS.zxnext. Without this the inline-asm lexer reads e.g. MUL as
     * the '*' operator (use_zxnext_asm). */
    as.zxnext = zxnext;
    asm_set_include_paths(&as, arch, include_path);

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
