/*
 * zxbpp — ZX BASIC Preprocessor (C port)
 *
 * Main header file shared between lexer, parser, and driver.
 */
#ifndef ZXBPP_H
#define ZXBPP_H

#include "strbuf.h"
#include "hashmap.h"
#include "vec.h"
#include "arena.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * Macro definition
 */
typedef struct MacroParam {
    char *name;
} MacroParam;

typedef struct MacroDef {
    char *name;
    char *body;          /* expansion text */
    int num_params;      /* -1 = object-like, 0+ = function-like */
    char **param_names;
    bool is_builtin;
    bool evaluating;     /* recursion guard */
    int def_line;        /* line where defined */
    char *def_file;      /* file where defined */
} MacroDef;

/*
 * Conditional compilation stack entry
 */
typedef struct IfDefEntry {
    bool enabled;        /* is this branch active? */
    bool else_seen;      /* have we seen #else yet? */
    int line;            /* line of the #if/#ifdef */
} IfDefEntry;

/*
 * Include file tracking
 */
typedef struct IncludeInfo {
    bool once;           /* #pragma once seen */
} IncludeInfo;

/*
 * File stack entry for #include nesting
 */
typedef struct FileStackEntry {
    char *filename;
    int lineno;
    char *content;       /* file content */
    int pos;             /* current position in content */
} FileStackEntry;

/*
 * Preprocessor state
 */
typedef struct PreprocState {
    /* Memory */
    Arena arena;

    /* Output buffer */
    StrBuf output;

    /* Macro definitions table */
    HashMap macros;      /* key: name, value: MacroDef* */

    /* Conditional compilation stack */
    VEC(IfDefEntry) ifdef_stack;

    /* Include tracking */
    HashMap included;    /* key: abs path, value: IncludeInfo* */

    /* File stack for include nesting */
    VEC(FileStackEntry) file_stack;

    /* Current file info */
    char *current_file;
    int current_line;

    /* Include paths */
    VEC(char *) include_paths;

    /* Options */
    char *arch;          /* target architecture, e.g. "zx48k" */
    int debug_level;
    bool enabled;        /* false when inside a false #ifdef branch */
    int warning_count;
    int error_count;
    int expect_warnings; /* suppress first N warnings */

    /* Output tracking: has_output is false until the first content line
     * is emitted. Used to decide whether #define emits a blank line
     * (first production) or a #line directive (subsequent). */
    bool has_output;

    /* ASM mode: inside asm..end asm block, comment char is ; not ' */
    bool in_asm;

    /* Block comment: inside /' ... '/ multi-line comment */
    bool in_block_comment;

    /* Error output */
    FILE *err_file;
} PreprocState;

/* Initialize the preprocessor state */
void preproc_init(PreprocState *pp);

/* Destroy the preprocessor state */
void preproc_destroy(PreprocState *pp);

/* Process an input file, appending to pp->output */
int preproc_file(PreprocState *pp, const char *filename);

/* Process an input string, appending to pp->output */
int preproc_string(PreprocState *pp, const char *input, const char *filename);

/* Define a macro (object-like, no params) */
void preproc_define(PreprocState *pp, const char *name, const char *body,
                    int line, const char *file);

/* Define a function-like macro */
void preproc_define_func(PreprocState *pp, const char *name, const char *body,
                         int num_params, char **param_names,
                         int line, const char *file);

/* Undefine a macro */
void preproc_undef(PreprocState *pp, const char *name);

/* Check if a macro is defined */
bool preproc_is_defined(PreprocState *pp, const char *name);

/* Expand a macro by name, returns expanded text (arena-allocated) */
char *preproc_expand_macro(PreprocState *pp, const char *name,
                           int argc, char **argv);

/* Emit a #line directive to output */
void preproc_emit_line(PreprocState *pp, int line, const char *file);

/* Emit a warning */
void preproc_warning(PreprocState *pp, int code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Emit an error */
void preproc_error(PreprocState *pp, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* ZXBPP_H */
