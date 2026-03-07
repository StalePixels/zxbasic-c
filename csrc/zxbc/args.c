/*
 * args.c — Command-line argument parsing for zxbc
 *
 * Extracted from main.c. Ported from src/zxbc/args_parser.py and
 * src/zxbc/args_config.py.
 *
 * Key Python behavior replicated:
 *   - argparse returns None for unspecified boolean flags (--autorun, --BASIC)
 *   - Config file values are loaded FIRST, then cmdline overrides them
 *   - We track which options were explicitly set via opts->cmdline_set bitmask
 */
#include "args.h"
#include "ya_getopt.h"
#include "utils.h"
#include "config_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZXBASIC_C_VERSION
#define ZXBASIC_C_VERSION "dev"
#endif

/* Long option IDs (values > 255 to avoid collision with short opts) */
enum {
    LOPT_ARRAY_BASE = 256,
    LOPT_STRING_BASE,
    LOPT_DEBUG_MEMORY,
    LOPT_DEBUG_ARRAY,
    LOPT_STRICT_BOOL,
    LOPT_ENABLE_BREAK,
    LOPT_EXPLICIT,
    LOPT_STRICT,
    LOPT_HEADERLESS,
    LOPT_ARCH,
    LOPT_EXPECT_WARNINGS,
    LOPT_HIDE_WARNING_CODES,
    LOPT_SAVE_CONFIG,
    LOPT_VERSION,
    LOPT_PARSE_ONLY,
    LOPT_HEAP_ADDR,
    LOPT_APPEND_BIN,
    LOPT_APPEND_HEADLESS_BIN,
    LOPT_OPT_STRATEGY,
    LOPT_DISABLE_WARNING,
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
    { "parse-only",            ya_no_argument,       NULL, LOPT_PARSE_ONLY },
    { "BASIC",                 ya_no_argument,       NULL, 'B' },
    { "autorun",               ya_no_argument,       NULL, 'a' },
    { "org",                   ya_required_argument, NULL, 'S' },
    { "errmsg",                ya_required_argument, NULL, 'e' },
    { "array-base",            ya_required_argument, NULL, LOPT_ARRAY_BASE },
    { "string-base",           ya_required_argument, NULL, LOPT_STRING_BASE },
    { "sinclair",              ya_no_argument,       NULL, 'Z' },
    { "heap-size",             ya_required_argument, NULL, 'H' },
    { "heap-address",          ya_required_argument, NULL, LOPT_HEAP_ADDR },
    { "debug-memory",          ya_no_argument,       NULL, LOPT_DEBUG_MEMORY },
    { "debug-array",           ya_no_argument,       NULL, LOPT_DEBUG_ARRAY },
    { "strict-bool",           ya_no_argument,       NULL, LOPT_STRICT_BOOL },
    { "enable-break",          ya_no_argument,       NULL, LOPT_ENABLE_BREAK },
    { "explicit",              ya_no_argument,       NULL, LOPT_EXPLICIT },
    { "define",                ya_required_argument, NULL, 'D' },
    { "mmap",                  ya_required_argument, NULL, 'M' },
    { "ignore-case",           ya_no_argument,       NULL, 'i' },
    { "include-path",          ya_required_argument, NULL, 'I' },
    { "strict",                ya_no_argument,       NULL, LOPT_STRICT },
    { "headerless",            ya_no_argument,       NULL, LOPT_HEADERLESS },
    { "zxnext",                ya_no_argument,       NULL, 'N' },
    { "arch",                  ya_required_argument, NULL, LOPT_ARCH },
    { "expect-warnings",       ya_required_argument, NULL, LOPT_EXPECT_WARNINGS },
    { "hide-warning-codes",    ya_no_argument,       NULL, LOPT_HIDE_WARNING_CODES },
    { "config-file",           ya_required_argument, NULL, 'F' },
    { "save-config",           ya_required_argument, NULL, LOPT_SAVE_CONFIG },
    { "version",               ya_no_argument,       NULL, LOPT_VERSION },
    { "opt-strategy",          ya_required_argument, NULL, LOPT_OPT_STRATEGY },
    { "append-binary",         ya_required_argument, NULL, LOPT_APPEND_BIN },
    { "append-headless-binary",ya_required_argument, NULL, LOPT_APPEND_HEADLESS_BIN },
    { "disable-warning",       ya_required_argument, NULL, LOPT_DISABLE_WARNING },
    { NULL, 0, NULL, 0 },
};

/* Config file callback — applies key=value pairs to CompilerOptions,
 * but only for fields NOT explicitly set on the cmdline. */
static bool config_apply_option(const char *key, const char *value, void *userdata) {
    CompilerOptions *opts = (CompilerOptions *)userdata;

    if ((strcmp(key, "optimization_level") == 0 || strcmp(key, "optimize") == 0)
        && !(opts->cmdline_set & OPT_SET_OPT_LEVEL)) {
        opts->optimization_level = atoi(value);
    } else if (strcmp(key, "org") == 0 && !(opts->cmdline_set & OPT_SET_ORG)) {
        parse_int(value, &opts->org);
    } else if (strcmp(key, "heap_size") == 0 && !(opts->cmdline_set & OPT_SET_HEAP_SIZE)) {
        opts->heap_size = atoi(value);
    } else if (strcmp(key, "debug_level") == 0 && !(opts->cmdline_set & OPT_SET_DEBUG)) {
        opts->debug_level = atoi(value);
    } else if (strcmp(key, "array_base") == 0 && !(opts->cmdline_set & OPT_SET_ARRAY_BASE)) {
        opts->array_base = atoi(value);
    } else if (strcmp(key, "string_base") == 0 && !(opts->cmdline_set & OPT_SET_STRING_BASE)) {
        opts->string_base = atoi(value);
    } else if (strcmp(key, "case_insensitive") == 0 && !(opts->cmdline_set & OPT_SET_CASE_INS)) {
        opts->case_insensitive = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(key, "strict") == 0 && !(opts->cmdline_set & OPT_SET_STRICT)) {
        opts->strict = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(key, "sinclair") == 0 && !(opts->cmdline_set & OPT_SET_SINCLAIR)) {
        opts->sinclair = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(key, "output_file_type") == 0 && !(opts->cmdline_set & OPT_SET_OUTPUT_TYPE)) {
        opts->output_file_type = strdup(value);
    } else if (strcmp(key, "autorun") == 0 && !(opts->cmdline_set & OPT_SET_AUTORUN)) {
        opts->autorun = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    } else if (strcmp(key, "use_basic_loader") == 0 && !(opts->cmdline_set & OPT_SET_BASIC)) {
        opts->use_basic_loader = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
    }
    return true;
}

int zxbc_parse_args(int argc, char **argv, CompilerOptions *opts) {
    /* Reset ya_getopt state for re-entrant calls (needed by tests).
     * Setting ya_optind=0 triggers ya_getopt's internal reset (clears ya_optnext). */
    ya_optind = 0;
    ya_opterr = 0;

    opts->cmdline_set = 0;

    /* Pre-scan for +W (enable-warning) — ya_getopt doesn't support + prefix */
    {
        int new_argc = 0;
        for (int i = 0; i < argc; i++) {
            if (i > 0 && argv[i][0] == '+' && argv[i][1] == 'W') {
                const char *code = argv[i] + 2;
                if (!code[0] && i + 1 < argc) code = argv[++i];
                opts->enabled_warnings = realloc(opts->enabled_warnings,
                    sizeof(char *) * (opts->enabled_warning_count + 1));
                opts->enabled_warnings[opts->enabled_warning_count++] = (char *)code;
            } else {
                argv[new_argc++] = argv[i];
            }
        }
        argc = new_argc;
    }

    int opt;
    while ((opt = ya_getopt_long(argc, argv, "hdO:o:f:TtAEBaS:e:ZH:D:M:iI:NF:W:",
                                  long_options, NULL)) != -1) {
        switch (opt) {
            case 'h':
                return -1;
            case 'd':
                opts->debug_level++;
                opts->cmdline_set |= OPT_SET_DEBUG;
                break;
            case 'O':
                opts->optimization_level = atoi(ya_optarg);
                opts->cmdline_set |= OPT_SET_OPT_LEVEL;
                break;
            case 'o':
                opts->output_filename = ya_optarg;
                break;
            case 'f':
                opts->output_file_type = ya_optarg;
                opts->cmdline_set |= OPT_SET_OUTPUT_TYPE;
                break;
            case 'T':
                opts->output_file_type = "tzx";
                opts->cmdline_set |= OPT_SET_OUTPUT_TYPE;
                break;
            case 't':
                opts->output_file_type = "tap";
                opts->cmdline_set |= OPT_SET_OUTPUT_TYPE;
                break;
            case 'A':
                opts->output_file_type = "asm";
                opts->cmdline_set |= OPT_SET_OUTPUT_TYPE;
                break;
            case 'E':
                opts->emit_backend = true;
                opts->output_file_type = "ir";
                opts->cmdline_set |= OPT_SET_OUTPUT_TYPE;
                break;
            case LOPT_PARSE_ONLY:
                opts->parse_only = true;
                break;
            case 'B':
                opts->use_basic_loader = true;
                opts->cmdline_set |= OPT_SET_BASIC;
                break;
            case 'a':
                opts->autorun = true;
                opts->cmdline_set |= OPT_SET_AUTORUN;
                break;
            case 'S':
                if (!parse_int(ya_optarg, &opts->org)) {
                    fprintf(stderr, "Error: Invalid --org option '%s'\n", ya_optarg);
                    return 1;
                }
                opts->cmdline_set |= OPT_SET_ORG;
                break;
            case 'e':
                opts->stderr_filename = ya_optarg;
                break;
            case LOPT_ARRAY_BASE:
                opts->array_base = atoi(ya_optarg);
                opts->cmdline_set |= OPT_SET_ARRAY_BASE;
                break;
            case LOPT_STRING_BASE:
                opts->string_base = atoi(ya_optarg);
                opts->cmdline_set |= OPT_SET_STRING_BASE;
                break;
            case 'Z':
                opts->sinclair = true;
                opts->cmdline_set |= OPT_SET_SINCLAIR;
                break;
            case 'H':
                opts->heap_size = atoi(ya_optarg);
                opts->cmdline_set |= OPT_SET_HEAP_SIZE;
                break;
            case LOPT_DEBUG_MEMORY:
                opts->memory_check = true;
                break;
            case LOPT_DEBUG_ARRAY:
                opts->array_check = true;
                break;
            case LOPT_STRICT_BOOL:
                opts->strict_bool = true;
                break;
            case LOPT_ENABLE_BREAK:
                opts->enable_break = true;
                break;
            case LOPT_EXPLICIT:
                opts->explicit_ = true;
                break;
            case 'D':
                /* preprocessor define — will be passed to zxbpp */
                break;
            case 'M':
                opts->memory_map = ya_optarg;
                break;
            case 'i':
                opts->case_insensitive = true;
                opts->cmdline_set |= OPT_SET_CASE_INS;
                break;
            case 'I':
                opts->include_path = ya_optarg;
                break;
            case LOPT_STRICT:
                opts->strict = true;
                opts->cmdline_set |= OPT_SET_STRICT;
                break;
            case LOPT_HEADERLESS:
                opts->headerless = true;
                break;
            case 'N':
                opts->zxnext = true;
                break;
            case LOPT_ARCH:
                opts->architecture = ya_optarg;
                break;
            case LOPT_EXPECT_WARNINGS:
                opts->expected_warnings = atoi(ya_optarg);
                break;
            case LOPT_HIDE_WARNING_CODES:
                opts->hide_warning_codes = true;
                break;
            case 'F':
                opts->project_filename = ya_optarg;
                break;
            case LOPT_SAVE_CONFIG:
                /* save config — not yet implemented */
                break;
            case LOPT_VERSION:
                return -1;
            case 'W':
            case LOPT_DISABLE_WARNING:
                opts->disabled_warnings = realloc(opts->disabled_warnings,
                    sizeof(char *) * (opts->disabled_warning_count + 1));
                opts->disabled_warnings[opts->disabled_warning_count++] = ya_optarg;
                break;
            case LOPT_OPT_STRATEGY:
                if (strcmp(ya_optarg, "size") == 0)
                    opts->opt_strategy = OPT_STRATEGY_SIZE;
                else if (strcmp(ya_optarg, "speed") == 0)
                    opts->opt_strategy = OPT_STRATEGY_SPEED;
                else
                    opts->opt_strategy = OPT_STRATEGY_AUTO;
                break;
            case '?':
                fprintf(stderr, "Unknown option: %s\n", argv[ya_optind - 1]);
                return 1;
            default:
                break;
        }
    }

    /* Remaining argument is the input file */
    if (ya_optind >= argc) {
        fprintf(stderr, "Error: no input file specified\n");
        return 1;
    }
    opts->input_filename = argv[ya_optind];

    /* Load config file if specified (-F).
     * Python loads config FIRST, then cmdline overrides. Our config_apply_option
     * checks cmdline_set bitmask and skips fields that were explicitly set. */
    if (opts->project_filename) {
        config_load_section(opts->project_filename, "zxbc", config_apply_option, opts);
    }

    return 0;
}
