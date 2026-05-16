/*
 * opts_embedded.h — table of the 52 embedded .opt files (verbatim bytes).
 *
 * `content` is the exact byte content of src/arch/z80/peephole/opts/<name>.
 * The engine loader iterates this table the way read_opts() iterates a
 * directory listing: parse each, drop on parse error, stable-sort the
 * survivors by OFLAG, accumulate MAXLEN.
 */
#ifndef ZXBC_PEEPHOLE_OPTS_EMBEDDED_H
#define ZXBC_PEEPHOLE_OPTS_EMBEDDED_H

typedef struct EmbeddedOpt {
    const char *name;    /* basename, e.g. "028_o2_pop_up.opt" */
    const char *content; /* verbatim file bytes */
} EmbeddedOpt;

extern const EmbeddedOpt PEEP_EMBEDDED_OPTS[];
extern const int PEEP_EMBEDDED_OPTS_COUNT;

#endif /* ZXBC_PEEPHOLE_OPTS_EMBEDDED_H */
