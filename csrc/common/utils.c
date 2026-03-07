/*
 * utils.c — Utility functions for ZX BASIC C port
 *
 * Ported from src/api/utils.py
 */
#include "utils.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

bool parse_int(const char *str, int *out) {
    if (!str || !out) return false;

    /* Skip leading whitespace */
    while (*str && isspace((unsigned char)*str)) str++;
    if (!*str) return false;

    /* Find end, trim trailing whitespace */
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) len--;
    if (len == 0) return false;

    /* Work with a mutable upper-cased copy */
    char buf[64];
    if (len >= sizeof(buf)) return false;
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)toupper((unsigned char)str[i]);
    buf[len] = '\0';

    int base = 10;
    char *num = buf;
    size_t nlen = len;

    if (nlen >= 2 && num[0] == '0' && num[1] == 'X') {
        /* 0xNNNN hex */
        base = 16;
        /* Don't trim — strtol handles 0x prefix */
    } else if (nlen >= 1 && num[nlen - 1] == 'H') {
        /* NNNNh hex — first char must be a digit */
        if (num[0] < '0' || num[0] > '9') return false;
        base = 16;
        num[nlen - 1] = '\0';
        nlen--;
    } else if (nlen >= 1 && num[0] == '$') {
        /* $NNNN hex */
        base = 16;
        num++;
        nlen--;
    } else if (nlen >= 1 && num[0] == '%') {
        /* %NNNN binary */
        base = 2;
        num++;
        nlen--;
    } else if (nlen >= 1 && num[nlen - 1] == 'B') {
        /* NNNNb binary — first char must be 0 or 1 */
        if (num[0] != '0' && num[0] != '1') return false;
        base = 2;
        num[nlen - 1] = '\0';
        nlen--;
    }

    if (nlen == 0) return false;

    char *endp = NULL;
    long val = strtol(num, &endp, base);

    /* Must consume all remaining characters */
    if (endp != num + strlen(num)) return false;

    *out = (int)val;
    return true;
}
