/*
 * utils.h — Utility functions for ZX BASIC C port
 *
 * Ported from src/api/utils.py
 */
#ifndef ZXBC_UTILS_H
#define ZXBC_UTILS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * parse_int — Parse an integer from a string supporting multiple formats.
 *
 * Formats: decimal, hex (0xNNNN, $NNNN, NNNNh), binary (%NNNN, NNNNb).
 * Hex numbers starting with a letter (e.g. "A0h") are ambiguous and return false.
 * Such numbers must be prefixed with 0 (e.g. "0A0h").
 *
 * Returns true on success (result stored in *out), false on failure.
 */
bool parse_int(const char *str, int *out);

/*
 * get_executable_dir — Absolute directory containing the running executable.
 *
 * C has no __file__; this is the analogue of zxbpp.get_include_path()'s
 * os.path.dirname(__file__) anchor (src/zxbpp/zxbpp.py:142-152), which is
 * CWD-independent. Uses, in order: macOS _NSGetExecutablePath, Linux
 * /proc/self/exe, Windows GetModuleFileNameA; final fallback
 * realpath(argv0) / PATH search (argv0 may be NULL if unavailable).
 *
 * Writes the realpath()-resolved absolute directory (no trailing slash)
 * into `out` (capacity `out_size`). Returns true on success, false if the
 * path could not be determined.
 */
bool get_executable_dir(const char *argv0, char *out, size_t out_size);

/*
 * get_lib_include_root — Root directory under which arch/<arch>/{stdlib,
 * runtime} lookups are performed by zxbpp / zxbc / asm_bridge / codegen.
 *
 * Resolution order:
 *   1. If $ZXBASIC_INC_PATH is set and non-empty, use that verbatim.
 *      Lets a package install (e.g. /usr/share/zxbasic-c) point the
 *      toolchain at lib resources outside the dev-tree layout.
 *   2. Otherwise compute realpath(<exe_dir>/../../../src/lib) — the
 *      faithful default that mirrors Python's
 *      os.path.dirname(__file__)/.. anchor and matches the dev-tree
 *      shape <root>/csrc/build/bin/<tool> -> <root>/src/lib.
 *
 * Callers append "/arch/<arch>/stdlib" and "/arch/<arch>/runtime" as
 * before. Writes the absolute path (no trailing slash) into out;
 * returns false if neither resolution succeeds.
 */
bool get_lib_include_root(const char *argv0, char *out, size_t out_size);

#endif /* ZXBC_UTILS_H */
