/*
 * Multicall dispatcher — busybox-style single binary that selects the
 * applet from argv[0] basename. Symlink as zxbpp / zxbasm / zxbc.
 * One shared text segment instead of three near-duplicate executables.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>

int zxbpp_main(int argc, char *argv[]);
int zxbasm_main(int argc, char *argv[]);
int zxbc_main(int argc, char *argv[]);

static const char *applet_name(const char *argv0) {
    /* basename() on macOS/glibc may mutate its argument; work on a copy. */
    static char buf[256];
    strncpy(buf, argv0, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *name = basename(buf);
    /* Strip a trailing .exe (Windows). */
    char *dot = strrchr(name, '.');
    if (dot && strcmp(dot, ".exe") == 0) *dot = '\0';
    return name;
}

int main(int argc, char *argv[]) {
    const char *name = applet_name(argv[0]);

    if (strcmp(name, "zxbpp") == 0)  return zxbpp_main(argc, argv);
    if (strcmp(name, "zxbasm") == 0) return zxbasm_main(argc, argv);
    if (strcmp(name, "zxbc") == 0)   return zxbc_main(argc, argv);

    /* If invoked directly (not via symlink), accept the applet as argv[1]. */
    if (argc >= 2) {
        const char *sub = argv[1];
        if (strcmp(sub, "zxbpp") == 0)  return zxbpp_main(argc - 1, argv + 1);
        if (strcmp(sub, "zxbasm") == 0) return zxbasm_main(argc - 1, argv + 1);
        if (strcmp(sub, "zxbc") == 0)   return zxbc_main(argc - 1, argv + 1);
    }

    fprintf(stderr,
        "zxbasic-suite: unknown applet '%s'\n"
        "  Symlink as one of: zxbpp, zxbasm, zxbc\n"
        "  Or invoke as: %s {zxbpp|zxbasm|zxbc} <args>\n",
        name, argv[0]);
    return 1;
}
