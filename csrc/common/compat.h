/*
 * Platform compatibility — Windows (MSVC) vs POSIX.
 *
 * Simple #define mappings for MSVC equivalents of POSIX functions.
 * For getopt, we use ya_getopt (BSD-licensed, bundled in common/).
 */
#ifndef COMPAT_H
#define COMPAT_H

/* GCC/Clang printf format checking — no-op on MSVC */
#if defined(__GNUC__) || defined(__clang__)
    #define PRINTF_FMT(fmtarg, firstva) __attribute__((format(printf, fmtarg, firstva)))
#else
    #define PRINTF_FMT(fmtarg, firstva)
#endif

#ifdef _MSC_VER
    #include <string.h>
    #include <direct.h>
    #include <io.h>
    #include <stdlib.h>

    /* POSIX → MSVC function mappings */
    #define strncasecmp  _strnicmp
    #define strcasecmp   _stricmp
    #define strdup       _strdup
    #define PATH_MAX     _MAX_PATH

    /* access() and R_OK */
    #define access       _access
    #define R_OK         4

    /* realpath → _fullpath, with backslash normalization */
    static inline char *realpath(const char *path, char *resolved) {
        char *result = _fullpath(resolved, path, PATH_MAX);
        if (result) {
            for (char *p = result; *p; p++)
                if (*p == '\\') *p = '/';
        }
        return result;
    }

    /* getcwd → _getcwd, with backslash normalization */
    static inline char *compat_getcwd(char *buf, int size) {
        char *result = _getcwd(buf, size);
        if (result) {
            for (char *p = result; *p; p++)
                if (*p == '\\') *p = '/';
        }
        return result;
    }
    #define getcwd compat_getcwd

    /* dirname: return directory portion of path */
    static inline char *compat_dirname(char *path) {
        if (!path || !*path) return ".";
        char *sep = strrchr(path, '/');
        char *sep2 = strrchr(path, '\\');
        if (sep2 && (!sep || sep2 > sep)) sep = sep2;
        if (!sep) return ".";
        if (sep == path) { path[1] = '\0'; return path; }
        *sep = '\0';
        return path;
    }

    /* basename: return filename portion of path */
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
    /* POSIX */
    #include <unistd.h>
    #include <limits.h>
    #include <strings.h>
    #include <libgen.h>
#endif

#endif /* COMPAT_H */
