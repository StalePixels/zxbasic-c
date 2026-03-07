/*
 * options.h — Compiler options for ZX BASIC compiler
 *
 * Ported from src/api/config.py and src/api/options.py.
 * Instead of the dynamic Options container from Python, we use a flat struct.
 */
#ifndef ZXBC_OPTIONS_H
#define ZXBC_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ----------------------------------------------------------------
 * Optimization strategy (from OptimizationStrategy in config.py)
 * ---------------------------------------------------------------- */
typedef enum {
    OPT_STRATEGY_SIZE  = 0,
    OPT_STRATEGY_SPEED = 1,
    OPT_STRATEGY_AUTO  = 2,
} OptStrategy;

/* ----------------------------------------------------------------
 * Compiler options — flat struct replacing Python's dynamic OPTIONS
 * ---------------------------------------------------------------- */
typedef struct CompilerOptions {
    /* File I/O */
    char *input_filename;
    char *output_filename;
    char *stderr_filename;
    char *project_filename;

    /* Console redirection (NULL = use stdin/stdout/stderr) */
    FILE *stdin_f;
    FILE *stdout_f;
    FILE *stderr_f;

    /* Compilation flags */
    int debug_level;
    int optimization_level;
    bool case_insensitive;
    int array_base;          /* 0 or 1 */
    int string_base;         /* 0 or 1 */
    bool default_byref;
    int max_syntax_errors;

    /* Output control */
    char *memory_map;        /* filename for memory map, or NULL */
    bool use_basic_loader;
    bool autorun;
    char *output_file_type;  /* "bin", "tap", "tzx", "asm", etc. */

    /* Include paths (colon-separated) */
    char *include_path;

    /* Runtime checks */
    bool memory_check;
    bool array_check;
    bool strict_bool;

    /* Language options */
    bool enable_break;
    bool emit_backend;
    bool explicit_;          /* OPTION EXPLICIT */
    bool sinclair;
    bool strict;             /* force type checking */

    /* Architecture */
    char *architecture;      /* "zx48k", "zxnext" */
    bool zxnext;             /* ZX Next extended opcodes */
    bool force_asm_brackets;

    /* Memory layout */
    int org;                 /* start of machine code (default 32768 = 0x8000) */
    int heap_size;           /* heap size in bytes (default 4768) */
    int heap_address;        /* explicit heap address, or -1 for auto */
    bool headerless;         /* omit prologue/epilogue */

    /* Warnings */
    int expected_warnings;
    bool hide_warning_codes;
    char **disabled_warnings;    /* -W codes to disable */
    int disabled_warning_count;
    char **enabled_warnings;     /* +W codes to enable */
    int enabled_warning_count;

    /* Optimization */
    OptStrategy opt_strategy;

    /* Parse-only mode (no output generated) */
    bool parse_only;

    /* Bitmask of options explicitly set on the command line.
     * Used to implement Python's "ignore None" semantics: config file values
     * are only applied for fields NOT set on the cmdline. */
    uint32_t cmdline_set;
} CompilerOptions;

/* Bitmask flags for cmdline_set — tracks which options were explicitly given */
#define OPT_SET_ORG              (1u << 0)
#define OPT_SET_OPT_LEVEL        (1u << 1)
#define OPT_SET_AUTORUN          (1u << 2)
#define OPT_SET_BASIC            (1u << 3)
#define OPT_SET_DEBUG            (1u << 4)
#define OPT_SET_HEAP_SIZE        (1u << 5)
#define OPT_SET_ARRAY_BASE       (1u << 6)
#define OPT_SET_STRING_BASE      (1u << 7)
#define OPT_SET_CASE_INS         (1u << 8)
#define OPT_SET_SINCLAIR         (1u << 9)
#define OPT_SET_STRICT           (1u << 10)
#define OPT_SET_OUTPUT_TYPE      (1u << 11)

/* Initialize options with defaults matching Python's config.init() */
void compiler_options_init(CompilerOptions *opts);

/* ----------------------------------------------------------------
 * Default values (from api/global_.py)
 * ---------------------------------------------------------------- */
#define DEFAULT_OPTIMIZATION_LEVEL 0
#define DEFAULT_MAX_SYNTAX_ERRORS  20

#endif /* ZXBC_OPTIONS_H */
