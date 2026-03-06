/*
 * zxbpp — ZX BASIC Preprocessor (C port)
 *
 * CLI entry point. Processes a BASIC or ASM source file through
 * the preprocessor, expanding macros, includes, and conditionals.
 *
 * Usage: zxbpp [options] input_file
 */
#include "zxbpp.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] [input_file]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o, --output FILE    Output file (default: stdout)\n");
    fprintf(stderr, "  -d, --debug          Increase debug level\n");
    fprintf(stderr, "  -e, --errmsg FILE    Error messages file (default: stderr)\n");
    fprintf(stderr, "  -D, --define NAME[=VALUE]  Define a macro\n");
    fprintf(stderr, "  -I, --include-path DIR     Add include search path\n");
    fprintf(stderr, "  --arch ARCH          Target architecture (default: zx48k)\n");
    fprintf(stderr, "  --expect-warnings N  Suppress first N warnings\n");
    fprintf(stderr, "  -h, --help           Show this help\n");
}

int main(int argc, char *argv[])
{
    const char *output_file = NULL;
    const char *error_file = NULL;
    const char *input_file = NULL;
    const char *arch = "zx48k";
    int debug_level = 0;
    int expect_warnings = 0;

    /* Macro definitions from command line */
    char *cmdline_defines[64];
    int num_defines = 0;

    /* Include paths from command line */
    char *cmdline_includes[64];
    int num_includes = 0;

    static struct option long_options[] = {
        {"output",          required_argument, NULL, 'o'},
        {"debug",           no_argument,       NULL, 'd'},
        {"errmsg",          required_argument, NULL, 'e'},
        {"define",          required_argument, NULL, 'D'},
        {"include-path",    required_argument, NULL, 'I'},
        {"arch",            required_argument, NULL, 'A'},
        {"expect-warnings", required_argument, NULL, 'W'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:de:D:I:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'o':
            output_file = optarg;
            break;
        case 'd':
            debug_level++;
            break;
        case 'e':
            error_file = optarg;
            break;
        case 'D':
            if (num_defines < 64)
                cmdline_defines[num_defines++] = optarg;
            break;
        case 'I':
            if (num_includes < 64)
                cmdline_includes[num_includes++] = optarg;
            break;
        case 'A':
            arch = optarg;
            break;
        case 'W':
            expect_warnings = atoi(optarg);
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        input_file = argv[optind];
    }

    /* Set up preprocessor state */
    PreprocState pp;
    preproc_init(&pp);
    pp.debug_level = debug_level;
    pp.arch = arena_strdup(&pp.arena, arch);
    pp.expect_warnings = expect_warnings;

    /* Error output */
    if (error_file) {
        if (strcmp(error_file, "/dev/null") == 0) {
            pp.err_file = fopen("/dev/null", "w");
        } else if (strcmp(error_file, "/dev/stderr") == 0) {
            pp.err_file = stderr;
        } else {
            pp.err_file = fopen(error_file, "w");
            if (!pp.err_file) {
                fprintf(stderr, "Cannot open error file: %s\n", error_file);
                return 1;
            }
        }
    }

    /* Add include paths */
    for (int i = 0; i < num_includes; i++) {
        /* Handle colon-separated paths */
        char *paths = strdup(cmdline_includes[i]);
        char *tok = strtok(paths, ":");
        while (tok) {
            char *p = arena_strdup(&pp.arena, tok);
            vec_push(pp.include_paths, p);
            tok = strtok(NULL, ":");
        }
        free(paths);
    }

    /* Add default include paths based on architecture */
    /* These will be relative to the zxbasic root — for now, we try
     * to find them relative to the input file or current directory */

    /* Process command-line defines */
    for (int i = 0; i < num_defines; i++) {
        char *eq = strchr(cmdline_defines[i], '=');
        if (eq) {
            char *name = arena_strndup(&pp.arena, cmdline_defines[i],
                                       (size_t)(eq - cmdline_defines[i]));
            preproc_define(&pp, name, eq + 1, 0, "<cmdline>");
        } else {
            preproc_define(&pp, cmdline_defines[i], "", 0, "<cmdline>");
        }
    }

    /* Process input */
    if (input_file) {
        preproc_file(&pp, input_file);
    } else {
        /* Read from stdin */
        StrBuf input;
        strbuf_init(&input);
        char buf[4096];
        while (fgets(buf, sizeof(buf), stdin)) {
            strbuf_append(&input, buf);
        }
        preproc_string(&pp, strbuf_cstr(&input), "<stdin>");
        strbuf_free(&input);
    }

    /* Write output only if no errors (matches Python: if not global_.has_errors) */
    if (pp.error_count == 0) {
        if (output_file) {
            FILE *f = fopen(output_file, "w");
            if (!f) {
                fprintf(stderr, "Cannot open output file: %s\n", output_file);
                preproc_destroy(&pp);
                return 1;
            }
            fputs(strbuf_cstr(&pp.output), f);
            fclose(f);
        } else {
            fputs(strbuf_cstr(&pp.output), stdout);
        }
    }

    /* Cleanup */
    if (pp.err_file && pp.err_file != stderr)
        fclose(pp.err_file);

    int exit_code = pp.error_count > 0 ? 1 : 0;
    preproc_destroy(&pp);
    return exit_code;
}
