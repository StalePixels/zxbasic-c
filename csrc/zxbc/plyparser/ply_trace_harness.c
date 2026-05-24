/*
 * ply_trace_harness.c — validation harness for the C ply_engine loop.
 *
 * Reads a token-id stream (from csrc/scripts/ply_tokdump.py: one
 * "<id>\t<name>\t<lineno>\t<value>" per line) on stdin, runs ply_parse with
 * no-op actions and the trace hook, and prints the C engine's shift/reduce/
 * accept/error decision trace — to be diffed against ply_trace_ref.py.
 *
 * This proves the C loop makes the SAME LALR decisions as PLY before any AST
 * action is wired in. Build: standalone (links only ply_engine + ply_tables).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ply_engine.h"

typedef struct {
    int *ids;
    int *linenos;
    int n;
    int pos;
} TokStream;

static bool lex_cb(void *ud, PlySym *out) {
    TokStream *ts = ud;
    if (ts->pos >= ts->n) return false;
    if (ts->ids[ts->pos] == PLY_END_ID) { ts->pos++; return false; }
    out->type = ts->ids[ts->pos];
    out->lineno = ts->linenos[ts->pos];
    out->value = NULL;
    ts->pos++;
    return true;
}

/* No-op action: just return success and a NULL value. We're validating the
 * shift/reduce skeleton only here. (The 3 error-productions would raise a
 * SyntaxError in PLY; on valid inputs they never reduce, so no-op is faithful
 * for the valid-input trace.) */
static bool action_cb(void *ud, int prodno, PlySym *rhs, int len,
                      void **result_value, int *result_lineno) {
    (void)ud; (void)prodno; (void)rhs; (void)len;
    *result_value = NULL;
    *result_lineno = (len > 0) ? rhs[0].lineno : 0;
    return true;
}

static void error_cb(void *ud, const PlySym *errtoken) {
    (void)ud; (void)errtoken;
    /* default: no errok; let the engine run default recovery */
}

static void trace_cb(void *ud, char kind, int arg) {
    (void)ud;
    switch (kind) {
        case 'S': printf("S %d\n", arg); break;
        case 'R': printf("R %d\n", arg); break;
        case 'A': printf("A\n"); break;
        case 'E':
            if (arg == PLY_END_ID) printf("E $end\n");
            else if (arg < PLY_N_TERM) printf("E %s\n", ply_sym_name[arg]);
            else printf("E %s\n", ply_sym_name[arg]);
            break;
    }
}

int main(void) {
    TokStream ts;
    int cap = 256;
    ts.ids = malloc(sizeof(int) * cap);
    ts.linenos = malloc(sizeof(int) * cap);
    ts.n = 0; ts.pos = 0;

    char line[8192];
    int last_lineno = 1;
    while (fgets(line, sizeof(line), stdin)) {
        int id, lineno;
        /* parse leading "<id>\t<name>\t<lineno>" */
        char name[256];
        if (sscanf(line, "%d\t%255[^\t]\t%d", &id, name, &lineno) >= 3) {
            if (ts.n == cap) {
                cap *= 2;
                ts.ids = realloc(ts.ids, sizeof(int) * cap);
                ts.linenos = realloc(ts.linenos, sizeof(int) * cap);
            }
            ts.ids[ts.n] = id;
            ts.linenos[ts.n] = lineno;
            last_lineno = lineno;
            ts.n++;
        }
    }

    PlyParser p;
    ply_parser_init(&p, lex_cb, action_cb, error_cb, &ts);
    p.trace = trace_cb;
    p.cur_lineno = last_lineno;
    ply_parse(&p);

    free(ts.ids);
    free(ts.linenos);
    return 0;
}
