/*
 * utils.h — Utility functions for ZX BASIC C port
 *
 * Ported from src/api/utils.py
 */
#ifndef ZXBC_UTILS_H
#define ZXBC_UTILS_H

#include <stdbool.h>

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

#endif /* ZXBC_UTILS_H */
