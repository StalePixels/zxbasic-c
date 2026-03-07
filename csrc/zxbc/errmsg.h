/*
 * errmsg.h — Error and warning message system for ZX BASIC compiler
 *
 * Ported from src/api/errmsg.py. Provides formatted error/warning output
 * with deduplication (matching Python's error_msg_cache behavior).
 */
#ifndef ZXBC_ERRMSG_H
#define ZXBC_ERRMSG_H

#include "arena.h"
#include "hashmap.h"
#include "options.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

/* Forward declaration */
typedef struct CompilerState CompilerState;

/* ----------------------------------------------------------------
 * Error/warning reporting
 * ---------------------------------------------------------------- */

/* Core error function: fname:lineno: error: msg */
void zxbc_error(CompilerState *cs, int lineno, const char *fmt, ...);

/* Core warning function: fname:lineno: warning: msg */
void zxbc_warning(CompilerState *cs, int lineno, const char *fmt, ...);

/* Output a message (with dedup) */
void zxbc_msg_output(CompilerState *cs, const char *msg);

/* Info messages (only if debug_level >= 1) */
void zxbc_info(CompilerState *cs, const char *fmt, ...);

/* ----------------------------------------------------------------
 * Specific error messages (matching Python's errmsg.py functions)
 * ---------------------------------------------------------------- */
void err_expected_string(CompilerState *cs, int lineno, const char *type_name);
void err_wrong_for_var(CompilerState *cs, int lineno, const char *expected, const char *got);
void err_not_constant(CompilerState *cs, int lineno);
void err_not_array_nor_func(CompilerState *cs, int lineno, const char *name);
void err_not_an_array(CompilerState *cs, int lineno, const char *name);
void err_func_type_mismatch(CompilerState *cs, int lineno, const char *name, int prev_lineno);
void err_parameter_mismatch(CompilerState *cs, int lineno, const char *name, int prev_lineno);
void err_cant_convert(CompilerState *cs, int lineno, const char *expr_str, const char *type_name);
void err_is_sub_not_func(CompilerState *cs, int lineno, const char *name);
void err_undeclared_type(CompilerState *cs, int lineno, const char *id);
void err_cannot_assign(CompilerState *cs, int lineno, const char *id);
void err_address_must_be_constant(CompilerState *cs, int lineno);
void err_cannot_pass_array_by_value(CompilerState *cs, int lineno, const char *id);
void err_no_data_defined(CompilerState *cs, int lineno);
void err_cannot_init_array_of_type(CompilerState *cs, int lineno, const char *type_name);
void err_cannot_define_default_array_arg(CompilerState *cs, int lineno);
void err_unexpected_class(CompilerState *cs, int lineno, const char *name,
                          const char *wrong_class, const char *good_class);
void err_already_declared(CompilerState *cs, int lineno, const char *name,
                          const char *as_class, int at_lineno);
void err_mandatory_after_optional(CompilerState *cs, int lineno, const char *param1, const char *param2);
void err_for_without_next(CompilerState *cs, int lineno);
void err_loop_not_closed(CompilerState *cs, int lineno, const char *loop_type);

/* ----------------------------------------------------------------
 * Warning messages with codes (matching Python's registered warnings)
 * ---------------------------------------------------------------- */
void warn_implicit_type(CompilerState *cs, int lineno, const char *id, const char *type_name);
void warn_condition_always(CompilerState *cs, int lineno, bool cond);
void warn_conversion_lose_digits(CompilerState *cs, int lineno);
void warn_empty_loop(CompilerState *cs, int lineno);
void warn_empty_if(CompilerState *cs, int lineno);
void warn_not_used(CompilerState *cs, int lineno, const char *id, const char *kind);
void warn_fastcall_n_params(CompilerState *cs, int lineno, const char *kind, const char *id, int n);
void warn_func_never_called(CompilerState *cs, int lineno, const char *name);
void warn_unreachable_code(CompilerState *cs, int lineno);
void warn_function_should_return(CompilerState *cs, int lineno, const char *name);
void warn_value_truncated(CompilerState *cs, int lineno);
void warn_unknown_pragma(CompilerState *cs, int lineno, const char *pragma_name);

#endif /* ZXBC_ERRMSG_H */
