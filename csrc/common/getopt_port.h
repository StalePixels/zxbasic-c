/*
 * Portable getopt / getopt_long for platforms without POSIX getopt.h (e.g. MSVC).
 * On POSIX systems, this just includes the system <getopt.h>.
 */
#ifndef GETOPT_PORT_H
#define GETOPT_PORT_H

#ifdef _MSC_VER

/* Minimal getopt implementation for MSVC */
#include <string.h>
#include <stdio.h>

static char *optarg = NULL;
static int optind = 1;
static int opterr = 1;
static int optopt = 0;

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

#define no_argument       0
#define required_argument 1
#define optional_argument 2

static int getopt_long(int argc, char *const argv[], const char *optstring,
                       const struct option *longopts, int *longindex)
{
    static int pos = 0; /* position within grouped short opts */

    optarg = NULL;

    while (optind < argc) {
        const char *arg = argv[optind];

        if (pos == 0) {
            /* Not in the middle of grouped short opts */
            if (arg[0] != '-' || arg[1] == '\0') return -1; /* not an option */

            if (arg[1] == '-') {
                if (arg[2] == '\0') { optind++; return -1; } /* "--" */

                /* Long option */
                const char *eq = strchr(arg + 2, '=');
                size_t namelen = eq ? (size_t)(eq - arg - 2) : strlen(arg + 2);

                for (int i = 0; longopts && longopts[i].name; i++) {
                    if (strncmp(longopts[i].name, arg + 2, namelen) == 0 &&
                        strlen(longopts[i].name) == namelen) {
                        if (longindex) *longindex = i;
                        optind++;
                        if (longopts[i].has_arg) {
                            if (eq) {
                                optarg = (char *)(eq + 1);
                            } else if (optind < argc) {
                                optarg = argv[optind++];
                            } else {
                                if (opterr) fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], longopts[i].name);
                                return '?';
                            }
                        }
                        if (longopts[i].flag) {
                            *longopts[i].flag = longopts[i].val;
                            return 0;
                        }
                        return longopts[i].val;
                    }
                }
                if (opterr) fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], arg);
                optind++;
                return '?';
            }
        }

        /* Short option(s) */
        if (pos == 0) pos = 1;
        char c = arg[pos];
        const char *p = strchr(optstring, c);

        if (!p || c == ':') {
            optopt = c;
            if (opterr) fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
            pos++;
            if (arg[pos] == '\0') { optind++; pos = 0; }
            return '?';
        }

        if (p[1] == ':') {
            /* Requires argument */
            if (arg[pos + 1] != '\0') {
                optarg = (char *)&arg[pos + 1];
            } else {
                optind++;
                if (optind < argc) {
                    optarg = argv[optind];
                } else {
                    if (opterr) fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
                    pos = 0;
                    optind++;
                    return (optstring[0] == ':') ? ':' : '?';
                }
            }
            optind++;
            pos = 0;
            return c;
        }

        /* No argument */
        pos++;
        if (arg[pos] == '\0') { optind++; pos = 0; }
        return c;
    }

    return -1;
}

#else
    #include <getopt.h>
#endif

#endif /* GETOPT_PORT_H */
