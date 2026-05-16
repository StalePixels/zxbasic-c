/*
 * template.h — Port of src/arch/z80/peephole/template.py
 *
 *   LineTemplate(BasicLinePattern).filter(vars_) -> str
 *   BlockTemplate.filter(vars_) -> list[str]   (drops empties)
 *
 * LineTemplate reuses BasicLinePattern.__init__ (same .output token list
 * built by line_pattern_new); only `filter` differs from LinePattern.
 */
#ifndef ZXBC_PEEPHOLE_TEMPLATE_H
#define ZXBC_PEEPHOLE_TEMPLATE_H

#include <stdbool.h>
#include "arena.h"
#include "vec.h"
#include "hashmap.h"
#include "pattern.h"

typedef struct LineTemplate {
    LinePattern *base; /* shares BasicLinePattern.__init__ output list */
} LineTemplate;

typedef VEC(LineTemplate *) LineTplVec;

typedef struct BlockTemplate {
    LineTplVec templates;
} BlockTemplate;

LineTemplate *line_template_new(Arena *a, const char *line);

/* filter: returns the assembled line (arena), or sets *unbound=true and
 * returns NULL on an unbound "$N" (UnboundVarError). vars_ is "$N"->str. */
char *line_template_filter(Arena *a, const LineTemplate *t,
                           const HashMap *vars_, bool *unbound);

BlockTemplate *block_template_new(Arena *a, const char *const *lines, int n);

/* [y for y in [x.filter(vars_) ...] if y] — appends non-empty results
 * to `out`. Sets *unbound on UnboundVarError (propagates like Python). */
typedef VEC(char *) TplStrVec;
void block_template_filter(Arena *a, const BlockTemplate *bt,
                           const HashMap *vars_, TplStrVec *out,
                           bool *unbound);

#endif /* ZXBC_PEEPHOLE_TEMPLATE_H */
