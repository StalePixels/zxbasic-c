/*
 * utils.c — Utility functions for ZX BASIC C port
 *
 * Ported from src/api/utils.py
 */
#include "utils.h"
#include "compat.h"   /* PATH_MAX, realpath, getcwd (POSIX/MSVC parity) */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#elif defined(_WIN32)
    #include <windows.h>       /* GetModuleFileNameA */
#endif

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

/* Resolve `argv0` to an absolute executable path: if it contains a path
 * separator treat it as a (possibly relative) path; otherwise search the
 * PATH environment. Fallback only — the OS APIs below are tried first. */
static bool resolve_from_argv0(const char *argv0, char *out, size_t out_size) {
    (void)out_size;  /* out is always PATH_MAX-sized (real_exe); realpath bounds it */
    if (!argv0 || !*argv0)
        return false;

    if (strchr(argv0, '/') || strchr(argv0, '\\')) {
        if (realpath(argv0, out))
            return true;
        return false;
    }

#ifndef _WIN32
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env)
        return false;

    char pathbuf[8192];
    if (strlen(path_env) >= sizeof(pathbuf))
        return false;
    strcpy(pathbuf, path_env);

    char *save = NULL;
    for (char *seg = strtok_r(pathbuf, ":", &save);
         seg != NULL;
         seg = strtok_r(NULL, ":", &save)) {
        char cand[PATH_MAX];
        if (!*seg)
            seg = ".";
        if ((size_t)snprintf(cand, sizeof(cand), "%s/%s", seg, argv0) >= sizeof(cand))
            continue;
        if (access(cand, X_OK) == 0 && realpath(cand, out))
            return true;
    }
#endif
    return false;
}

bool get_executable_dir(const char *argv0, char *out, size_t out_size) {
    char exe[PATH_MAX];
    char real_exe[PATH_MAX];
    bool found = false;

#if defined(__APPLE__)
    {
        uint32_t bufsize = (uint32_t)sizeof(exe);
        if (_NSGetExecutablePath(exe, &bufsize) == 0)
            found = true;
    }
#elif defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
        if (n > 0 && n < sizeof(exe))
            found = true;
    }
#else /* Linux / generic POSIX */
    {
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0 && (size_t)n < sizeof(exe)) {
            exe[n] = '\0';
            found = true;
        }
    }
#endif

    /* Fallback: resolve from argv[0] (path or PATH search). */
    if (!found)
        found = resolve_from_argv0(argv0, real_exe, sizeof(real_exe));
    else if (!realpath(exe, real_exe))
        return false;

    if (!found)
        return false;

    /* Strip the trailing component to get the directory. real_exe is an
     * absolute realpath()'d path; on Windows compat.h's realpath() has
     * already normalised backslashes to '/'. */
    char *slash = strrchr(real_exe, '/');
    if (!slash) {
        /* No separator at all — should not happen for a realpath() result;
         * treat as current directory. */
        if (out_size < 2) return false;
        out[0] = '.';
        out[1] = '\0';
        return true;
    }
    if (slash == real_exe) {
        /* Executable lives in the filesystem root. */
        if (out_size < 2) return false;
        out[0] = '/';
        out[1] = '\0';
        return true;
    }
    *slash = '\0';

    if (strlen(real_exe) + 1 > out_size)
        return false;
    strcpy(out, real_exe);
    return true;
}
