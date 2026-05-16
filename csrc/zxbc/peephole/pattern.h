/*
 * pattern.h — Port of src/arch/z80/peephole/pattern.py
 *
 *   BasicLinePattern   : RE_PARSE-split tokenisation, RE_SVAR ($N) binding,
 *                        sanitize() metachar escaping, output token list.
 *   LinePattern.match  : re.match (START-anchored, end UNanchored) with
 *                        the mdict.get(k,v)!=v cross-line conflict reject.
 *   BlockPattern       : list of LinePattern; .match returns the unified
 *                        {"$N": value} dict (empty {} is a valid match).
 *
 * No external regex: the only constructs the generated regex uses are
 *   - sanitized literal chars
 *   - r"\s+"                (whitespace run; 1+)
 *   - r"(?P<_N>.*)"         (greedy, '.' = any non-newline)
 *   - r"\<n>" backref       (NEVER produced by a single .opt LinePattern —
 *                            each $N occurs at most once per line; verified
 *                            across all 52 shipped files. Cross-line var
 *                            unification is the mdict conflict check, not a
 *                            regex backref. Backref support is therefore
 *                            represented in the element model but the
 *                            shipped data never exercises it.)
 * A purpose-built backtracking matcher over an element list reproduces
 * Python's re.match semantics for exactly this construct set.
 */
#ifndef ZXBC_PEEPHOLE_PATTERN_H
#define ZXBC_PEEPHOLE_PATTERN_H

#include <stdbool.h>
#include "arena.h"
#include "vec.h"
#include "hashmap.h"

/* A single regex element. */
typedef enum {
    PE_LIT,   /* one literal character (post-sanitize, matched verbatim) */
    PE_WS,    /* \s+  : one-or-more ASCII whitespace */
    PE_GROUP, /* (?P<_N>.*) : greedy capture into var "_N" */
    PE_BACKREF/* \<n> : backref to the n-th group (1-based) */
} PatElemKind;

typedef struct PatElem {
    PatElemKind kind;
    char        ch;       /* PE_LIT */
    char       *var;      /* PE_GROUP: the "_N" group name (arena) */
    int         ref;      /* PE_BACKREF: 1-based group index */
} PatElem;

typedef VEC(PatElem) PatElemVec;
typedef VEC(char *)   PStrVec;

typedef struct LinePattern {
    char       *line;     /* normalized .line */
    PStrVec     output;   /* .output token list (literal pieces & "$N", "$") */
    PatElemVec  elems;    /* compiled element sequence (the regex) */
    PStrVec     vars;     /* {"$N", ...} (post _N -> $N rename) */
    PStrVec     group_order; /* group var names in declaration order ("_N") */
} LinePattern;

typedef VEC(LinePattern *) LinePatVec;

typedef struct BlockPattern {
    LinePatVec  patterns;
    PStrVec     lines;    /* [p.line for p in patterns] */
    PStrVec     vars;     /* union of all patterns' vars */
} BlockPattern;

/* BasicLinePattern.sanitize: escape r".^$*+?{}[]\|()" — for our matcher
 * the "escape" is a no-op semantically (we match the literal char), so
 * sanitize() is folded into element construction. */

LinePattern *line_pattern_new(Arena *a, const char *line);

/* re.match against `line`, updating `vars_` (a HashMap<str,str> of "$N").
 * Returns true on match. Implements the mdict.get(k,v)!=v reject. */
bool line_pattern_match(Arena *a, const LinePattern *lp,
                        const char *line, HashMap *vars_);

/* Build from list of raw lines (strips, drops empties). */
BlockPattern *block_pattern_new(Arena *a, const char *const *lines, int n);

#define block_pattern_len(bp) ((bp)->lines.len)

/* BlockPattern.match: instructions[start:], len check, per-line match
 * with shared univars. On success, returns a HashMap* of {"$N": v}
 * (arena-allocated); on no-match returns NULL. Empty pattern -> empty
 * (non-NULL) map. */
HashMap *block_pattern_match(Arena *a, const BlockPattern *bp,
                             const char *const *instructions, int n_instr,
                             int start);

#endif /* ZXBC_PEEPHOLE_PATTERN_H */
