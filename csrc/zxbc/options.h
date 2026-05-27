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

    /* Whether each deprecated output-format flag (and -f/--output-format)
     * was seen on the command line. Mirrors Python's options.output_format
     * / options.tzx / options.tap / options.asm / options.emit_backend
     * truthiness in args_config.py:107-128 — used ONLY to drive the
     * deprecation-warning elif chain; output_file_type resolution itself
     * is unchanged (last-wins, out of S7.2b scope). memset-zeroed by
     * compiler_options_init like the other bool fields. */
    bool opt_seen_output_format;
    bool opt_seen_tzx;
    bool opt_seen_tap;
    bool opt_seen_asm;
    bool opt_seen_emit_backend;

    /* S7.2d-i — faithful port of the args_parser.py:64-99
     * add_mutually_exclusive_group() {-T/--tzx, -t/--tap, -A/--asm,
     * -E/--emit-backend, --parse-only, -f/--output-format}. argparse
     * rejects when >=2 group members are given, reporting
     *   "argument <second-seen>: not allowed with argument <first-seen>"
     * using each action's canonical '/'-joined option_strings, in the
     * left-to-right order the members were seen. We record the
     * first-seen and second-seen member as a small enum + the
     * canonical argparse spelling, populated in the getopt loop (which
     * processes tokens left-to-right; ya_getopt splits combined short
     * opts like -tT into successive returns, so the loop sees the true
     * order). 0 = none seen yet. */
    int mutex_seen_count;            /* how many distinct group members seen */
    const char *mutex_first_optstr;  /* canonical argparse spelling, 1st seen */
    const char *mutex_second_optstr; /* canonical argparse spelling, 2nd seen */

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

    /* Preprocessor -D/--define raw strings, in order, one per -D
     * (Python args_parser.py:143-150 dest=defines action=append). Split
     * on first '=' at seed time, faithful to args_config.py:91-96. */
    char **defines;
    int defines_count;

    /* --append-binary / --append-headless-binary filenames, in order,
     * one per occurrence (Python args_parser.py:175-183 default=[]
     * action="append" -> options.append_binary /
     * options.append_headless_binary are ordered lists of filename
     * strings). Raw ya_optarg pointers, realloc-grown arrays — exactly
     * the `defines` idiom. asmparse.generate_binary (asmparse.py:1013-
     * 1044) reads each file's raw bytes; headed = (basename, bytes),
     * headless = bytes. Zero-init by compiler_options_init's memset,
     * same convention as the `defines` pair. */
    char **append_binary;
    int append_binary_count;
    char **append_headless_binary;
    int append_headless_binary_count;

    /* Optimization */
    OptStrategy opt_strategy;

    /* Parse-only mode (no output generated) */
    bool parse_only;

    /* --save-config FILE (Python args_parser.py:213
     * parser.add_argument("--save-config", type=str) -> options.save_config
     * is a filename string or None). When set AND the compile had no
     * errors, zxbc.py:71-73 save_config() writes the [zxbc] config INI
     * via src.api.config.save_config_into_file (config.py:143-186).
     * Raw ya_optarg pointer (NULL = not given), same convention as the
     * other char* filename opts (memory_map, stderr_filename). */
    char *save_config;

    /* Bitmask of options explicitly set on the command line.
     * Used to implement Python's "ignore None" semantics: config file values
     * are only applied for fields NOT set on the cmdline. */
    uint32_t cmdline_set;

    /* #pragma push/pop stacks (Python anchor: src/api/options.py:146-160
     * Option.push/.pop on a per-Option `stack: list[Any]`). Each known
     * stackable option keeps a tiny fixed-depth LIFO of int values
     * (bool stores 0/1; int stores the value verbatim). Top is at
     * pragma_stack_depth[i]-1. Library code in the wild only pushes
     * case_insensitive and string_base, but any bool/int option that
     * apply_pragma_option recognises is also stackable in Python via
     * the same generic OPTIONS[name].push()/pop() — see Python anchor
     * src/zxbc/zxbparser.py:3248-3263. Depth 16 is plenty for
     * realistic include trees (each lib pushes once at top, pops at
     * bottom; deepest observed is ~4 nested through nextlib + sublib). */
#define PRAGMA_STACK_MAX 16
    int pragma_stack_case_insensitive[PRAGMA_STACK_MAX];
    int pragma_stack_string_base[PRAGMA_STACK_MAX];
    int pragma_stack_array_base[PRAGMA_STACK_MAX];
    int pragma_stack_explicit[PRAGMA_STACK_MAX];
    int pragma_stack_strict[PRAGMA_STACK_MAX];
    int pragma_stack_strict_bool[PRAGMA_STACK_MAX];
    int pragma_stack_memory_check[PRAGMA_STACK_MAX];
    int pragma_stack_array_check[PRAGMA_STACK_MAX];
    int pragma_stack_enable_break[PRAGMA_STACK_MAX];
    int pragma_stack_default_byref[PRAGMA_STACK_MAX];
    int pragma_stack_sinclair[PRAGMA_STACK_MAX];
    int pragma_stack_zxnext[PRAGMA_STACK_MAX];
    int pragma_stack_headerless[PRAGMA_STACK_MAX];
    int pragma_stack_optimization_level[PRAGMA_STACK_MAX];
    int pragma_stack_debug_level[PRAGMA_STACK_MAX];
    int pragma_stack_org[PRAGMA_STACK_MAX];
    int pragma_stack_heap_size[PRAGMA_STACK_MAX];
    int pragma_stack_heap_address[PRAGMA_STACK_MAX];
    int pragma_stack_max_syntax_errors[PRAGMA_STACK_MAX];
    int pragma_stack_expected_warnings[PRAGMA_STACK_MAX];
    /* depth tracker per stack — keyed by the same enum-like ordinal as the
     * arrays above (use option_pragma_stack_ptr to dispatch). 0 = empty. */
    int pragma_stack_depth[20];
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
#define OPT_SET_HEAP_ADDR        (1u << 12)

/* Initialize options with defaults matching Python's config.init() */
void compiler_options_init(CompilerOptions *opts);

/* Split a raw -D string on the FIRST '=' exactly like Python's
 * args_config.py:91-96  macro = list(i.split("=", 1));
 * name = macro[0]; val = "".join(macro[1:]).
 *
 *   "FOO"          -> name "FOO",     val ""
 *   "FOO=BAR=BAZ"  -> name "FOO",     val "BAR=BAZ"
 *   "=X"           -> name "",        val "X"     (reproduced faithfully)
 *
 * `raw` is copied into caller-provided `scratch` (>= strlen(raw)+1) so
 * the returned `*name_out`/`*val_out` point into `scratch` and remain
 * valid for the caller's lifetime. */
void compiler_split_define(const char *raw, char *scratch,
                           const char **name_out, const char **val_out);

/* ----------------------------------------------------------------
 * Default values (from api/global_.py)
 * ---------------------------------------------------------------- */
/* Python global_.py:178 DEFAULT_OPTIMIZATION_LEVEL=2 (effective
 * parse-only default; -O unset keeps the config default via
 * ignore_none). Without this S2.4's OptimizerVisitor O-gate makes the
 * pass inert and Phase-5 AST-equivalence cannot converge. */
#define DEFAULT_OPTIMIZATION_LEVEL 2
#define DEFAULT_MAX_SYNTAX_ERRORS  20

#endif /* ZXBC_OPTIONS_H */
