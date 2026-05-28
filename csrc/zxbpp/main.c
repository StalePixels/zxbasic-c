/*
 * zxbpp — ZX BASIC Preprocessor (C port)
 *
 * CLI entry point. Processes a BASIC or ASM source file through
 * the preprocessor, expanding macros, includes, and conditionals.
 *
 * Usage: zxbpp [options] input_file
 */
#include "zxbpp.h"

#include "cwalk.h"
#include "utils.h"
#include "ya_getopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] [input_file]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o, --output FILE    Output file (default: stdout)\n");
    fprintf(stderr, "  -d, --debug          Increase debug level\n");
    fprintf(stderr, "  -e, --errmsg FILE    Error messages file (default: stderr)\n");
    fprintf(stderr, "  --arch ARCH          Target architecture (default: zx48k)\n");
    fprintf(stderr, "  --expect-warnings N  Suppress first N warnings\n");
    fprintf(stderr, "  -h, --help           Show this help\n");
}

int main(int argc, char *argv[])
{
    cwk_path_set_style(CWK_STYLE_UNIX);

    const char *output_file = NULL;
    const char *error_file = NULL;
    const char *input_file = NULL;
    const char *arch = "zx48k";
    int debug_level = 0;
    int expect_warnings = 0;

    static struct option long_options[] = {
        {"output",          required_argument, NULL, 'o'},
        {"debug",           no_argument,       NULL, 'd'},
        {"errmsg",          required_argument, NULL, 'e'},
        {"arch",            required_argument, NULL, 'A'},
        {"expect-warnings", required_argument, NULL, 'W'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:de:h", long_options, NULL)) != -1) {
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
            /* Unrecognized option. Python zxbpp uses argparse, which
             * rejects unknown options with exit code 2 (parser.error).
             * getopt cannot reproduce argparse's exact "usage:" preamble
             * or its tokenized "unrecognized arguments:" list (carried
             * user-adjudication); the faithful, byte-required behavior is
             * an error:-prefixed stderr line + exit 2 (matching the C
             * parser.error convention used for --arch validation below
             * and in zxbasm/main.c). getopt has already printed its own
             * diagnostic to stderr for the offending token. */
            fprintf(stderr, "error: unrecognized arguments\n");
            return 2;
        }
    }

    if (optind < argc) {
        input_file = argv[optind];
    }

    /* --arch validation — faithful port of src/zxbpp/zxbpp.py:1033-1034:
     *   if options.arch not in arch.AVAILABLE_ARCHITECTURES:
     *       parser.error(f"Invalid architecture '{options.arch}'")
     * AVAILABLE_ARCHITECTURES = ("zx48k","zxnext") (arch/__init__.py:12-18).
     * argparse parser.error -> stderr + exit 2. The C parser.error-
     * analogue convention (zxbasm/main.c:138, the S7.2d-i zxbc gates)
     * is fprintf(stderr,"error: <msg>") + exit 2; the argparse usage:
     * preamble is the carried user-adjudication, not reproduced. */
    if (strcmp(arch, "zx48k") != 0 && strcmp(arch, "zxnext") != 0) {
        fprintf(stderr, "error: Invalid architecture '%s'\n", arch);
        return 2;
    }

    /* Set up preprocessor state */
    PreprocState pp;
    preproc_init(&pp);
    pp.debug_level = debug_level;
    pp.arch = arena_strdup(&pp.arena, arch);
    pp.expect_warnings = expect_warnings;

    /* Built-in include path — faithful port of Python zxbpp
     * set_include_path / get_include_path (src/zxbpp/zxbpp.py:142-165):
     *   get_include_path(arch) = realpath(dirname(__file__)/os.pardir/
     *                                     "lib"/"arch"/arch)
     *   set_include_path(): INCLUDE_MAP[arch] = [pwd/stdlib, pwd/runtime];
     *                       INCLUDEPATH = INCLUDE_MAP[OPTIONS.architecture]
     * Python zxbpp has NO -I/-D CLI option (S7.2e-i removed the C-only
     * extras); its include path is derived ENTIRELY from --arch + the
     * built-in pair, anchored to the interpreter file's own directory
     * (CWD-INDEPENDENT). The C analogue anchors to the running
     * executable's dir via get_executable_dir (the os.path.dirname
     * (__file__) analogue) + realpath (os.path.realpath). This is the
     * SAME faithful shape already proven in csrc/zxbc/main.c:249-265
     * (S7.1e); pushed unconditionally (set_include_path adds the pair
     * for the arch with no existence check — only per-file lookup skips
     * missing dirs, zxbpp.py:158-205). */
    {
        char exe_dir[PATH_MAX];
        /* Oversized: "%s/../../../src/lib/arch/%s/..." joins two PATH_MAX-class
         * strings, so gcc's -Wformat-truncation fires on PATH_MAX dest. */
        char raw_path[PATH_MAX * 2 + 64];
        char real_path[PATH_MAX];

        if (get_executable_dir(argv[0], exe_dir, sizeof(exe_dir))) {
            snprintf(raw_path, sizeof(raw_path),
                     "%s/../../../src/lib/arch/%s/stdlib", exe_dir, arch);
            if (realpath(raw_path, real_path))
                vec_push(pp.include_paths, arena_strdup(&pp.arena, real_path));
            snprintf(raw_path, sizeof(raw_path),
                     "%s/../../../src/lib/arch/%s/runtime", exe_dir, arch);
            if (realpath(raw_path, real_path))
                vec_push(pp.include_paths, arena_strdup(&pp.arena, real_path));
        }
    }

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

    /* NOTE: Python zxbpp exposes no -D/-I CLI option (S7.2e-i removed
     * the C-only extras); macros are not defined from argv, and the
     * include path is the --arch-derived built-in pair set up above
     * (the set_include_path port) — exactly Python's model. */

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
