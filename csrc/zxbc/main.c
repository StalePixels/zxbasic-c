/*
 * main.c — ZX BASIC Compiler entry point
 *
 * Ported from src/zxbc/zxbc.py and src/zxbc/args_parser.py
 */
#include "zxbc.h"
#include "parser.h"
#include "errmsg.h"
#include "ya_getopt.h"
#include "compat.h"
#include "cwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZXBASIC_C_VERSION
#define ZXBASIC_C_VERSION "dev"
#endif

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] PROGRAM.bas\n"
        "\n"
        "ZX BASIC Compiler v%s\n"
        "\n"
        "Options:\n"
        "  -h, --help              Show this help message\n"
        "  -d, --debug             Enable debug output (repeat for more)\n"
        "  -O, --optimize LEVEL    Optimization level (0=none)\n"
        "  -o, --output FILE       Output file\n"
        "  -f, --output-format FMT Output format: asm, bin, tap, tzx, sna, z80, ir\n"
        "  -T, --tzx               Output .tzx format (deprecated, use -f)\n"
        "  -t, --tap               Output .tap format (deprecated, use -f)\n"
        "  -A, --asm               Output .asm format (deprecated, use -f)\n"
        "  -E, --emit-backend      Emit backend IR (deprecated, use -f)\n"
        "  --parse-only            Only parse, check syntax/semantics\n"
        "  -B, --BASIC             Create BASIC loader\n"
        "  -a, --autorun           Auto-run after loading\n"
        "  -S, --org ADDRESS       Start of machine code\n"
        "  -e, --errmsg FILE       Error message output file\n"
        "  --array-base N          Default array lower bound (0 or 1)\n"
        "  --string-base N         Default string lower bound (0 or 1)\n"
        "  -Z, --sinclair          Enable Sinclair BASIC features\n"
        "  -H, --heap-size N       Heap size in bytes\n"
        "  --debug-memory          Out-of-memory debugging\n"
        "  --debug-array           Array boundary checking\n"
        "  --strict-bool           Enforce boolean 0/1\n"
        "  --enable-break          Enable BREAK detection\n"
        "  --explicit              Require declarations\n"
        "  -D, --define MACRO      Define preprocessor macro\n"
        "  -M, --mmap FILE         Generate memory map\n"
        "  -i, --ignore-case       Case-insensitive identifiers\n"
        "  -I, --include-path PATH Include search path\n"
        "  --strict                Force explicit type declarations\n"
        "  --headerless            Omit prologue/epilogue\n"
        "  -N, --zxnext            Enable ZX Next opcodes\n"
        "  --arch ARCH             Target architecture (zx48k, zxnext)\n"
        "  --expect-warnings N     Silence first N warnings\n"
        "  --hide-warning-codes    Hide WXXX codes\n"
        "  -F, --config-file FILE  Load config from file\n"
        "  --save-config FILE      Save config to file\n"
        "  --version               Show version\n",
        prog, ZXBASIC_C_VERSION);
}

/* Long option definitions */
enum {
    OPT_ARRAY_BASE = 256,
    OPT_STRING_BASE,
    OPT_DEBUG_MEMORY,
    OPT_DEBUG_ARRAY,
    OPT_STRICT_BOOL,
    OPT_ENABLE_BREAK,
    OPT_EXPLICIT,
    OPT_STRICT,
    OPT_HEADERLESS,
    OPT_ARCH,
    OPT_EXPECT_WARNINGS,
    OPT_HIDE_WARNING_CODES,
    OPT_SAVE_CONFIG,
    OPT_VERSION,
    OPT_PARSE_ONLY,
    OPT_HEAP_ADDR,
    OPT_APPEND_BIN,
    OPT_APPEND_HEADLESS_BIN,
    OPT_OPT_STRATEGY,
};

static const struct option long_options[] = {
    { "help",                  ya_no_argument,       NULL, 'h' },
    { "debug",                 ya_no_argument,       NULL, 'd' },
    { "optimize",              ya_required_argument, NULL, 'O' },
    { "output",                ya_required_argument, NULL, 'o' },
    { "output-format",         ya_required_argument, NULL, 'f' },
    { "tzx",                   ya_no_argument,       NULL, 'T' },
    { "tap",                   ya_no_argument,       NULL, 't' },
    { "asm",                   ya_no_argument,       NULL, 'A' },
    { "emit-backend",          ya_no_argument,       NULL, 'E' },
    { "parse-only",            ya_no_argument,       NULL, OPT_PARSE_ONLY },
    { "BASIC",                 ya_no_argument,       NULL, 'B' },
    { "autorun",               ya_no_argument,       NULL, 'a' },
    { "org",                   ya_required_argument, NULL, 'S' },
    { "errmsg",                ya_required_argument, NULL, 'e' },
    { "array-base",            ya_required_argument, NULL, OPT_ARRAY_BASE },
    { "string-base",           ya_required_argument, NULL, OPT_STRING_BASE },
    { "sinclair",              ya_no_argument,       NULL, 'Z' },
    { "heap-size",             ya_required_argument, NULL, 'H' },
    { "heap-address",          ya_required_argument, NULL, OPT_HEAP_ADDR },
    { "debug-memory",          ya_no_argument,       NULL, OPT_DEBUG_MEMORY },
    { "debug-array",           ya_no_argument,       NULL, OPT_DEBUG_ARRAY },
    { "strict-bool",           ya_no_argument,       NULL, OPT_STRICT_BOOL },
    { "enable-break",          ya_no_argument,       NULL, OPT_ENABLE_BREAK },
    { "explicit",              ya_no_argument,       NULL, OPT_EXPLICIT },
    { "define",                ya_required_argument, NULL, 'D' },
    { "mmap",                  ya_required_argument, NULL, 'M' },
    { "ignore-case",           ya_no_argument,       NULL, 'i' },
    { "include-path",          ya_required_argument, NULL, 'I' },
    { "strict",                ya_no_argument,       NULL, OPT_STRICT },
    { "headerless",            ya_no_argument,       NULL, OPT_HEADERLESS },
    { "zxnext",                ya_no_argument,       NULL, 'N' },
    { "arch",                  ya_required_argument, NULL, OPT_ARCH },
    { "expect-warnings",       ya_required_argument, NULL, OPT_EXPECT_WARNINGS },
    { "hide-warning-codes",    ya_no_argument,       NULL, OPT_HIDE_WARNING_CODES },
    { "config-file",           ya_required_argument, NULL, 'F' },
    { "save-config",           ya_required_argument, NULL, OPT_SAVE_CONFIG },
    { "version",               ya_no_argument,       NULL, OPT_VERSION },
    { "opt-strategy",          ya_required_argument, NULL, OPT_OPT_STRATEGY },
    { "append-binary",         ya_required_argument, NULL, OPT_APPEND_BIN },
    { "append-headless-binary",ya_required_argument, NULL, OPT_APPEND_HEADLESS_BIN },
    { NULL, 0, NULL, 0 },
};

int main(int argc, char *argv[]) {
    CompilerState cs;
    compiler_init(&cs);

    /* Set cwalk to Unix-style paths for consistent output */
    cwk_path_set_style(CWK_STYLE_UNIX);

    bool parse_only = false;
    int opt;

    ya_opterr = 0;  /* We handle errors ourselves */

    while ((opt = ya_getopt_long(argc, argv, "hdO:o:f:TtAEBaS:e:ZH:D:M:iI:NF:",
                                  long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                compiler_destroy(&cs);
                return 0;
            case 'd':
                cs.opts.debug_level++;
                break;
            case 'O':
                cs.opts.optimization_level = atoi(ya_optarg);
                break;
            case 'o':
                cs.opts.output_filename = ya_optarg;
                break;
            case 'f':
                cs.opts.output_file_type = ya_optarg;
                break;
            case 'T':
                cs.opts.output_file_type = "tzx";
                break;
            case 't':
                cs.opts.output_file_type = "tap";
                break;
            case 'A':
                cs.opts.output_file_type = "asm";
                break;
            case 'E':
                cs.opts.emit_backend = true;
                cs.opts.output_file_type = "ir";
                break;
            case OPT_PARSE_ONLY:
                parse_only = true;
                break;
            case 'B':
                cs.opts.use_basic_loader = true;
                break;
            case 'a':
                cs.opts.autorun = true;
                break;
            case 'S':
                /* org address — will be handled by arch setup */
                break;
            case 'e':
                cs.opts.stderr_filename = ya_optarg;
                break;
            case OPT_ARRAY_BASE:
                cs.opts.array_base = atoi(ya_optarg);
                break;
            case OPT_STRING_BASE:
                cs.opts.string_base = atoi(ya_optarg);
                break;
            case 'Z':
                cs.opts.sinclair = true;
                break;
            case 'H':
                /* heap size — will be handled by arch setup */
                break;
            case OPT_DEBUG_MEMORY:
                cs.opts.memory_check = true;
                break;
            case OPT_DEBUG_ARRAY:
                cs.opts.array_check = true;
                break;
            case OPT_STRICT_BOOL:
                cs.opts.strict_bool = true;
                break;
            case OPT_ENABLE_BREAK:
                cs.opts.enable_break = true;
                break;
            case OPT_EXPLICIT:
                cs.opts.explicit_ = true;
                break;
            case 'D':
                /* preprocessor define — will be passed to zxbpp */
                break;
            case 'M':
                cs.opts.memory_map = ya_optarg;
                break;
            case 'i':
                cs.opts.case_insensitive = true;
                break;
            case 'I':
                cs.opts.include_path = ya_optarg;
                break;
            case OPT_STRICT:
                cs.opts.strict = true;
                break;
            case OPT_HEADERLESS:
                /* headerless mode */
                break;
            case 'N':
                cs.opts.zxnext = true;
                break;
            case OPT_ARCH:
                cs.opts.architecture = ya_optarg;
                break;
            case OPT_EXPECT_WARNINGS:
                cs.opts.expected_warnings = atoi(ya_optarg);
                break;
            case OPT_HIDE_WARNING_CODES:
                cs.opts.hide_warning_codes = true;
                break;
            case 'F':
                cs.opts.project_filename = ya_optarg;
                break;
            case OPT_SAVE_CONFIG:
                /* save config */
                break;
            case OPT_VERSION:
                printf("zxbc %s\n", ZXBASIC_C_VERSION);
                compiler_destroy(&cs);
                return 0;
            case OPT_OPT_STRATEGY:
                if (strcmp(ya_optarg, "size") == 0)
                    cs.opts.opt_strategy = OPT_STRATEGY_SIZE;
                else if (strcmp(ya_optarg, "speed") == 0)
                    cs.opts.opt_strategy = OPT_STRATEGY_SPEED;
                else
                    cs.opts.opt_strategy = OPT_STRATEGY_AUTO;
                break;
            case '?':
                fprintf(stderr, "Unknown option: %s\n", argv[ya_optind - 1]);
                print_usage(argv[0]);
                compiler_destroy(&cs);
                return 1;
            default:
                break;
        }
    }

    /* Remaining argument is the input file */
    if (ya_optind >= argc) {
        fprintf(stderr, "Error: no input file specified\n");
        print_usage(argv[0]);
        compiler_destroy(&cs);
        return 1;
    }

    cs.opts.input_filename = argv[ya_optind];
    cs.current_file = cs.opts.input_filename;

    /* Open error file if specified */
    if (cs.opts.stderr_filename) {
        cs.opts.stderr_f = fopen(cs.opts.stderr_filename, "w");
        if (!cs.opts.stderr_f) {
            fprintf(stderr, "Error: cannot open error file '%s'\n", cs.opts.stderr_filename);
            compiler_destroy(&cs);
            return 1;
        }
    }

    /* TODO: Phase 3 implementation continues here:
     * 1. Read input file
     * 2. Run preprocessor (zxbpp)
     * 3. Lex and parse
     * 4. Build AST
     * 5. Semantic checks
     *
     * For now, just verify the infrastructure works.
     */

    if (cs.opts.debug_level > 0) {
        zxbc_info(&cs, "Input file: %s", cs.opts.input_filename);
        zxbc_info(&cs, "Output format: %s", cs.opts.output_file_type);
        zxbc_info(&cs, "Optimization level: %d", cs.opts.optimization_level);
    }

    /* Read input file */
    FILE *f = fopen(cs.opts.input_filename, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open input file '%s'\n", cs.opts.input_filename);
        compiler_destroy(&cs);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *source = arena_alloc(&cs.arena, fsize + 1);
    size_t nread = fread(source, 1, fsize, f);
    source[nread] = '\0';
    fclose(f);

    /* Parse */
    Parser parser;
    parser_init(&parser, &cs, source);
    AstNode *ast = parser_parse(&parser);

    int rc = 0;
    if (parser.had_error || !ast) {
        rc = 1;
    } else if (parse_only) {
        /* --parse-only: just report success */
        if (cs.opts.debug_level > 0)
            zxbc_info(&cs, "Parse OK (%d top-level statements)", ast->child_count);
    } else {
        /* TODO: semantic checks, code generation */
        fprintf(stderr, "zxbc: code generation not yet implemented (Phase 3 in progress)\n");
        rc = 1;
    }

    /* Cleanup */
    if (cs.opts.stderr_f)
        fclose(cs.opts.stderr_f);

    compiler_destroy(&cs);
    return rc;
}
