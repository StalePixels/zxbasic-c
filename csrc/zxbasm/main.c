/*
 * zxbasm — ZX BASIC Assembler (C port)
 *
 * CLI entry point. Processes a Z80 assembly source file:
 *   1. Preprocess via zxbpp (ASM mode)
 *   2. Parse and assemble
 *   3. Generate binary output
 *
 * Usage: zxbasm [options] input_file
 * Mirrors src/zxbasm/zxbasm.py
 */
#include "zxbasm.h"
#include "zxbpp.h"

#include "compat.h"
#include "cwalk.h"
#include "ya_getopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] PROGRAM\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d, --debug          Increase debug level\n");
    fprintf(stderr, "  -O, --optimize N     Optimization level (default: 0)\n");
    fprintf(stderr, "  -o, --output FILE    Output file (default: input.bin)\n");
    fprintf(stderr, "  -T, --tzx            Output TZX format\n");
    fprintf(stderr, "  -t, --tap            Output TAP format\n");
    fprintf(stderr, "  -B, --BASIC          Create BASIC loader\n");
    fprintf(stderr, "  -a, --autorun        Auto-run on load (implies -B)\n");
    fprintf(stderr, "  -e, --errmsg FILE    Error output file\n");
    fprintf(stderr, "  -M, --mmap FILE      Generate label memory map\n");
    fprintf(stderr, "  -b, --bracket        Brackets for indirection only\n");
    fprintf(stderr, "  -N, --zxnext         Enable ZX Next opcodes\n");
    fprintf(stderr, "  --version            Show version\n");
    fprintf(stderr, "  -h, --help           Show this help\n");
}

/* Generate default output filename: basename without extension + ".bin" */
static char *default_output(const char *input, const char *ext)
{
    const char *base_ptr;
    size_t base_len;
    cwk_path_get_basename(input, &base_ptr, &base_len);
    if (!base_ptr || base_len == 0) {
        base_ptr = input;
        base_len = strlen(input);
    }

    /* Copy basename so we can strip extension */
    char *base = malloc(base_len + 1);
    memcpy(base, base_ptr, base_len);
    base[base_len] = '\0';

    /* Strip extension */
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    size_t len = strlen(base) + strlen(ext) + 2;
    char *out = malloc(len);
    snprintf(out, len, "%s.%s", base, ext);
    free(base);
    return out;
}

int main(int argc, char *argv[])
{
    cwk_path_set_style(CWK_STYLE_UNIX);

    const char *output_file = NULL;
    const char *error_file = NULL;
    const char *input_file = NULL;
    const char *memory_map_file = NULL;
    int debug_level = 0;
    bool use_tzx = false;
    bool use_tap = false;
    bool use_basic = false;
    bool use_autorun = false;
    bool use_brackets = false;
    bool use_zxnext = false;

    static struct option long_options[] = {
        {"debug",    no_argument,       NULL, 'd'},
        {"optimize", required_argument, NULL, 'O'},
        {"output",   required_argument, NULL, 'o'},
        {"tzx",      no_argument,       NULL, 'T'},
        {"tap",      no_argument,       NULL, 't'},
        {"BASIC",    no_argument,       NULL, 'B'},
        {"autorun",  no_argument,       NULL, 'a'},
        {"errmsg",   required_argument, NULL, 'e'},
        {"mmap",     required_argument, NULL, 'M'},
        {"bracket",  no_argument,       NULL, 'b'},
        {"zxnext",   no_argument,       NULL, 'N'},
        {"version",  no_argument,       NULL, 'V'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "dO:o:TtBae:M:bNh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd': debug_level++; break;
        case 'O': /* optimization level — ignored for assembler */ break;
        case 'o': output_file = optarg; break;
        case 'T': use_tzx = true; break;
        case 't': use_tap = true; break;
        case 'B': use_basic = true; break;
        case 'a': use_autorun = true; use_basic = true; break;
        case 'e': error_file = optarg; break;
        case 'M': memory_map_file = optarg; break;
        case 'b': use_brackets = true; break;
        case 'N': use_zxnext = true; break;
        case 'V':
            printf("zxbasm %s (C port)\n", ZXBASIC_C_VERSION);
            return 0;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "error: the following arguments are required: PROGRAM\n");
        usage(argv[0]);
        return 2;
    }

    input_file = argv[optind];

    /* Validate input file exists */
    FILE *check = fopen(input_file, "r");
    if (!check) {
        fprintf(stderr, "error: No such file or directory: '%s'\n", input_file);
        return 2;
    }
    fclose(check);

    /* Determine output format */
    const char *output_format = "bin";
    if (use_tzx) output_format = "tzx";
    else if (use_tap) output_format = "tap";

    /* S7.2d-iii — faithful port of src/zxbasm/zxbasm.py:143-149.
     * Python uses o_parser.error(...) which (argparse) prints
     * "usage: …\n<prog>: error: <msg>" to stderr and sys.exit(2);
     * the `return 3` / `return 4` after each o_parser.error are DEAD
     * code (never reached). So BOTH exit 2 (not 3/4), with Python's
     * exact message text (note the second is asymmetric: "--tzx or
     * tap format" — bare `tap`, not `--tap`). The argparse usage:
     * preamble is the carried user-adjudication, not reproduced
     * (same standing decision as the S7.2d-i/-ii gates). */
    if ((int)use_tzx + (int)use_tap > 1) {
        fprintf(stderr, "error: Options --tap, --tzx and --asm are mutually exclusive\n");
        return 2;
    }

    if (use_basic && !use_tzx && !use_tap) {
        fprintf(stderr, "error: Option --BASIC and --autorun requires --tzx or tap format\n");
        return 2;
    }

    /* Default output filename */
    char *default_out = NULL;
    if (!output_file) {
        default_out = default_output(input_file, output_format);
        output_file = default_out;
    }

    /* Set up assembler state */
    AsmState as;
    asm_init(&as);
    as.debug_level = debug_level;
    as.zxnext = use_zxnext;
    as.force_brackets = use_brackets;
    as.input_filename = arena_strdup(&as.arena, input_file);
    as.output_filename = arena_strdup(&as.arena, output_file);
    as.output_format = arena_strdup(&as.arena, output_format);
    as.use_basic_loader = use_basic;
    as.autorun = use_autorun;
    as.current_file = as.input_filename;
    if (memory_map_file) {
        as.memory_map_file = arena_strdup(&as.arena, memory_map_file);
    }

    /* Error output */
    if (error_file) {
        if (strcmp(error_file, "/dev/null") == 0) {
            as.err_file = fopen("/dev/null", "w");
        } else if (strcmp(error_file, "/dev/stderr") == 0) {
            as.err_file = stderr;
        } else {
            as.err_file = fopen(error_file, "w");
            if (!as.err_file) {
                fprintf(stderr, "Cannot open error file: %s\n", error_file);
                free(default_out);
                return 1;
            }
        }
    }

    /* Step 1: Preprocess via zxbpp in ASM mode */
    PreprocState pp;
    preproc_init(&pp);
    pp.debug_level = debug_level;
    pp.in_asm = true;  /* ASM mode: zxbpp.setMode("asm") in Python */

    /* Redirect preprocessor errors to same error file */
    if (as.err_file != stderr) {
        pp.err_file = as.err_file;
    }

    preproc_file(&pp, input_file);

    /* Python zxbasm.main() (src/zxbasm/zxbasm.py:155-161) runs the
     * preprocessor, then assembles OUTPUT *unconditionally*, and only
     * then checks has_errors. It never short-circuits between the two —
     * so a preprocessor error still produces an assembler diagnostic on
     * the (often empty) output (see newl.err / preprocerr2.err, where
     * the assembler EOF message follows the preprocessor's). */
    int preproc_errors = pp.error_count;

    const char *preprocessed = strbuf_cstr(&pp.output);

    /* Step 2: Parse and assemble */
    asm_assemble(&as, preprocessed);

    preproc_destroy(&pp);

    if (preproc_errors > 0 || as.error_count > 0) {
        if (as.err_file && as.err_file != stderr)
            fclose(as.err_file);
        asm_destroy(&as);
        free(default_out);
        return 1;
    }

    /* Step 3: Handle #init entries and generate binary */
    /* TODO: #init support (CALL NN for each init label, JP NN at end) */

    /* Step 4: Memory map (mirrors src/zxbasm/zxbasm.py:183-185)
     *
     *   if OPTIONS.memory_map:
     *       with open(OPTIONS.memory_map, "wt") as f:
     *           f.write(asmparse.MEMORY.memory_map)
     *
     * Python skips this when MEMORY.memory_bytes is empty: zxbasm.py:163
     * (`if not asmparse.MEMORY.memory_bytes: ... return 0`) returns
     * before the write.
     *
     * The faithful C analogue of "memory_bytes is non-empty" is "was
     * Memory.set_memory_slot() ever called". Python populates
     * memory_bytes[org]=0 from set_memory_slot(), reached BOTH from
     * __set_byte()/add_instruction (memory.py:157) AND from EVERY
     * non-temporary declare_label() including EQU constants
     * (memory.py:249). So an EQU-only unit has memory_bytes={0:0}
     * (verified) and Python DOES write an (empty) mmap; only a unit
     * with zero bytes AND zero declared labels has empty memory_bytes
     * (and that path fails earlier with a parse error anyway).
     *
     * byte_set[] alone is too strict here: mem_declare_label()
     * deliberately does not set byte_set[] (memory.c:240-244), so an
     * EQU-only unit would be wrongly suppressed. Mirror Python by
     * also treating "a label was declared in the global scope"
     * (label_scopes[0], where top-level + merged PROC labels land,
     * the same map mem_dump() sweeps) as non-empty. */
    if (memory_map_file) {
        bool any_byte = as.mem.label_scopes[0].count > 0;
        for (int i = 0; !any_byte && i < MAX_MEM; i++) {
            if (as.mem.byte_set[i]) any_byte = true;
        }
        if (any_byte) {
            FILE *mf = fopen(memory_map_file, "w");
            if (!mf) {
                fprintf(stderr, "Cannot open memory map file: %s\n",
                        memory_map_file);
                if (as.err_file && as.err_file != stderr)
                    fclose(as.err_file);
                asm_destroy(&as);
                free(default_out);
                return 1;
            }
            char *mmap = mem_memory_map(&as.mem, &as.arena);
            fputs(mmap, mf);
            fclose(mf);
        }
    }

    /* Step 5: Generate binary output.
     * zxbasm's own --append-binary CLI is NOT wired in this slice
     * (S6.7a-owned) — pass empty/zero aux to preserve zxbasm's exact
     * current behaviour (no appended blocks). */
    int result = asm_generate_binary(&as, output_file, output_format,
                                     NULL, 0, NULL, 0);

    /* Cleanup */
    if (as.err_file && as.err_file != stderr)
        fclose(as.err_file);

    int exit_code = (result != 0 || as.error_count > 0) ? 1 : 0;
    asm_destroy(&as);
    free(default_out);
    return exit_code;
}
