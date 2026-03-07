/*
 * errmsg.c — Error and warning message system
 *
 * Ported from src/api/errmsg.py
 */
#include "zxbc.h"
#include "errmsg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Output helpers
 * ---------------------------------------------------------------- */

static FILE *err_stream(CompilerState *cs) {
    return cs->opts.stderr_f ? cs->opts.stderr_f : stderr;
}

void zxbc_msg_output(CompilerState *cs, const char *msg) {
    /* Deduplicate messages */
    if (hashmap_get(&cs->error_cache, msg) != NULL)
        return;
    hashmap_set(&cs->error_cache, msg, (void *)1);
    fprintf(err_stream(cs), "%s\n", msg);
}

void zxbc_info(CompilerState *cs, const char *fmt, ...) {
    if (cs->opts.debug_level < 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(err_stream(cs), "info: ");
    vfprintf(err_stream(cs), fmt, ap);
    fprintf(err_stream(cs), "\n");
    va_end(ap);
}

/* ----------------------------------------------------------------
 * Core error/warning
 * ---------------------------------------------------------------- */

void zxbc_error(CompilerState *cs, int lineno, const char *fmt, ...) {
    const char *fname = cs->current_file ? cs->current_file : "<unknown>";

    if (cs->error_count > cs->opts.max_syntax_errors) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s:%d: error: Too many errors. Giving up!", fname, lineno);
        zxbc_msg_output(cs, buf);
        exit(1);
    }

    char msg_body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_body, sizeof(msg_body), fmt, ap);
    va_end(ap);

    char full_msg[1280];
    snprintf(full_msg, sizeof(full_msg), "%s:%d: error: %s", fname, lineno, msg_body);
    zxbc_msg_output(cs, full_msg);

    cs->error_count++;
    if (cs->error_count > cs->opts.max_syntax_errors) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s:%d: error: Too many errors. Giving up!", fname, lineno);
        zxbc_msg_output(cs, buf);
        exit(1);
    }
}

void zxbc_warning(CompilerState *cs, int lineno, const char *fmt, ...) {
    cs->warning_count++;
    if (cs->warning_count <= cs->opts.expected_warnings)
        return;

    const char *fname = cs->current_file ? cs->current_file : "<unknown>";

    char msg_body[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg_body, sizeof(msg_body), fmt, ap);
    va_end(ap);

    char full_msg[1280];
    snprintf(full_msg, sizeof(full_msg), "%s:%d: warning: %s", fname, lineno, msg_body);
    zxbc_msg_output(cs, full_msg);
}

/* ----------------------------------------------------------------
 * Specific error messages
 * ---------------------------------------------------------------- */

void err_expected_string(CompilerState *cs, int lineno, const char *type_name) {
    zxbc_error(cs, lineno, "Expected a 'string' type expression, got '%s' instead", type_name);
}

void err_wrong_for_var(CompilerState *cs, int lineno, const char *expected, const char *got) {
    zxbc_error(cs, lineno, "FOR variable should be '%s' instead of '%s'", expected, got);
}

void err_not_constant(CompilerState *cs, int lineno) {
    zxbc_error(cs, lineno, "Initializer expression is not constant.");
}

void err_not_array_nor_func(CompilerState *cs, int lineno, const char *name) {
    zxbc_error(cs, lineno, "'%s' is neither an array nor a function.", name);
}

void err_not_an_array(CompilerState *cs, int lineno, const char *name) {
    zxbc_error(cs, lineno, "'%s' is not an array (or has not been declared yet)", name);
}

void err_func_type_mismatch(CompilerState *cs, int lineno, const char *name, int prev_lineno) {
    zxbc_error(cs, lineno, "Function '%s' (previously declared at %d) type mismatch", name, prev_lineno);
}

void err_parameter_mismatch(CompilerState *cs, int lineno, const char *name, int prev_lineno) {
    zxbc_error(cs, lineno, "Function '%s' (previously declared at %d) parameter mismatch", name, prev_lineno);
}

void err_cant_convert(CompilerState *cs, int lineno, const char *expr_str, const char *type_name) {
    zxbc_error(cs, lineno, "Cant convert '%s' to type %s", expr_str, type_name);
}

void err_is_sub_not_func(CompilerState *cs, int lineno, const char *name) {
    zxbc_error(cs, lineno, "'%s' is a SUB not a FUNCTION", name);
}

void err_undeclared_type(CompilerState *cs, int lineno, const char *id) {
    zxbc_error(cs, lineno, "strict mode: missing type declaration for '%s'", id);
}

void err_cannot_assign(CompilerState *cs, int lineno, const char *id) {
    zxbc_error(cs, lineno, "Cannot assign a value to '%s'. It's not a variable", id);
}

void err_address_must_be_constant(CompilerState *cs, int lineno) {
    zxbc_error(cs, lineno, "Address must be a numeric constant expression");
}

void err_cannot_pass_array_by_value(CompilerState *cs, int lineno, const char *id) {
    zxbc_error(cs, lineno, "Array parameter '%s' must be passed ByRef", id);
}

void err_no_data_defined(CompilerState *cs, int lineno) {
    zxbc_error(cs, lineno, "No DATA defined");
}

void err_cannot_init_array_of_type(CompilerState *cs, int lineno, const char *type_name) {
    zxbc_error(cs, lineno, "Cannot initialize array of type %s", type_name);
}

void err_cannot_define_default_array_arg(CompilerState *cs, int lineno) {
    zxbc_error(cs, lineno, "Cannot define default array argument");
}

void err_unexpected_class(CompilerState *cs, int lineno, const char *name,
                          const char *wrong_class, const char *good_class) {
    const char *n1 = (wrong_class[0] == 'a' || wrong_class[0] == 'A') ? "n" : "";
    const char *n2 = (good_class[0] == 'a' || good_class[0] == 'A') ? "n" : "";
    zxbc_error(cs, lineno, "'%s' is a%s %s, not a%s %s", name, n1, wrong_class, n2, good_class);
}

void err_already_declared(CompilerState *cs, int lineno, const char *name,
                          const char *as_class, int at_lineno) {
    zxbc_error(cs, lineno, "'%s' already declared as %s at %d", name, as_class, at_lineno);
}

void err_mandatory_after_optional(CompilerState *cs, int lineno, const char *param1, const char *param2) {
    zxbc_error(cs, lineno, "Can't declare mandatory param '%s' after optional param '%s'", param2, param1);
}

void err_for_without_next(CompilerState *cs, int lineno) {
    zxbc_error(cs, lineno, "FOR without NEXT");
}

void err_loop_not_closed(CompilerState *cs, int lineno, const char *loop_type) {
    zxbc_error(cs, lineno, "%s loop not closed", loop_type);
}

/* ----------------------------------------------------------------
 * Warning messages with codes
 * ---------------------------------------------------------------- */

void warn_implicit_type(CompilerState *cs, int lineno, const char *id, const char *type_name) {
    if (cs->opts.strict) {
        err_undeclared_type(cs, lineno, id);
        return;
    }
    if (!type_name && cs->default_type)
        type_name = cs->default_type->name;
    zxbc_warning(cs, lineno, "Using default implicit type '%s' for '%s'", type_name, id);
}

void warn_condition_always(CompilerState *cs, int lineno, bool cond) {
    zxbc_warning(cs, lineno, "Condition is always %s", cond ? "True" : "False");
}

void warn_conversion_lose_digits(CompilerState *cs, int lineno) {
    zxbc_warning(cs, lineno, "Conversion may lose significant digits");
}

void warn_empty_loop(CompilerState *cs, int lineno) {
    zxbc_warning(cs, lineno, "Empty loop");
}

void warn_empty_if(CompilerState *cs, int lineno) {
    zxbc_warning(cs, lineno, "Useless empty IF ignored");
}

void warn_not_used(CompilerState *cs, int lineno, const char *id, const char *kind) {
    if (cs->opts.optimization_level > 0)
        zxbc_warning(cs, lineno, "%s '%s' is never used", kind, id);
}

void warn_fastcall_n_params(CompilerState *cs, int lineno, const char *kind, const char *id, int n) {
    zxbc_warning(cs, lineno, "%s '%s' declared as FASTCALL with %d parameters", kind, id, n);
}

void warn_func_never_called(CompilerState *cs, int lineno, const char *name) {
    zxbc_warning(cs, lineno, "Function '%s' is never called and has been ignored", name);
}

void warn_unreachable_code(CompilerState *cs, int lineno) {
    zxbc_warning(cs, lineno, "Unreachable code");
}

void warn_function_should_return(CompilerState *cs, int lineno, const char *name) {
    zxbc_warning(cs, lineno, "Function '%s' should return a value", name);
}

void warn_value_truncated(CompilerState *cs, int lineno) {
    zxbc_warning(cs, lineno, "Value will be truncated");
}

void warn_unknown_pragma(CompilerState *cs, int lineno, const char *pragma_name) {
    zxbc_warning(cs, lineno, "Ignoring unknown pragma '%s'", pragma_name);
}
