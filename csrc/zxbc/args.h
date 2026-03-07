/*
 * args.h — Command-line argument parsing for zxbc
 *
 * Extracted from main.c so tests can call zxbc_parse_args() directly
 * and verify option values (matching Python's tests/cmdline/test_zxb.py).
 */
#ifndef ZXBC_ARGS_H
#define ZXBC_ARGS_H

#include "options.h"

/*
 * Parse command-line arguments into CompilerOptions.
 *
 * Handles:
 *   - All CLI flags (--org, --parse-only, -O, etc.)
 *   - Config file loading (-F) with proper cmdline override semantics
 *   - +W enable-warning extraction
 *
 * Returns 0 on success, 1 on error, -1 for early exit (--help, --version).
 * On success, opts->input_filename is set to the positional argument.
 */
int zxbc_parse_args(int argc, char **argv, CompilerOptions *opts);

#endif /* ZXBC_ARGS_H */
