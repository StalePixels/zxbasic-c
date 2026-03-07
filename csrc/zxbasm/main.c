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
#include "getopt_port.h"
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
    char *tmp = strdup(input);
    char *base = basename(tmp);

    /* Strip extension */
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';

    size_t len = strlen(base) + strlen(ext) + 2;
    char *out = malloc(len);
    snprintf(out, len, "%s.%s", base, ext);
    free(tmp);
    return out;
}

int main(int argc, char *argv[])
{
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

    if ((int)use_tzx + (int)use_tap > 1) {
        fprintf(stderr, "error: Options --tap and --tzx are mutually exclusive\n");
        return 3;
    }

    if (use_basic && !use_tzx && !use_tap) {
        fprintf(stderr, "error: Option --BASIC and --autorun requires --tzx or --tap format\n");
        return 4;
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

    if (pp.error_count > 0) {
        preproc_destroy(&pp);
        if (as.err_file && as.err_file != stderr)
            fclose(as.err_file);
        asm_destroy(&as);
        free(default_out);
        return 1;
    }

    const char *preprocessed = strbuf_cstr(&pp.output);

    /* Step 2: Parse and assemble */
    asm_assemble(&as, preprocessed);

    preproc_destroy(&pp);

    if (as.error_count > 0) {
        if (as.err_file && as.err_file != stderr)
            fclose(as.err_file);
        asm_destroy(&as);
        free(default_out);
        return 1;
    }

    /* Step 3: Handle #init entries and generate binary */
    /* TODO: #init support (CALL NN for each init label, JP NN at end) */

    /* Step 4: Memory map */
    if (memory_map_file) {
        /* TODO: generate memory map */
    }

    /* Step 5: Generate binary output */
    int result = asm_generate_binary(&as, output_file, output_format);

    /* Cleanup */
    if (as.err_file && as.err_file != stderr)
        fclose(as.err_file);

    int exit_code = (result != 0 || as.error_count > 0) ? 1 : 0;
    asm_destroy(&as);
    free(default_out);
    return exit_code;
}
