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
#include "compat.h"
#include "cwalk.h"

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
    pp->block_comment_level = 0;
    pp->builtins_registered = false;
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
        const char *base_ptr;
        size_t base_len;
        cwk_path_get_basename(pp->current_file, &base_ptr, &base_len);
        if (!base_ptr) { base_ptr = pp->current_file; base_len = strlen(pp->current_file); }
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_printf(&sb, "\"%.*s\"", (int)base_len, base_ptr);
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

    /* Skip UTF-8 BOM if present */
    if (nread >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, nread - 3 + 1);
    }

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
        size_t dir_len;
        cwk_path_get_dirname(pp->current_file, &dir_len);
        /* dir_len includes trailing separator; if 0, use "." */
        if (dir_len > 0) {
            /* Strip trailing separator for snprintf */
            size_t d = dir_len;
            if (d > 1 && (pp->current_file[d-1] == '/' || pp->current_file[d-1] == '\\'))
                d--;
            snprintf(path, sizeof(path), "%.*s/%s", (int)d, pp->current_file, name);
        } else {
            snprintf(path, sizeof(path), "./%s", name);
        }
        if (access(path, R_OK) == 0) {
            /* Normalize: strip leading "./" */
            const char *normalized = path;
            if (normalized[0] == '.' && normalized[1] == '/')
                normalized += 2;
            return arena_strdup(&pp->arena, normalized);
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

/* Stringize an argument: wrap in quotes, doubling any internal quotes */
static char *stringize_arg(PreprocState *pp, const char *arg)
{
    StrBuf sb;
    strbuf_init(&sb);
    strbuf_append_char(&sb, '"');
    for (const char *p = arg; *p; p++) {
        if (*p == '"')
            strbuf_append_char(&sb, '"'); /* double the quote */
        strbuf_append_char(&sb, *p);
    }
    strbuf_append_char(&sb, '"');
    char *r = arena_strdup(&pp->arena, strbuf_cstr(&sb));
    strbuf_free(&sb);
    return r;
}

/* Find which parameter index matches an identifier, or -1 */
static int find_param(const MacroDef *def, const char *id, int id_len)
{
    for (int p = 0; p < def->num_params; p++) {
        if ((int)strlen(def->param_names[p]) == id_len &&
            strncmp(id, def->param_names[p], (size_t)id_len) == 0)
            return p;
    }
    return -1;
}

/* Substitute parameters in macro body, handling ## (token paste) and # (stringize) */
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
        /* Check for stringize operator: # followed by parameter name */
        if (body[i] == '#' && body[i+1] != '#') {
            i++; /* skip # */
            /* Skip whitespace after # */
            while (body[i] == ' ' || body[i] == '\t') i++;
            int id_len = scan_id(&body[i]);
            if (id_len > 0) {
                int pidx = find_param(def, &body[i], id_len);
                if (pidx >= 0 && pidx < argc && argv[pidx]) {
                    char *s = stringize_arg(pp, argv[pidx]);
                    strbuf_append(&result, s);
                } else {
                    /* Not a parameter — output # and the identifier */
                    strbuf_append_char(&result, '#');
                    strbuf_append_n(&result, &body[i], (size_t)id_len);
                }
                i += id_len;
            } else {
                strbuf_append_char(&result, '#');
            }
            continue;
        }

        /* Check for parameter name or token paste */
        int id_len = scan_id(&body[i]);
        if (id_len > 0) {
            int pidx = find_param(def, &body[i], id_len);
            const char *value;
            if (pidx >= 0 && pidx < argc && argv[pidx])
                value = argv[pidx];
            else if (pidx >= 0)
                value = "";
            else
                value = NULL;

            if (value) {
                strbuf_append(&result, value);
            } else {
                strbuf_append_n(&result, &body[i], (size_t)id_len);
            }
            i += id_len;

            /* Check for ## (token paste) after this token */
            const char *after = &body[i];
            while (*after == ' ' || *after == '\t') after++;
            if (after[0] == '#' && after[1] == '#') {
                /* Token paste: strip trailing whitespace from result,
                 * skip ## and leading whitespace, paste next token */
                while (result.len > 0 && (result.data[result.len-1] == ' ' ||
                       result.data[result.len-1] == '\t'))
                    result.len--;
                i = (int)(after - body) + 2; /* skip ## */
                while (body[i] == ' ' || body[i] == '\t') i++;
                /* The next token will be appended directly (pasted) */
            }
        } else if (body[i] == '#' && body[i+1] == '#') {
            /* ## not preceded by identifier — strip trailing ws from result */
            while (result.len > 0 && (result.data[result.len-1] == ' ' ||
                   result.data[result.len-1] == '\t'))
                result.len--;
            i += 2; /* skip ## */
            while (body[i] == ' ' || body[i] == '\t') i++;
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
        /* Function-like macro — check argument count */
        if (argc != def->num_params) {
            preproc_error(pp, "Macro \"%s\" expected %d params, got %d",
                          name, def->num_params, argc);
            def->evaluating = false;
            return arena_strdup(&pp->arena, "");
        }
        /* Function-like macro — substitute params */
        expanded = substitute_params(pp, def, argc, argv);
    } else {
        /* Object-like macro */
        expanded = arena_strdup(&pp->arena, def->body);
    }

    /* Recursively expand macros in the result */
    char *final = expand_macros_in_text(pp, expanded);
    def->evaluating = false;

    /* Python's expand_macros() calls remove_spaces() on each MacroCall result:
     * strip leading/trailing whitespace, return " " if all-whitespace. */
    if (final) {
        const char *start = final;
        while (*start == ' ' || *start == '\t') start++;
        if (!*start) {
            /* All whitespace → single space */
            return arena_strdup(&pp->arena, " ");
        }
        const char *end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (start != final || end != start + strlen(start)) {
            final = arena_strndup(&pp->arena, start, (size_t)(end - start));
        }
    }
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
        /* Inside a string literal? Pass through.
         * BASIC escapes quotes by doubling (""), not backslash (\"). */
        if (text[i] == '"') {
            strbuf_append_char(&result, text[i]);
            i++;
            while (i < len) {
                if (text[i] == '"') {
                    if (i + 1 < len && text[i + 1] == '"') {
                        /* Doubled quote "" — escaped, still inside string */
                        strbuf_append_char(&result, text[i]);
                        strbuf_append_char(&result, text[i + 1]);
                        i += 2;
                    } else {
                        /* Closing quote */
                        break;
                    }
                } else {
                    strbuf_append_char(&result, text[i]);
                    i++;
                }
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

                        /* Expand macros in each argument before substitution */
                        for (int a = 0; a < argc && a < 64; a++) {
                            if (argv[a])
                                argv[a] = expand_macros_in_text(pp, argv[a]);
                        }

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
                    if (expanded) {
                        /* If the expanded result is an identifier followed by
                         * '(' in the remaining text, we need to rescan so the
                         * function-like macro can be called with the args.
                         * Build new text = expanded + remaining and re-expand. */
                        int exp_id = scan_id(expanded);
                        const char *after_exp = skip_ws(&text[i]);
                        if (exp_id > 0 && (size_t)exp_id == strlen(expanded) &&
                            *after_exp == '(') {
                            /* Rescan: prepend expanded to remaining text */
                            StrBuf rescan;
                            strbuf_init(&rescan);
                            strbuf_append(&rescan, expanded);
                            strbuf_append(&rescan, &text[i]);
                            char *rescanned = expand_macros_in_text(pp,
                                                strbuf_cstr(&rescan));
                            strbuf_free(&rescan);
                            strbuf_append(&result, rescanned);
                            i = len; /* consumed everything */
                        } else {
                            strbuf_append(&result, expanded);
                        }
                    }
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

        p = skip_ws(p);
        if (*p == ')') {
            /* Empty parens: #define foo() — Python creates one epsilon param */
            params[0] = arena_strdup(&pp->arena, "");
            num_params = 1;
            p++;
        } else {
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
        }

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

/* ----------------------------------------------------------------
 * #if expression evaluator
 *
 * Grammar (matching Python PLY precedence):
 *   expr = primary ( (== | != | < | <= | > | >=) primary )*
 *   primary = atom ( (&& | ||) atom )*    [lower precedence]
 *
 * Actually the Python grammar has:
 *   expr : macrocall | NUMBER | STRING | expr AND expr | expr OR expr
 *        | expr EQ expr | expr NE expr | expr LT expr | ...
 *        | LLP expr RRP
 *
 * We implement a recursive-descent parser with proper precedence:
 *   or_expr   = and_expr ( '||' and_expr )*
 *   and_expr  = cmp_expr ( '&&' cmp_expr )*
 *   cmp_expr  = atom ( ('==' | '!=' | '<' | '<=' | '>' | '>=') atom )?
 *   atom      = NUMBER | STRING | ID ['(' args ')'] | '(' or_expr ')'
 *
 * All values are strings. Comparison uses string equality.
 * Boolean result: "1" or "0".
 * ---------------------------------------------------------------- */

typedef struct IfExprParser {
    PreprocState *pp;
    const char *pos;
} IfExprParser;

static void if_skip_ws(IfExprParser *ep)
{
    while (*ep->pos == ' ' || *ep->pos == '\t') ep->pos++;
}

static char *if_parse_or(IfExprParser *ep);

static char *if_parse_atom(IfExprParser *ep)
{
    if_skip_ws(ep);

    /* Parenthesized expression */
    if (*ep->pos == '(') {
        ep->pos++;
        char *val = if_parse_or(ep);
        if_skip_ws(ep);
        if (*ep->pos == ')') ep->pos++;
        return val;
    }

    /* String literal — keep quotes to match Python behavior */
    if (*ep->pos == '"') {
        StrBuf sb;
        strbuf_init(&sb);
        strbuf_append_char(&sb, *ep->pos); /* opening quote */
        ep->pos++;
        while (*ep->pos && *ep->pos != '"') {
            strbuf_append_char(&sb, *ep->pos);
            ep->pos++;
        }
        if (*ep->pos == '"') {
            strbuf_append_char(&sb, *ep->pos); /* closing quote */
            ep->pos++;
        }
        char *r = arena_strdup(&ep->pp->arena, strbuf_cstr(&sb));
        strbuf_free(&sb);
        return r;
    }

    /* Number */
    if (*ep->pos >= '0' && *ep->pos <= '9') {
        const char *start = ep->pos;
        while (*ep->pos >= '0' && *ep->pos <= '9') ep->pos++;
        return arena_strndup(&ep->pp->arena, start, (size_t)(ep->pos - start));
    }

    /* Identifier (possibly macro call) */
    int id_len = scan_id(ep->pos);
    if (id_len > 0) {
        char *id = arena_strndup(&ep->pp->arena, ep->pos, (size_t)id_len);
        ep->pos += id_len;
        if_skip_ws(ep);

        /* Function-like macro call? */
        if (*ep->pos == '(') {
            /* Build the full call text and expand */
            StrBuf call;
            strbuf_init(&call);
            strbuf_append(&call, id);
            /* Capture everything from ( to matching ) */
            int depth = 0;
            do {
                if (*ep->pos == '(') depth++;
                else if (*ep->pos == ')') depth--;
                strbuf_append_char(&call, *ep->pos);
                ep->pos++;
            } while (*ep->pos && depth > 0);

            char *expanded = expand_macros_in_text(ep->pp, strbuf_cstr(&call));
            strbuf_free(&call);
            /* Trim whitespace */
            char *r = arena_strdup(&ep->pp->arena, expanded);
            strip_trailing(r);
            const char *tr = skip_ws(r);
            return arena_strdup(&ep->pp->arena, tr);
        }

        /* Object-like macro — expand it */
        char *expanded = expand_macros_in_text(ep->pp, id);
        char *r = arena_strdup(&ep->pp->arena, expanded);
        strip_trailing(r);
        const char *tr = skip_ws(r);
        return arena_strdup(&ep->pp->arena, tr);
    }

    return arena_strdup(&ep->pp->arena, "");
}

static long if_to_int(const char *s)
{
    if (!s || !*s) return 0;
    char *end;
    long val = strtol(s, &end, 10);
    if (end == s) return 0;
    return val;
}

static bool if_to_bool(const char *s)
{
    if (!s || !*s) return false;
    /* If all digits, convert to int */
    const char *p = s;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '\0' && p > s)
        return strtol(s, NULL, 10) != 0;
    /* Non-empty non-numeric string is truthy */
    return true;
}

static char *if_parse_cmp(IfExprParser *ep)
{
    char *left = if_parse_atom(ep);
    if_skip_ws(ep);

    /* Check for comparison operators */
    if (ep->pos[0] == '=' && ep->pos[1] == '=') {
        ep->pos += 2;
        char *right = if_parse_atom(ep);
        return arena_strdup(&ep->pp->arena, strcmp(left, right) == 0 ? "1" : "0");
    }
    if ((ep->pos[0] == '!' && ep->pos[1] == '=') ||
        (ep->pos[0] == '<' && ep->pos[1] == '>')) {
        ep->pos += 2;
        char *right = if_parse_atom(ep);
        return arena_strdup(&ep->pp->arena, strcmp(left, right) != 0 ? "1" : "0");
    }
    if (ep->pos[0] == '<' && ep->pos[1] == '=') {
        ep->pos += 2;
        char *right = if_parse_atom(ep);
        return arena_strdup(&ep->pp->arena, if_to_int(left) <= if_to_int(right) ? "1" : "0");
    }
    if (ep->pos[0] == '>' && ep->pos[1] == '=') {
        ep->pos += 2;
        char *right = if_parse_atom(ep);
        return arena_strdup(&ep->pp->arena, if_to_int(left) >= if_to_int(right) ? "1" : "0");
    }
    if (ep->pos[0] == '<') {
        ep->pos += 1;
        char *right = if_parse_atom(ep);
        return arena_strdup(&ep->pp->arena, if_to_int(left) < if_to_int(right) ? "1" : "0");
    }
    if (ep->pos[0] == '>') {
        ep->pos += 1;
        char *right = if_parse_atom(ep);
        return arena_strdup(&ep->pp->arena, if_to_int(left) > if_to_int(right) ? "1" : "0");
    }

    return left;
}

static char *if_parse_and(IfExprParser *ep)
{
    char *left = if_parse_cmp(ep);
    while (1) {
        if_skip_ws(ep);
        if (ep->pos[0] == '&' && ep->pos[1] == '&') {
            ep->pos += 2;
            char *right = if_parse_cmp(ep);
            left = arena_strdup(&ep->pp->arena,
                               (if_to_bool(left) && if_to_bool(right)) ? "1" : "0");
        } else {
            break;
        }
    }
    return left;
}

static char *if_parse_or(IfExprParser *ep)
{
    char *left = if_parse_and(ep);
    while (1) {
        if_skip_ws(ep);
        if (ep->pos[0] == '|' && ep->pos[1] == '|') {
            ep->pos += 2;
            char *right = if_parse_and(ep);
            left = arena_strdup(&ep->pp->arena,
                               (if_to_bool(left) || if_to_bool(right)) ? "1" : "0");
        } else {
            break;
        }
    }
    return left;
}

/* Handle: #if EXPR */
static void handle_if(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    bool result = false;

    IfExprParser ep = { .pp = pp, .pos = p };
    char *val = if_parse_or(&ep);

    /* If numeric, check non-zero. If non-numeric, check if defined. */
    const char *v = val;
    bool is_number = (*v != '\0');
    for (const char *c = v; *c; c++) {
        if (*c < '0' || *c > '9') { is_number = false; break; }
    }
    if (is_number && *v)
        result = strtol(v, NULL, 10) != 0;
    else if (*v)
        result = preproc_is_defined(pp, v);

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

    /* Check for ONCE keyword (may come before [arch:] modifier) */
    bool once = false;
    if (strnicmp_local(p, "once", 4) == 0 && !is_id_char(p[4])) {
        once = true;
        p += 4;
        p = skip_ws(p);
    }

    /* Check for optional [arch:XXX] modifier */
    char arch_value[64] = "";
    if (*p == '[') {
        p++; /* skip '[' */
        /* Parse modifier: arch:value */
        p = skip_ws(p);
        if (strnicmp_local(p, "arch", 4) == 0 && p[4] == ':') {
            p += 5;
            int ai = 0;
            while (*p && *p != ']' && ai < 63) {
                arch_value[ai++] = *p++;
            }
            arch_value[ai] = '\0';
            /* Trim whitespace from arch value */
            while (ai > 0 && (arch_value[ai-1] == ' ' || arch_value[ai-1] == '\t'))
                arch_value[--ai] = '\0';
        }
        while (*p && *p != ']') p++;
        if (*p == ']') p++;
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
        char *expanded = expand_macros_in_text(pp, p);
        if (expanded) {
            expanded = arena_strdup(&pp->arena, expanded);
            strip_trailing(expanded);
            const char *ep = skip_ws(expanded);
            if (*ep == '<') {
                is_system = true;
                ep++;
                int fi = 0;
                while (*ep && *ep != '>' && fi < (int)sizeof(filename) - 1)
                    filename[fi++] = *ep++;
                filename[fi] = '\0';
            } else if (*ep == '"') {
                ep++;
                int fi = 0;
                while (*ep && *ep != '"' && fi < (int)sizeof(filename) - 1)
                    filename[fi++] = *ep++;
                filename[fi] = '\0';
            } else {
                preproc_error(pp, "expected filename after #include");
                return;
            }
        } else {
            preproc_error(pp, "expected filename after #include");
            return;
        }
    }

    /* Convert Windows-style backslash paths to forward slash */
    for (int fi = 0; filename[fi]; fi++) {
        if (filename[fi] == '\\') filename[fi] = '/';
    }

    /* Strip leading ./ from filename */
    char *fn = filename;
    while (fn[0] == '.' && fn[1] == '/') fn += 2;

    /* If an architecture modifier was specified, try arch-specific include path first */
    char *resolved = NULL;
    if (arch_value[0]) {
        const char *default_arch = pp->arch ? pp->arch : "zx48k";
        for (int ip = 0; ip < pp->include_paths.len; ip++) {
            const char *ipath = pp->include_paths.data[ip];
            /* Check if include path contains the default arch */
            const char *arch_pos = strstr(ipath, default_arch);
            if (arch_pos) {
                /* Build path with substituted arch */
                char arch_ipath[PATH_MAX];
                int prefix_len = (int)(arch_pos - ipath);
                snprintf(arch_ipath, sizeof(arch_ipath), "%.*s%s%s",
                        prefix_len, ipath, arch_value,
                        arch_pos + strlen(default_arch));
                char trypath[PATH_MAX];
                snprintf(trypath, sizeof(trypath), "%s/%s", arch_ipath, fn);
                if (access(trypath, R_OK) == 0) {
                    const char *norm = trypath;
                    while (norm[0] == '.' && norm[1] == '/') norm += 2;
                    resolved = arena_strdup(&pp->arena, norm);
                    break;
                }
            }
        }
    }

    /* Standard path resolution */
    if (!resolved)
        resolved = resolve_include(pp, fn, is_system);
    if (!resolved) {
        preproc_error(pp, "cannot find include file \"%s\"", filename);
        return;
    }

    /* Normalize: make path relative to CWD if possible */
    {
        char abs_resolved[PATH_MAX];
        char abs_cwd[PATH_MAX];
        if (realpath(resolved, abs_resolved) && getcwd(abs_cwd, sizeof(abs_cwd))) {
            size_t cwd_len = strlen(abs_cwd);
            if (strncmp(abs_resolved, abs_cwd, cwd_len) == 0 &&
                abs_resolved[cwd_len] == '/') {
                resolved = arena_strdup(&pp->arena, abs_resolved + cwd_len + 1);
            }
        }
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

    /* Restore file state and emit parent's #line */
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

    /* Parse and format pragma variants to match Python output:
     *   #pragma ID              → "#pragma ID"
     *   #pragma ID = ID/INT     → "#pragma ID = value"
     *   #pragma ID = "STRING"   → "#pragma ID = STRING" (quotes stripped)
     *   #pragma push(ID)        → "#pragma push(ID)"
     *   #pragma pop(ID)         → "#pragma pop(ID)"
     */
    const char *pr = skip_ws(rest);

    /* Check for push/pop: e.g. "push(case_insensitive)" */
    if ((strnicmp_local(pr, "push", 4) == 0 || strnicmp_local(pr, "pop", 3) == 0) &&
        pr[strnicmp_local(pr, "push", 4) == 0 ? 4 : 3] == '(') {
        /* Pass through as-is — Python formats as "#pragma push(ID)" with no spaces */
        strbuf_printf(&pp->output, "#pragma %s\n", pr);
        return false;
    }

    /* Parse: ID [= value] */
    int id_len = scan_id(pr);
    if (id_len > 0) {
        char *id = arena_strndup(&pp->arena, pr, (size_t)id_len);
        const char *after_id = skip_ws(pr + id_len);

        if (*after_id == '=') {
            /* #pragma ID = value */
            const char *val = skip_ws(after_id + 1);
            if (*val == '"') {
                /* String value — strip quotes */
                const char *end = strchr(val + 1, '"');
                if (end) {
                    char *stripped = arena_strndup(&pp->arena, val + 1, (size_t)(end - val - 1));
                    strbuf_printf(&pp->output, "#pragma %s = %s\n", id, stripped);
                } else {
                    strbuf_printf(&pp->output, "#pragma %s = %s\n", id, val);
                }
            } else {
                /* ID or integer value — trim trailing whitespace */
                int vlen = 0;
                while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r')
                    vlen++;
                while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t'))
                    vlen--;
                char *trimmed = arena_strndup(&pp->arena, val, (size_t)vlen);
                strbuf_printf(&pp->output, "#pragma %s = %s\n", id, trimmed);
            }
        } else {
            /* #pragma ID (no value) */
            strbuf_printf(&pp->output, "#pragma %s\n", id);
        }
    } else {
        /* Fallback — pass through */
        strbuf_printf(&pp->output, "#pragma %s\n", pr);
    }
    return false;
}

/* Handle: #require "file"
 * Python applies sanitize_filename() which replaces backslashes with forward slashes. */
static void handle_require(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);

    /* sanitize_filename: replace \ with / inside the string argument */
    StrBuf sanitized;
    strbuf_init(&sanitized);
    for (const char *c = p; *c && *c != '\n' && *c != '\r'; c++) {
        strbuf_append_char(&sanitized, *c == '\\' ? '/' : *c);
    }
    strbuf_printf(&pp->output, "#require %s\n", strbuf_cstr(&sanitized));
    strbuf_free(&sanitized);
}

/* Handle: #init "name" or #init name
 * Python: p_init (bare ID) wraps in quotes, p_init_str (STRING) passes through */
static void handle_init(PreprocState *pp, const char *rest)
{
    const char *p = skip_ws(rest);
    if (*p == '"') {
        /* Already a string — pass through as-is */
        strbuf_printf(&pp->output, "#init %s\n", p);
    } else {
        /* Bare identifier — wrap in quotes */
        int id_len = 0;
        while (p[id_len] && p[id_len] != ' ' && p[id_len] != '\t' &&
               p[id_len] != '\n' && p[id_len] != '\r')
            id_len++;
        char *id = arena_strndup(&pp->arena, p, (size_t)id_len);
        strbuf_printf(&pp->output, "#init \"%s\"\n", id);
    }
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
            /* Reset has_output: the inner program is a new grammar scope,
             * so the first define/content inside gets "first production" treatment. */
            pp->has_output = false;
        }
        return;
    }
    if (strnicmp_local(p, "ifndef", (size_t)dlen) == 0 && dlen == 6) {
        bool was_enabled = is_enabled(pp);
        handle_ifdef(pp, rest, true);
        bool now_enabled = is_enabled(pp);
        if (was_enabled && now_enabled) {
            strbuf_append_char(&pp->output, '\n');
            pp->has_output = false;
        }
        return;
    }
    if (strnicmp_local(p, "if", (size_t)dlen) == 0 && dlen == 2) {
        bool was_enabled = is_enabled(pp);
        handle_if(pp, rest);
        bool now_enabled = is_enabled(pp);
        if (was_enabled && now_enabled) {
            strbuf_append_char(&pp->output, '\n');
            pp->has_output = false;
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
        /* In ASM mode, #line is consumed silently (updates tracking only).
         * In BASIC mode, #line passes through to output. */
        if (!pp->in_asm) {
            const char *lr = skip_ws(rest);
            strbuf_printf(&pp->output, "#line %s\n", lr);
            pp->has_output = true;
        }
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
    /* Handle block comments (/' ... '/) with nesting support.
     * Python tracks __COMMENT_LEVEL as an integer counter. */
    if (pp->block_comment_level > 0) {
        const char *p = line;
        while (*p) {
            if (p[0] == '/' && p[1] == '\'') {
                /* Nested block comment open */
                pp->block_comment_level++;
                p += 2;
            } else if (p[0] == '\'' && p[1] == '/') {
                pp->block_comment_level--;
                p += 2;
                if (pp->block_comment_level == 0) {
                    /* Anything after '/ on this line is content.
                     * Don't strip whitespace — Python preserves it. */
                    if (*p) {
                        process_line(pp, p);
                        return;
                    }
                    break;
                }
            } else {
                p++;
            }
        }
        /* Still inside block comment (or just closed with nothing after) */
        if (is_enabled(pp)) {
            strbuf_append_char(&pp->output, '\n');
            pp->has_output = true;
        }
        return;
    }

    /* Check for block comment start /' */
    if (!pp->in_asm) {
        const char *bc = strstr(line, "/'");
        if (bc) {
            /* Scan for matching close, respecting nesting */
            int depth = 1;
            const char *p = bc + 2;
            const char *close = NULL;
            while (*p) {
                if (p[0] == '/' && p[1] == '\'') {
                    depth++;
                    p += 2;
                } else if (p[0] == '\'' && p[1] == '/') {
                    depth--;
                    if (depth == 0) {
                        close = p;
                        break;
                    }
                    p += 2;
                } else {
                    p++;
                }
            }
            if (close) {
                /* Block comment fully closed on same line — remove it and process rest */
                StrBuf cleaned;
                strbuf_init(&cleaned);
                strbuf_append_n(&cleaned, line, (size_t)(bc - line));
                strbuf_append(&cleaned, close + 2);
                char *cl = arena_strdup(&pp->arena, strbuf_cstr(&cleaned));
                strbuf_free(&cleaned);
                process_line(pp, cl);
                return;
            }
            /* Multi-line block comment starts here */
            pp->block_comment_level = depth;
            /* Content before /' on this line */
            if (bc > line) {
                char *before = arena_strndup(&pp->arena, line, (size_t)(bc - line));
                const char *t = skip_ws(before);
                if (*t) {
                    process_line(pp, before);
                    return;
                }
            }
            if (is_enabled(pp)) {
                strbuf_append_char(&pp->output, '\n');
                pp->has_output = true;
            }
            return;
        }
    }

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

    /* In ASM mode, backslash is not a valid token (Python lexer rejects it) */
    if (pp->in_asm && strchr(to_expand, '\\')) {
        preproc_error(pp, "illegal preprocessor character '\\'");
        return;
    }

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

    /* Register builtins (once per PreprocState instance) */
    if (!pp->builtins_registered) {
        register_builtins(pp);
        pp->builtins_registered = true;
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
            char last = cur[curlen - 1];
            /* Backslash continuation (for #define and ASM lines) */
            if (last == '\\') {
                continued = true;
                if (pp->in_asm) {
                    /* In ASM mode, join lines by removing the backslash */
                    linebuf.len--;
                    linebuf.data[linebuf.len] = '\0';
                } else {
                    /* Replace backslash with newline to preserve line structure */
                    linebuf.data[linebuf.len - 1] = '\n';
                }
            }
            /* Underscore continuation (BASIC line continuation).
             * Only when _ is at end of line AND is not part of an identifier.
             * i.e., preceded by non-identifier char or is the only char. */
            else if (last == '_' && (curlen == 1 || !is_id_char(cur[curlen - 2]))) {
                continued = true;
                /* Keep the _ and append \n */
                strbuf_append_char(&linebuf, '\n');
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
    /* Register builtins (once per PreprocState instance) */
    if (!pp->builtins_registered) {
        register_builtins(pp);
        pp->builtins_registered = true;
    }

    pp->current_file = arena_strdup(&pp->arena, filename ? filename : "<string>");
    pp->current_line = 1;
    preproc_emit_line(pp, 1, pp->current_file);

    /* Make a mutable copy and process with continuation support
     * (matching preproc_file's line handling) */
    char *copy = strdup(input);
    char *line_start = copy;
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

        strbuf_append_n(&linebuf, line_start, line_len);

        /* Check for line continuation */
        const char *cur = strbuf_cstr(&linebuf);
        int curlen = (int)strlen(cur);
        bool continued = false;

        if (curlen > 0) {
            char last = cur[curlen - 1];
            if (last == '\\') {
                continued = true;
                if (pp->in_asm) {
                    linebuf.len--;
                    linebuf.data[linebuf.len] = '\0';
                } else {
                    linebuf.data[linebuf.len - 1] = '\n';
                }
            } else if (last == '_' && (curlen == 1 || !is_id_char(cur[curlen - 2]))) {
                continued = true;
                strbuf_append_char(&linebuf, '\n');
            }
        }

        if (!continued) {
            char *complete_line = arena_strdup(&pp->arena, strbuf_cstr(&linebuf));
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
    free(copy);
    return pp->error_count;
}
