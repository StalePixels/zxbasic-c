/*
 * Platform compatibility shims for Windows (MSVC) vs POSIX.
 */
#ifndef COMPAT_H
#define COMPAT_H

/* GCC/Clang format attribute — no-op on MSVC */
#if defined(__GNUC__) || defined(__clang__)
    #define PRINTF_FMT(fmtarg, firstva) __attribute__((format(printf, fmtarg, firstva)))
#else
    #define PRINTF_FMT(fmtarg, firstva)
#endif

#ifdef _MSC_VER
    /* MSVC doesn't have these POSIX functions */
    #include <string.h>
    #include <direct.h>
    #include <io.h>
    #include <stdlib.h>

    #define strncasecmp  _strnicmp
    #define strcasecmp   _stricmp
    #define getcwd       _getcwd
    #define strdup       _strdup
    #define PATH_MAX     _MAX_PATH

    /* realpath: MSVC has _fullpath */
    static inline char *realpath(const char *path, char *resolved) {
        return _fullpath(resolved, path, PATH_MAX);
    }

    /* dirname/basename: simple implementations for MSVC */
    static inline char *compat_dirname(char *path) {
        if (!path || !*path) return ".";
        /* Find last separator */
        char *sep = strrchr(path, '/');
        char *sep2 = strrchr(path, '\\');
        if (sep2 && (!sep || sep2 > sep)) sep = sep2;
        if (!sep) return ".";
        if (sep == path) { path[1] = '\0'; return path; }
        *sep = '\0';
        return path;
    }

    static inline char *compat_basename(char *path) {
        if (!path || !*path) return ".";
        char *sep = strrchr(path, '/');
        char *sep2 = strrchr(path, '\\');
        if (sep2 && (!sep || sep2 > sep)) sep = sep2;
        return sep ? sep + 1 : path;
    }

    #define dirname  compat_dirname
    #define basename compat_basename
#else
    #include <unistd.h>
    #include <limits.h>
    #include <strings.h>
    #include <libgen.h>
#endif

#endif /* COMPAT_H */
