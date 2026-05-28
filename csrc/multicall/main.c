/*
 * Multicall dispatcher — busybox-style single binary that selects the
 * applet from argv[0] basename. Symlink as zxbpp / zxbasm / zxbc.
 * One shared text segment instead of three near-duplicate executables.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cwalk.h"

int zxbpp_main(int argc, char *argv[]);
int zxbasm_main(int argc, char *argv[]);
int zxbc_main(int argc, char *argv[]);

static const char *applet_name(const char *argv0) {
    static char buf[256];
    const char *base_ptr;
    size_t base_len;
    cwk_path_get_basename(argv0, &base_ptr, &base_len);
    if (!base_ptr || base_len == 0) {
        base_ptr = argv0;
        base_len = strlen(argv0);
    }
    if (base_len >= sizeof(buf)) base_len = sizeof(buf) - 1;
    memcpy(buf, base_ptr, base_len);
    buf[base_len] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot && (strcmp(dot, ".exe") == 0 || strcmp(dot, ".EXE") == 0)) *dot = '\0';
    return buf;
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
