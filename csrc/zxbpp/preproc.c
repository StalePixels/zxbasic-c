/*
 * zxbpp preprocessor engine.
 *
 * Line-oriented recursive-descent preprocessor. Instead of using bison
 * (which would be overkill and hard to match PLY's exact behavior), we
 * parse directives directly in C. The preprocessor:
 *
 * 1. Reads input line by line
 * 2. Recognizes # directives at the start of lines
 * 3. Expands macros in non-directive lines
 * 4. Handles #include by recursing into included files
 * 5. Manages conditional compilation (#ifdef/#endif stack)
 * 6. Emits #line directives for source tracking
 */
#include "zxbpp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libgen.h>
#include <limits.h>
#include <unistd.h>

/* Forward declarations */
static void process_line(PreprocState *pp, const char *line);
static void process_directive(PreprocState *pp, const char *directive);
static char *expand_macros_in_text(PreprocState *pp, const char *text);
static char *read_file(const char *path);
static char *resolve_include(PreprocState *pp, const char *name, bool is_system);

/* ----------------------------------------------------------------
 * Initialization / Destruction
 * ---------------------------------------------------------------- */

void preproc_init(PreprocState *pp)
{
    arena_init(&pp->arena, 0);
    strbuf_init(&pp->output);
    hashmap_init(&pp->macros);
    vec_init(pp->ifdef_stack);
    hashmap_init(&pp->included);
    vec_init(pp->file_stack);
    pp->current_file = NULL;
    pp->current_line = 0;
    vec_init(pp->include_paths);
    pp->arch = NULL;
    pp->debug_level = 0;
    pp->enabled = true;
    pp->warning_count = 0;
    pp->error_count = 0;
    pp->expect_warnings = 0;
    pp->has_output = false;
    pp->in_asm = false;
    pp->err_file = stderr;
}

void preproc_destroy(PreprocState *pp)
{
    strbuf_free(&pp->output);
    hashmap_free(&pp->macros);
    vec_free(pp->ifdef_stack);
    hashmap_free(&pp->included);
    vec_free(pp->file_stack);
    vec_free(pp->include_paths);
    arena_destroy(&pp->arena);
}

/* ----------------------------------------------------------------
 * Warning / Error reporting
 * ---------------------------------------------------------------- */

void preproc_warning(PreprocState *pp, int code, const char *fmt, ...)
{
    pp->warning_count++;
    if (pp->expect_warnings > 0 && pp->warning_count <= pp->expect_warnings)
        return;

    va_list ap;
    va_start(ap, fmt);
    if (pp->current_file)
        fprintf(pp->err_file, "%s:%d: ", pp->current_file, pp->current_line);
    if (code > 0)
        fprintf(pp->err_file, "warning: [W%d] ", code);
    else
        fprintf(pp->err_file, "warning: ");
    vfprintf(pp->err_file, fmt, ap);
    fprintf(pp->err_file, "\n");
    va_end(ap);
}

void preproc_error(PreprocState *pp, const char *fmt, ...)
{
    pp->error_count++;
    va_list ap;
    va_start(ap, fmt);
    if (pp->current_file)
        fprintf(pp->err_file, "%s:%d: ", pp->current_file, pp->current_line);
    fprintf(pp->err_file, "error: ");
    vfprintf(pp->err_file, fmt, ap);
    fprintf(pp->err_file, "\n");
    va_end(ap);
}

/* ----------------------------------------------------------------
 * #line directive emission
 * ---------------------------------------------------------------- */

void preproc_emit_line(PreprocState *pp, int line, const char *file)
{
    if (file)
        strbuf_printf(&pp->output, "#line %d \"%s\"\n", line, file);
    else
        strbuf_printf(&pp->output, "#line %d\n", line);
}

/* ----------------------------------------------------------------
 * Macro management
 * ---------------------------------------------------------------- */

void preproc_define(PreprocState *pp, const char *name, const char *body,
                    int line, const char *file)
{
    MacroDef *existing = hashmap_get(&pp->macros, name);
    if (existing) {
        if (existing->is_builtin) {
            preproc_warning(pp, 500, "builtin macro \"%s\" redefined", name);
        } else {
            preproc_warning(pp, 510, "\"%s\" redefined (previous definition at %s:%d)",
                          name, existing->def_file ? existing->def_file : "?",
                          existing->def_line);
        }
    }

    MacroDef *def = arena_calloc(&pp->arena, 1, sizeof(MacroDef));
    def->name = arena_strdup(&pp->arena, name);
    def->body = body ? arena_strdup(&pp->arena, body) : arena_strdup(&pp->arena, "");
    def->num_params = -1; /* object-like */
    def->param_names = NULL;
    def->is_builtin = false;
    def->evaluating = false;
    def->def_line = line;
    def->def_file = file ? arena_strdup(&pp->arena, file) : NULL;

    hashmap_set(&pp->macros, name, def);
}

void preproc_define_func(PreprocState *pp, const char *name, const char *body,
                         int num_params, char **param_names,
                         int line, const char *file)
{
    MacroDef *existing = hashmap_get(&pp->macros, name);
    if (existing) {
        if (existing->is_builtin) {
            preproc_warning(pp, 500, "builtin macro \"%s\" redefined", name);
        } else {
            preproc_warning(pp, 510, "\"%s\" redefined (previous definition at %s:%d)",
                          name, existing->def_file ? existing->def_file : "?",
                          existing->def_line);
        }
    }

    MacroDef *def = arena_calloc(&pp->arena, 1, sizeof(MacroDef));
    def->name = arena_strdup(&pp->arena, name);
    def->body = body ? arena_strdup(&pp->arena, body) : arena_strdup(&pp->arena, "");
    def->num_params = num_params;
    if (num_params > 0 && param_names) {
        def->param_names = arena_calloc(&pp->arena, (size_t)num_params, sizeof(char *));
        for (int i = 0; i < num_params; i++)
            def->param_names[i] = arena_strdup(&pp->arena, param_names[i]);
    }
    def->is_builtin = false;
    def->evaluating = false;
    def->def_line = line;
    def->def_file = file ? arena_strdup(&pp->arena, file) : NULL;

    hashmap_set(&pp->macros, name, def);
}

void preproc_undef(PreprocState *pp, const char *name)
{
    hashmap_remove(&pp->macros, name);
}

bool preproc_is_defined(PreprocState *pp, const char *name)
{
    return hashmap_has(&pp->macros, name);
}

/* ----------------------------------------------------------------
 * Builtin macros
 * ---------------------------------------------------------------- */

static void register_builtins(PreprocState *pp)
{
    /* Builtins are handled specially during expansion, but we
     * register them so preproc_is_defined() works for #ifdef */
    const char *builtins[] = {"__FILE__", "__LINE__", "__BASE_FILE__", "__ABS_FILE__", NULL};
    for (int i = 0; builtins[i]; i++) {
        MacroDef *def = arena_calloc(&pp->arena, 1, sizeof(MacroDef));
        def->name = arena_strdup(&pp->arena, builtins[i]);
        def->body = arena_strdup(&pp->arena, "");
        def->num_params = -1;
        def->is_builtin = true;
        def->evaluating = false;
        def->def_line = 0;
        def->def_file = NULL;
        hashmap_set(&pp->macros, builtins[i], def);
    }
}

static char *expand_builtin(PreprocState *pp, const char *name)
{
    if (strcmp(name, "__LINE__") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", pp->current_line);
        return arena_strdup(&pp->arena, buf);
    }
    if (strcmp(name, "__FILE__") == 0) {
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_printf(&sb, "\"%s\"", pp->current_file ? pp->current_file : "");
        char *result = arena_strdup(&pp->arena, strbuf_cstr(&sb));
        strbuf_free(&sb);
        return result;
    }
    if (strcmp(name, "__BASE_FILE__") == 0) {
        /* basename only */
        if (!pp->current_file) return arena_strdup(&pp->arena, "\"\"");
        char *tmp = arena_strdup(&pp->arena, pp->current_file);
        char *base = basename(tmp);
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_printf(&sb, "\"%s\"", base);
        char *result = arena_strdup(&pp->arena, strbuf_cstr(&sb));
        strbuf_free(&sb);
        return result;
    }
    if (strcmp(name, "__ABS_FILE__") == 0) {
        if (!pp->current_file) return arena_strdup(&pp->arena, "\"\"");
        char abspath[PATH_MAX];
        if (realpath(pp->current_file, abspath)) {
            StrBuf sb;
            strbuf_init(&sb);
            strbuf_printf(&sb, "\"%s\"", abspath);
            char *result = arena_strdup(&pp->arena, strbuf_cstr(&sb));
            strbuf_free(&sb);
            return result;
        }
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_printf(&sb, "\"%s\"", pp->current_file);
        char *result = arena_strdup(&pp->arena, strbuf_cstr(&sb));
        strbuf_free(&sb);
        return result;
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Utility: string helpers
 * ---------------------------------------------------------------- */

/* Skip whitespace, return pointer to first non-space char */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Check if character can start an identifier */
static bool is_id_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* Check if character can continue an identifier */
static bool is_id_char(char c)
{
    return is_id_start(c) || (c >= '0' && c <= '9');
}

/* Extract an identifier from position, return length */
static int scan_id(const char *s)
{
    int len = 0;
    if (!is_id_start(s[0])) return 0;
    while (is_id_char(s[len])) len++;
    return len;
}

/* Case-insensitive string comparison for n characters */
static int strnicmp_local(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ca = (char)tolower((unsigned char)a[i]);
        char cb = (char)tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

/* Strip trailing whitespace and newline from a string (in-place) */
static void strip_trailing(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' ' || s[len-1] == '\t'))
        len--;
    s[len] = '\0';
}

/* Read entire file into malloc'd string */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t nread = fread(buf, 1, (size_t)size, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

/* ----------------------------------------------------------------
 * Include path resolution
 * ---------------------------------------------------------------- */

static char *resolve_include(PreprocState *pp, const char *name, bool is_system)
{
    char path[PATH_MAX];

    /* For local includes ("file"), try current file's directory first */
    if (!is_system && pp->current_file) {
        char *dir_tmp = arena_strdup(&pp->arena, pp->current_file);
        char *dir = dirname(dir_tmp);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        if (access(path, R_OK) == 0) {
            return arena_strdup(&pp->arena, path);
        }
    }

    /* Try include paths */
    for (int i = 0; i < pp->include_paths.len; i++) {
        snprintf(path, sizeof(path), "%s/%s", pp->include_paths.data[i], name);
        if (access(path, R_OK) == 0) {
            return arena_strdup(&pp->arena, path);
        }
    }

    /* Try current directory */
    if (access(name, R_OK) == 0) {
        return arena_strdup(&pp->arena, name);
    }

    return NULL;
}

/* ----------------------------------------------------------------
 * Macro expansion
 * ---------------------------------------------------------------- */

/* Parse macro arguments from text starting at '('.
 * Returns number of args parsed, fills argv array.
 * *end_pos is set to position after closing ')'. */
static int parse_macro_args(const char *text, int start, char **argv, int max_args,
                           int *end_pos, Arena *arena)
{
    int argc = 0;
    int pos = start;

    if (text[pos] != '(') {
        *end_pos = pos;
        return 0;
    }
    pos++; /* skip '(' */

    StrBuf arg;
    strbuf_init(&arg);
    int depth = 1;

    while (text[pos] && depth > 0) {
        if (text[pos] == '(') {
            depth++;
            strbuf_append_char(&arg, text[pos]);
        } else if (text[pos] == ')') {
            depth--;
            if (depth == 0) {
                /* End of args */
                if (argc < max_args) {
                    char *original = strbuf_detach(&arg);
                    char *s = original;
                    /* Trim whitespace */
                    while (*s == ' ' || *s == '\t') s++;
                    char *end = s + strlen(s);
                    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
                    *end = '\0';
                    argv[argc] = arena_strdup(arena, s);
                    free(original);
                }
                argc++;
                pos++; /* skip ')' */
                break;
            } else {
                strbuf_append_char(&arg, text[pos]);
            }
        } else if (text[pos] == ',' && depth == 1) {
            if (argc < max_args) {
                char *original = strbuf_detach(&arg);
                char *s = original;
                while (*s == ' ' || *s == '\t') s++;
                argv[argc] = arena_strdup(arena, s);
                free(original);
            }
            argc++;
            strbuf_init(&arg);
        } else {
            strbuf_append_char(&arg, text[pos]);
        }
        pos++;
    }

    strbuf_free(&arg);
    *end_pos = pos;
    return argc;
}

/* Substitute parameters in macro body */
static char *substitute_params(PreprocState *pp, const MacroDef *def,
                               int argc, char **argv)
{
    if (def->num_params <= 0 || !def->param_names)
        return arena_strdup(&pp->arena, def->body);

    StrBuf result;
    strbuf_init(&result);
    const char *body = def->body;
    int i = 0;

    while (body[i]) {
        /* Check for parameter name */
        int id_len = scan_id(&body[i]);
        if (id_len > 0) {
            /* Check if it matches a parameter */
            bool found = false;
            for (int p = 0; p < def->num_params; p++) {
                if ((int)strlen(def->param_names[p]) == id_len &&
                    strncmp(&body[i], def->param_names[p], (size_t)id_len) == 0) {
                    /* Substitute with argument value */
                    if (p < argc && argv[p])
                        strbuf_append(&result, argv[p]);
                    found = true;
                    break;
                }
            }
            if (!found) {
                strbuf_append_n(&result, &body[i], (size_t)id_len);
            }
            i += id_len;
        } else {
            strbuf_append_char(&result, body[i]);
            i++;
        }
    }

    char *r = arena_strdup(&pp->arena, strbuf_cstr(&result));
    strbuf_free(&result);
    return r;
}

char *preproc_expand_macro(PreprocState *pp, const char *name,
                           int argc, char **argv)
{
    MacroDef *def = hashmap_get(&pp->macros, name);
    if (!def) return NULL;

    /* Recursion guard */
    if (def->evaluating)
        return arena_strdup(&pp->arena, name);

    /* Builtin? */
    if (def->is_builtin) {
        char *result = expand_builtin(pp, name);
        if (result) return result;
    }

    def->evaluating = true;
    char *expanded;

    if (def->num_params >= 0) {
        /* Function-like macro — substitute params */
        expanded = substitute_params(pp, def, argc, argv);
    } else {
        /* Object-like macro */
        expanded = arena_strdup(&pp->arena, def->body);
    }

    /* Recursively expand macros in the result */
    char *final = expand_macros_in_text(pp, expanded);
    def->evaluating = false;
    return final;
}

/* Expand all macros in a text string */
static char *expand_macros_in_text(PreprocState *pp, const char *text)
{
    if (!text || !*text) return arena_strdup(&pp->arena, "");

    StrBuf result;
    strbuf_init(&result);
    int i = 0;
    int len = (int)strlen(text);

    while (i < len) {
        /* Inside a string literal? Pass through */
        if (text[i] == '"') {
            strbuf_append_char(&result, text[i]);
            i++;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len) {
                    strbuf_append_char(&result, text[i]);
                    i++;
                }
                strbuf_append_char(&result, text[i]);
                i++;
            }
            if (i < len) {
                strbuf_append_char(&result, text[i]); /* closing quote */
                i++;
            }
            continue;
        }

        /* Single-line comment? Stop expanding */
        if (text[i] == '\'' || (text[i] == 'R' && i + 2 < len &&
            text[i+1] == 'E' && text[i+2] == 'M' &&
            (i + 3 >= len || !is_id_char(text[i+3])))) {
            /* Copy rest as-is */
            strbuf_append(&result, &text[i]);
            break;
        }

        /* Identifier? Check for macro */
        int id_len = scan_id(&text[i]);
        if (id_len > 0) {
            char *id = arena_strndup(&pp->arena, &text[i], (size_t)id_len);
            MacroDef *def = hashmap_get(&pp->macros, id);

            if (def && !def->evaluating) {
                i += id_len;

                if (def->num_params >= 0) {
                    /* Function-like: need parentheses */
                    const char *after = skip_ws(&text[i]);
                    int skip = (int)(after - &text[i]);

                    if (after[0] == '(') {
                        i += skip;
                        char *argv[64];
                        int end_pos;
                        int argc = parse_macro_args(text, i, argv, 64, &end_pos, &pp->arena);
                        i = end_pos;

                        char *expanded = preproc_expand_macro(pp, id, argc, argv);
                        if (expanded)
                            strbuf_append(&result, expanded);
                    } else {
                        /* No parens — not a macro call, output as-is */
                        strbuf_append(&result, id);
                    }
                } else {
                    /* Object-like macro */
                    char *expanded = preproc_expand_macro(pp, id, 0, NULL);
                    if (expanded)
                        strbuf_append(&result, expanded);
                }
            } else {
                strbuf_append_n(&result, &text[i], (size_t)id_len);
                i += id_len;
            }
        } else {
            strbuf_append_char(&result, text[i]);
            i++;
        }
    }

    char *r = arena_strdup(&pp->arena, strbuf_cstr(&result));
    strbuf_free(&result);
    return r;
}

/* ----------------------------------------------------------------
 * Directive parsing
 * ---------------------------------------------------------------- */

/* Check if the current ifdef stack means we should process code */
static bool is_enabled(PreprocState *pp)
{
    for (int i = 0; i < pp->ifdef_stack.len; i++) {
        if (!pp->ifdef_stack.data[i].enabled)
            return false;
    }
    return true;
}

/* Handle: #define NAME [BODY] or #define NAME(params) BODY */
static void handle_define(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);

    /* Parse macro name */
    int name_len = scan_id(p);
    if (name_len == 0) {
        preproc_error(pp, "expected identifier after #define");
        return;
    }
    char *name = arena_strndup(&pp->arena, p, (size_t)name_len);
    p += name_len;

    /* Check for function-like macro: name immediately followed by '(' */
    if (*p == '(') {
        p++; /* skip '(' */
        /* Parse parameter list */
        char *params[64];
        int num_params = 0;

        while (*p && *p != ')') {
            p = skip_ws(p);
            if (*p == ')') break;

            int plen = scan_id(p);
            if (plen == 0) {
                preproc_error(pp, "expected parameter name in #define");
                return;
            }

            /* Check for duplicate parameter names */
            char *pname = arena_strndup(&pp->arena, p, (size_t)plen);
            for (int i = 0; i < num_params; i++) {
                if (strcmp(params[i], pname) == 0) {
                    preproc_error(pp, "Duplicated name parameter \"%s\"", pname);
                    return;
                }
            }

            params[num_params++] = pname;
            p += plen;
            p = skip_ws(p);
            if (*p == ',') p++;
        }
        if (*p == ')') p++;

        /* Body is the rest after optional whitespace */
        /* Check for missing whitespace */
        if (*p && *p != ' ' && *p != '\t' && *p != '\'' && *p != '\n' && *p != '\r' && *p != '\0') {
            preproc_warning(pp, 520, "missing whitespace after macro name");
        }
        p = skip_ws(p);

        /* Parse body: handle comments and continuation lines.
         * In BASIC, ' starts a comment. If a continuation (\) was in a
         * comment, it was replaced with \n during line joining. The body
         * continues on the next "line" (after the \n). */
        StrBuf body;
        strbuf_init(&body);
        while (*p) {
            if (*p == '\'') {
                /* Comment — skip until \n or end of string */
                p++;
                while (*p && *p != '\n') p++;
                /* If \n, body continues on next continuation line */
                if (*p == '\n') {
                    strbuf_append_char(&body, '\n');
                    p++;
                }
            } else if (*p == '\n') {
                /* Continuation line boundary without comment */
                strbuf_append_char(&body, '\n');
                p++;
            } else {
                strbuf_append_char(&body, *p);
                p++;
            }
        }

        /* Trim trailing whitespace from body */
        char *body_str = strbuf_detach(&body);
        strip_trailing(body_str);

        preproc_define_func(pp, name, body_str, num_params, params,
                           pp->current_line, pp->current_file);
        free(body_str);
    } else {
        /* Object-like macro */
        /* Check for missing whitespace */
        if (*p && *p != ' ' && *p != '\t' && *p != '\'' && *p != '\n' && *p != '\r' && *p != '\0') {
            preproc_warning(pp, 520, "missing whitespace after macro name");
        }
        p = skip_ws(p);

        /* Parse body with comment/continuation handling */
        StrBuf body;
        strbuf_init(&body);
        while (*p) {
            if (*p == '\'') {
                /* Comment — skip until \n or end of string */
                p++;
                while (*p && *p != '\n') p++;
                if (*p == '\n') {
                    strbuf_append_char(&body, '\n');
                    p++;
                }
            } else if (*p == '\n') {
                strbuf_append_char(&body, '\n');
                p++;
            } else {
                strbuf_append_char(&body, *p);
                p++;
            }
        }

        char *body_str = strbuf_detach(&body);
        strip_trailing(body_str);

        preproc_define(pp, name, body_str, pp->current_line, pp->current_file);

        free(body_str);
    }
}

/* Handle: #undef NAME */
static void handle_undef(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    int name_len = scan_id(p);
    if (name_len == 0) {
        preproc_error(pp, "expected identifier after #undef");
        return;
    }
    char *name = arena_strndup(&pp->arena, p, (size_t)name_len);
    preproc_undef(pp, name);
}

/* Handle: #ifdef NAME / #ifndef NAME */
static void handle_ifdef(PreprocState *pp, const char *rest, bool is_ifndef)
{
    const char *p = skip_ws(rest);
    int name_len = scan_id(p);
    if (name_len == 0) {
        preproc_error(pp, "expected identifier after #ifdef/#ifndef");
        return;
    }
    char *name = arena_strndup(&pp->arena, p, (size_t)name_len);
    bool defined = preproc_is_defined(pp, name);
    bool enabled = is_ifndef ? !defined : defined;

    /* If parent is disabled, this branch is also disabled */
    if (!is_enabled(pp))
        enabled = false;

    IfDefEntry entry = { .enabled = enabled, .else_seen = false,
                         .line = pp->current_line };
    vec_push(pp->ifdef_stack, entry);
}

/* Handle: #if EXPR */
static void handle_if(PreprocState *pp, const char *rest)
{
    /* Simple expression evaluation: currently just check if it's
     * a defined macro name or a numeric literal */
    const char *p = skip_ws(rest);
    bool result = false;

    /* Try to evaluate as a simple expression */
    int id_len = scan_id(p);
    if (id_len > 0) {
        char *id = arena_strndup(&pp->arena, p, (size_t)id_len);
        /* Check if it's a macro and expand it */
        if (preproc_is_defined(pp, id)) {
            char *val = preproc_expand_macro(pp, id, 0, NULL);
            if (val) {
                /* Try to parse as number */
                char *end;
                long num = strtol(val, &end, 10);
                if (end != val)
                    result = (num != 0);
                else
                    result = (strlen(val) > 0);
            }
        }
    } else {
        /* Try as number */
        char *end;
        long num = strtol(p, &end, 10);
        if (end != p)
            result = (num != 0);
    }

    if (!is_enabled(pp))
        result = false;

    IfDefEntry entry = { .enabled = result, .else_seen = false,
                         .line = pp->current_line };
    vec_push(pp->ifdef_stack, entry);
}

/* Handle: #else */
static void handle_else(PreprocState *pp)
{
    if (pp->ifdef_stack.len == 0) {
        preproc_error(pp, "#else without matching #ifdef");
        return;
    }
    IfDefEntry *top = &pp->ifdef_stack.data[pp->ifdef_stack.len - 1];
    if (top->else_seen) {
        preproc_error(pp, "duplicate #else");
        return;
    }
    top->else_seen = true;

    /* Check if parent is enabled */
    bool parent_enabled = true;
    for (int i = 0; i < pp->ifdef_stack.len - 1; i++) {
        if (!pp->ifdef_stack.data[i].enabled) {
            parent_enabled = false;
            break;
        }
    }

    if (parent_enabled)
        top->enabled = !top->enabled;
}

/* Handle: #endif */
static void handle_endif(PreprocState *pp)
{
    if (pp->ifdef_stack.len == 0) {
        preproc_error(pp, "#endif without matching #ifdef");
        return;
    }
    pp->ifdef_stack.len--;
}

/* Handle: #include "file" or #include <file> */
static void handle_include(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    bool is_system = false;
    char filename[PATH_MAX];

    /* Check for optional [arch:XXX] modifier */
    if (*p == '[') {
        /* TODO: architecture-specific includes */
        while (*p && *p != ']') p++;
        if (*p == ']') p++;
        p = skip_ws(p);
    }

    /* Check for ONCE keyword */
    bool once = false;
    if (strnicmp_local(p, "once", 4) == 0 && !is_id_char(p[4])) {
        once = true;
        p += 4;
        p = skip_ws(p);
    }

    if (*p == '<') {
        is_system = true;
        p++;
        int i = 0;
        while (*p && *p != '>' && i < (int)sizeof(filename) - 1)
            filename[i++] = *p++;
        filename[i] = '\0';
        if (*p == '>') p++;
    } else if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < (int)sizeof(filename) - 1)
            filename[i++] = *p++;
        filename[i] = '\0';
        if (*p == '"') p++;
    } else {
        /* Could be a macro that expands to a filename */
        preproc_error(pp, "expected filename after #include");
        return;
    }

    /* Resolve path */
    char *resolved = resolve_include(pp, filename, is_system);
    if (!resolved) {
        preproc_error(pp, "cannot find include file \"%s\"", filename);
        return;
    }

    /* Check for #pragma once / include once */
    IncludeInfo *inc_info = hashmap_get(&pp->included, resolved);
    if (inc_info && (inc_info->once || once)) {
        return; /* already included with once */
    }

    /* Track this include */
    if (!inc_info) {
        inc_info = arena_calloc(&pp->arena, 1, sizeof(IncludeInfo));
        inc_info->once = once;
        hashmap_set(&pp->included, resolved, inc_info);
    }

    /* Read the file */
    char *content = read_file(resolved);
    if (!content) {
        preproc_error(pp, "cannot read include file \"%s\"", resolved);
        return;
    }

    /* Save current file state */
    FileStackEntry entry;
    entry.filename = pp->current_file;
    entry.lineno = pp->current_line;
    entry.content = NULL;
    entry.pos = 0;
    vec_push(pp->file_stack, entry);

    /* Process included file */
    char *saved_file = pp->current_file;
    int saved_line = pp->current_line;

    pp->current_file = arena_strdup(&pp->arena, resolved);
    pp->current_line = 1;
    preproc_emit_line(pp, 1, pp->current_file);

    /* Process the included content line by line */
    char *line_start = content;
    while (*line_start) {
        char *line_end = strchr(line_start, '\n');
        if (line_end) {
            size_t line_len = (size_t)(line_end - line_start);
            char *line = arena_strndup(&pp->arena, line_start, line_len);
            process_line(pp, line);
            pp->current_line++;
            line_start = line_end + 1;
        } else {
            /* Last line without newline */
            process_line(pp, line_start);
            break;
        }
    }

    free(content);

    /* Emit closing #line for the included file */
    preproc_emit_line(pp, pp->current_line, pp->current_file);

    /* Restore file state */
    pp->file_stack.len--;
    pp->current_file = saved_file;
    pp->current_line = saved_line;
    preproc_emit_line(pp, pp->current_line + 1, pp->current_file);
}

/* Handle: #line NUMBER ["FILENAME"] */
static void handle_line_directive(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    char *end;
    long num = strtol(p, &end, 10);
    if (end == p) {
        preproc_error(pp, "expected line number after #line");
        return;
    }
    pp->current_line = (int)num - 1; /* -1 because it will be incremented */

    p = skip_ws(end);
    if (*p == '"') {
        p++;
        StrBuf fname;
        strbuf_init(&fname);
        while (*p && *p != '"') {
            strbuf_append_char(&fname, *p);
            p++;
        }
        pp->current_file = arena_strdup(&pp->arena, strbuf_cstr(&fname));
        strbuf_free(&fname);
    }
}

/* Handle: #pragma ...
 * Returns true if this was #pragma once (no text output needed). */
static bool handle_pragma(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);

    /* #pragma once */
    if (strnicmp_local(p, "once", 4) == 0 && !is_id_char(p[4])) {
        if (pp->current_file) {
            IncludeInfo *info = hashmap_get(&pp->included, pp->current_file);
            if (!info) {
                info = arena_calloc(&pp->arena, 1, sizeof(IncludeInfo));
                hashmap_set(&pp->included, pp->current_file, info);
            }
            info->once = true;
        }
        return true;
    }

    /* Pass other pragmas through to output */
    strbuf_printf(&pp->output, "#pragma %s\n", rest);
    return false;
}

/* Handle: #require "file" */
static void handle_require(PreprocState *pp, const char *rest)
{
    /* Pass through to output */
    strbuf_printf(&pp->output, "#require %s\n", rest);
}

/* Handle: #init "name" */
static void handle_init(PreprocState *pp, const char *rest)
{
    /* Pass through to output */
    strbuf_printf(&pp->output, "#init %s\n", rest);
}

/* Handle: #error MESSAGE */
static void handle_error_directive(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    preproc_error(pp, "%s", p);
}

/* Handle: #warning MESSAGE */
static void handle_warning_directive(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    preproc_warning(pp, 0, "%s", p);
}

/* Process a preprocessor directive line.
 *
 * Output model (matching Python PLY grammar):
 * - #ifdef (condition true): emit "\n" (blank line for the directive)
 * - #ifdef (condition false): no output
 * - #else (false→true): emit "#line N+1 file\n"
 * - #else (true→false): no output
 * - #endif: emit "#line N+1 file\n" (when parent was enabled)
 * - #define (first in file): emit "\n"
 * - #define (subsequent): emit "#line N+1 file\n"
 * - #undef, #error, #warning, #pragma once: emit "\n"
 * - #pragma other, #require, #init, #line: emit directive text
 * - #include: handled specially
 */
static void process_directive(PreprocState *pp, const char *directive)
{
    const char *p = skip_ws(directive);

    /* Skip '#' and optional whitespace */
    if (*p == '#') p++;
    p = skip_ws(p);

    /* Get directive name */
    int dlen = scan_id(p);
    if (dlen == 0) return;

    const char *rest = p + dlen;

    /* === Conditional directives (always processed) === */

    if (strnicmp_local(p, "ifdef", (size_t)dlen) == 0 && dlen == 5) {
        bool was_enabled = is_enabled(pp);
        handle_ifdef(pp, rest, false);
        bool now_enabled = is_enabled(pp);
        /* Emit blank line only when condition is true (both before and after enabled) */
        if (was_enabled && now_enabled) {
            strbuf_append_char(&pp->output, '\n');
            pp->has_output = true;
        }
        return;
    }
    if (strnicmp_local(p, "ifndef", (size_t)dlen) == 0 && dlen == 6) {
        bool was_enabled = is_enabled(pp);
        handle_ifdef(pp, rest, true);
        bool now_enabled = is_enabled(pp);
        if (was_enabled && now_enabled) {
            strbuf_append_char(&pp->output, '\n');
            pp->has_output = true;
        }
        return;
    }
    if (strnicmp_local(p, "if", (size_t)dlen) == 0 && dlen == 2) {
        bool was_enabled = is_enabled(pp);
        handle_if(pp, rest);
        bool now_enabled = is_enabled(pp);
        if (was_enabled && now_enabled) {
            strbuf_append_char(&pp->output, '\n');
            pp->has_output = true;
        }
        return;
    }
    if (strnicmp_local(p, "else", (size_t)dlen) == 0 && dlen == 4) {
        bool was_enabled = is_enabled(pp);
        handle_else(pp);
        bool now_enabled = is_enabled(pp);
        /* Transitioning from disabled to enabled: emit #line */
        if (!was_enabled && now_enabled) {
            strbuf_printf(&pp->output, "#line %d \"%s\"\n",
                         pp->current_line + 1, pp->current_file);
            pp->has_output = true;
        }
        return;
    }
    if (strnicmp_local(p, "endif", (size_t)dlen) == 0 && dlen == 5) {
        /* Check if parent (after pop) will be enabled */
        bool parent_enabled = true;
        for (int i = 0; i < pp->ifdef_stack.len - 1; i++) {
            if (!pp->ifdef_stack.data[i].enabled) {
                parent_enabled = false;
                break;
            }
        }
        handle_endif(pp);
        /* Emit #line when returning to an enabled state */
        if (parent_enabled) {
            strbuf_printf(&pp->output, "#line %d \"%s\"\n",
                         pp->current_line + 1, pp->current_file);
            pp->has_output = true;
        }
        return;
    }

    /* === Other directives (only when enabled) === */
    if (!is_enabled(pp)) return;

    if (strnicmp_local(p, "define", (size_t)dlen) == 0 && dlen == 6) {
        handle_define(pp, rest);
        /* First define in file: blank line. Subsequent: #line directive. */
        if (!pp->has_output) {
            strbuf_append_char(&pp->output, '\n');
        } else {
            strbuf_printf(&pp->output, "#line %d \"%s\"\n",
                         pp->current_line + 1, pp->current_file);
        }
        pp->has_output = true;
    } else if (strnicmp_local(p, "undef", (size_t)dlen) == 0 && dlen == 5) {
        handle_undef(pp, rest);
        strbuf_append_char(&pp->output, '\n');
        pp->has_output = true;
    } else if (strnicmp_local(p, "include", (size_t)dlen) == 0 && dlen == 7) {
        handle_include(pp, rest);
        pp->has_output = true;
    } else if (strnicmp_local(p, "line", (size_t)dlen) == 0 && dlen == 4) {
        handle_line_directive(pp, rest);
        /* #line directives pass through — reconstruct cleanly */
        const char *lr = skip_ws(rest);
        strbuf_printf(&pp->output, "#line %s\n", lr);
        pp->has_output = true;
    } else if (strnicmp_local(p, "pragma", (size_t)dlen) == 0 && dlen == 6) {
        bool was_once = handle_pragma(pp, rest);
        if (was_once) {
            /* #pragma once produces blank line (like #undef) */
            strbuf_append_char(&pp->output, '\n');
        }
        pp->has_output = true;
    } else if (strnicmp_local(p, "require", (size_t)dlen) == 0 && dlen == 7) {
        handle_require(pp, rest);
        pp->has_output = true;
    } else if (strnicmp_local(p, "init", (size_t)dlen) == 0 && dlen == 4) {
        handle_init(pp, rest);
        pp->has_output = true;
    } else if (strnicmp_local(p, "error", (size_t)dlen) == 0 && dlen == 5) {
        handle_error_directive(pp, rest);
        strbuf_append_char(&pp->output, '\n');
        pp->has_output = true;
    } else if (strnicmp_local(p, "warning", (size_t)dlen) == 0 && dlen == 7) {
        handle_warning_directive(pp, rest);
        strbuf_append_char(&pp->output, '\n');
        pp->has_output = true;
    } else {
        char *dname = arena_strndup(&pp->arena, p, (size_t)dlen);
        preproc_error(pp, "invalid directive #%s", dname);
    }
}

/* ----------------------------------------------------------------
 * BASIC comment stripping
 * ---------------------------------------------------------------- */

/* Find the start of a comment in a line.
 * In BASIC mode: ' or REM starts a comment.
 * In ASM mode: ; starts a comment, but ' is a valid token (e.g. af').
 * Returns pointer to start of comment, or NULL if none found. */
static const char *find_comment(const char *line, bool in_asm)
{
    const char *p = line;
    bool in_string = false;

    while (*p) {
        if (*p == '"') {
            in_string = !in_string;
        } else if (!in_string) {
            if (in_asm) {
                /* ASM mode: ; starts comment */
                if (*p == ';') return p;
            } else {
                /* BASIC mode: ' starts comment */
                if (*p == '\'') return p;
                /* REM keyword */
                if ((p[0] == 'R' || p[0] == 'r') &&
                    (p[1] == 'E' || p[1] == 'e') &&
                    (p[2] == 'M' || p[2] == 'm') &&
                    (p == line || !is_id_char(p[-1])) &&
                    !is_id_char(p[3])) {
                    return p;
                }
            }
        }
        p++;
    }
    return NULL;
}

/* Check if a line is entirely a comment (only whitespace before comment start) */
static bool is_comment_only_line(const char *line, bool in_asm)
{
    const char *p = skip_ws(line);
    if (in_asm) {
        return *p == ';';
    }
    if (*p == '\'') return true;
    if ((p[0] == 'R' || p[0] == 'r') &&
        (p[1] == 'E' || p[1] == 'e') &&
        (p[2] == 'M' || p[2] == 'm') &&
        !is_id_char(p[3])) {
        return true;
    }
    return false;
}

/* Strip trailing comment from a line, returning a new arena-allocated string.
 * Also strips trailing whitespace after removing the comment. */
static char *strip_comment(PreprocState *pp, const char *line)
{
    const char *comment = find_comment(line, pp->in_asm);
    if (!comment) return NULL; /* no comment found */

    /* Copy everything before the comment */
    size_t len = (size_t)(comment - line);
    char *result = arena_strndup(&pp->arena, line, len);
    /* Strip trailing whitespace */
    int rlen = (int)len;
    while (rlen > 0 && (result[rlen-1] == ' ' || result[rlen-1] == '\t'))
        rlen--;
    result[rlen] = '\0';
    return result;
}

/* Check if line starts/ends ASM mode. Updates pp->in_asm.
 * Returns true if this line is an asm/end asm boundary. */
static void check_asm_boundary(PreprocState *pp, const char *line)
{
    const char *p = skip_ws(line);

    if (pp->in_asm) {
        /* Check for "end asm" (case-insensitive) */
        if (strnicmp_local(p, "end", 3) == 0 && !is_id_char(p[3])) {
            const char *after = skip_ws(p + 3);
            if (strnicmp_local(after, "asm", 3) == 0 && !is_id_char(after[3])) {
                pp->in_asm = false;
            }
        }
    } else {
        /* Check for "asm" (case-insensitive), not "end asm" */
        if (strnicmp_local(p, "asm", 3) == 0 && !is_id_char(p[3])) {
            pp->in_asm = true;
        }
    }
}

/* ----------------------------------------------------------------
 * Line processing
 * ---------------------------------------------------------------- */

/* Check if a line starts with a preprocessor directive */
static bool is_directive(const char *line)
{
    const char *p = skip_ws(line);
    return *p == '#';
}

/* Process a single line of input.
 *
 * The output model matches the Python PLY grammar:
 * - Directives handle their own output in process_directive()
 * - Enabled content lines: expand macros, emit text + "\n"
 * - Enabled comment/empty lines: emit "\n" (blank line)
 * - Disabled lines: no output at all
 */
static void process_line(PreprocState *pp, const char *line)
{
    if (is_directive(line)) {
        process_directive(pp, line);
        return;
    }

    if (!is_enabled(pp)) return;

    /* Track ASM mode transitions */
    check_asm_boundary(pp, line);

    /* Comment-only lines → blank line */
    if (is_comment_only_line(line, pp->in_asm)) {
        strbuf_append_char(&pp->output, '\n');
        pp->has_output = true;
        return;
    }

    /* Empty lines → blank line */
    const char *trimmed = skip_ws(line);
    if (*trimmed == '\0') {
        strbuf_append_char(&pp->output, '\n');
        pp->has_output = true;
        return;
    }

    /* Strip inline comments from the line before macro expansion */
    char *stripped = strip_comment(pp, line);
    const char *to_expand = stripped ? stripped : line;

    /* Regular content line — expand macros and emit */
    char *expanded = expand_macros_in_text(pp, to_expand);
    strbuf_append(&pp->output, expanded);
    /* If expansion contains newlines (from continuation macros),
     * add a #line directive to resync after the multi-line output */
    if (strchr(expanded, '\n') != NULL) {
        strbuf_printf(&pp->output, "\n#line %d", pp->current_line + 1);
    }
    strbuf_append_char(&pp->output, '\n');
    pp->has_output = true;
}

/* ----------------------------------------------------------------
 * Public API: process a file
 * ---------------------------------------------------------------- */

int preproc_file(PreprocState *pp, const char *filename)
{
    char *content = read_file(filename);
    if (!content) {
        fprintf(pp->err_file, "error: cannot open file \"%s\"\n", filename);
        return 1;
    }

    /* Register builtins on first call */
    static bool builtins_done = false;
    if (!builtins_done) {
        register_builtins(pp);
        builtins_done = true;
    }

    pp->current_file = arena_strdup(&pp->arena, filename);
    pp->current_line = 1;

    /* Emit initial #line — this sets the baseline for line tracking.
     * Don't set has_output here; that flag tracks grammar-level output
     * (i.e., first define produces "\n", subsequent produces "#line"). */
    preproc_emit_line(pp, 1, pp->current_file);

    /* Process line by line */
    char *line_start = content;
    StrBuf linebuf;
    strbuf_init(&linebuf);

    while (*line_start) {
        char *line_end = strchr(line_start, '\n');
        size_t line_len;
        if (line_end) {
            line_len = (size_t)(line_end - line_start);
        } else {
            line_len = strlen(line_start);
        }

        /* Handle line continuation (backslash at end, or underscore for BASIC) */
        strbuf_append_n(&linebuf, line_start, line_len);

        /* Check for line continuation */
        const char *cur = strbuf_cstr(&linebuf);
        int curlen = (int)strbuf_cstr(&linebuf)[0] ? (int)strlen(strbuf_cstr(&linebuf)) : 0;
        bool continued = false;

        if (curlen > 0) {
            /* Backslash continuation (for #define) */
            char last = cur[curlen - 1];
            if (last == '\\') {
                continued = true;
                /* Replace backslash with newline to preserve line structure */
                linebuf.data[linebuf.len - 1] = '\n';
            }
        }

        if (!continued) {
            /* Process the complete line */
            char *complete_line = arena_strdup(&pp->arena, strbuf_cstr(&linebuf));
            /* Remove trailing \r */
            int clen = (int)strlen(complete_line);
            if (clen > 0 && complete_line[clen-1] == '\r')
                complete_line[clen-1] = '\0';

            process_line(pp, complete_line);
            strbuf_clear(&linebuf);
            pp->current_line++;
        } else {
            pp->current_line++;
        }

        if (line_end)
            line_start = line_end + 1;
        else
            break;
    }

    strbuf_free(&linebuf);
    free(content);
    return pp->error_count;
}

int preproc_string(PreprocState *pp, const char *input, const char *filename)
{
    /* Register builtins */
    static bool builtins_done2 = false;
    if (!builtins_done2) {
        register_builtins(pp);
        builtins_done2 = true;
    }

    pp->current_file = arena_strdup(&pp->arena, filename ? filename : "<string>");
    pp->current_line = 1;
    preproc_emit_line(pp, 1, pp->current_file);

    /* Make a mutable copy */
    char *copy = strdup(input);
    char *line_start = copy;

    while (*line_start) {
        char *line_end = strchr(line_start, '\n');
        if (line_end) {
            *line_end = '\0';
            /* Remove trailing \r */
            if (line_end > line_start && line_end[-1] == '\r')
                line_end[-1] = '\0';
            process_line(pp, line_start);
            pp->current_line++;
            line_start = line_end + 1;
        } else {
            int slen = (int)strlen(line_start);
            if (slen > 0 && line_start[slen-1] == '\r')
                line_start[slen-1] = '\0';
            process_line(pp, line_start);
            break;
        }
    }

    free(copy);
    return pp->error_count;
}
