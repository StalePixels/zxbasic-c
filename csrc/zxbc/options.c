/*
 * options.c — Compiler options initialization
 */
#include "options.h"
#include <string.h>

void compiler_options_init(CompilerOptions *opts) {
    memset(opts, 0, sizeof(*opts));

    opts->stdin_f = NULL;   /* will use stdin */
    opts->stdout_f = NULL;  /* will use stdout */
    opts->stderr_f = NULL;  /* will use stderr */

    opts->debug_level = 0;
    opts->optimization_level = DEFAULT_OPTIMIZATION_LEVEL;
    opts->case_insensitive = false;
    opts->array_base = 0;
    opts->string_base = 0;
    opts->default_byref = false;
    opts->max_syntax_errors = DEFAULT_MAX_SYNTAX_ERRORS;

    opts->use_basic_loader = false;
    opts->autorun = false;
    opts->output_file_type = "bin";

    opts->include_path = "";

    opts->memory_check = false;
    opts->array_check = false;
    opts->strict_bool = false;

    opts->enable_break = false;
    opts->emit_backend = false;
    opts->explicit_ = false;
    opts->sinclair = false;
    opts->strict = false;

    opts->zxnext = false;
    opts->force_asm_brackets = false;

    opts->org = 32768;         /* 0x8000 — default from arch/z80/backend */
    opts->heap_size = 4768;    /* default from Python */
    opts->heap_address = -1;   /* auto */
    opts->headerless = false;

    opts->expected_warnings = 0;
    opts->hide_warning_codes = false;

    opts->opt_strategy = OPT_STRATEGY_AUTO;
    opts->parse_only = false;
}
