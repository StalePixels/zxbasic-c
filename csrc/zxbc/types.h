/*
 * types.h — Type system enums and constants for ZX BASIC compiler
 *
 * Ported from src/api/constants.py (TYPE, CLASS, SCOPE, CONVENTION enums)
 * and src/symbols/type_.py (type size/properties).
 */
#ifndef ZXBC_TYPES_H
#define ZXBC_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 * Primary type constants (from TYPE IntEnum in constants.py)
 * ---------------------------------------------------------------- */
typedef enum {
    TYPE_unknown  = 0,
    TYPE_byte     = 1,
    TYPE_ubyte    = 2,
    TYPE_integer  = 3,
    TYPE_uinteger = 4,
    TYPE_long     = 5,
    TYPE_ulong    = 6,
    TYPE_fixed    = 7,
    TYPE_float    = 8,
    TYPE_string   = 9,
    TYPE_boolean  = 10,
    TYPE_COUNT    = 11,
} BasicType;

/* Size in bytes of each basic type */
static inline int basictype_size(BasicType t) {
    static const int sizes[TYPE_COUNT] = {
        0, /* unknown */
        1, /* byte */
        1, /* ubyte */
        2, /* integer */
        2, /* uinteger */
        4, /* long */
        4, /* ulong */
        4, /* fixed */
        5, /* float */
        2, /* string (pointer) */
        1, /* boolean */
    };
    return (t >= 0 && t < TYPE_COUNT) ? sizes[t] : 0;
}

/* Type name strings */
static inline const char *basictype_to_string(BasicType t) {
    static const char *names[TYPE_COUNT] = {
        "unknown", "byte", "ubyte", "integer", "uinteger",
        "long", "ulong", "fixed", "float", "string", "boolean",
    };
    return (t >= 0 && t < TYPE_COUNT) ? names[t] : "unknown";
}

/* Convert type name to BasicType. Returns TYPE_unknown on failure. */
static inline BasicType basictype_from_string(const char *name) {
    static const char *names[TYPE_COUNT] = {
        "unknown", "byte", "ubyte", "integer", "uinteger",
        "long", "ulong", "fixed", "float", "string", "boolean",
    };
    for (int i = 0; i < TYPE_COUNT; i++) {
        /* Simple case-sensitive comparison */
        const char *a = names[i];
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return (BasicType)i;
    }
    return TYPE_unknown;
}

/* Type property predicates */
static inline bool basictype_is_signed(BasicType t) {
    return t == TYPE_byte || t == TYPE_integer || t == TYPE_long ||
           t == TYPE_fixed || t == TYPE_float;
}

static inline bool basictype_is_unsigned(BasicType t) {
    return t == TYPE_boolean || t == TYPE_ubyte ||
           t == TYPE_uinteger || t == TYPE_ulong;
}

static inline bool basictype_is_integral(BasicType t) {
    return t == TYPE_boolean || t == TYPE_byte || t == TYPE_ubyte ||
           t == TYPE_integer || t == TYPE_uinteger ||
           t == TYPE_long || t == TYPE_ulong;
}

static inline bool basictype_is_decimal(BasicType t) {
    return t == TYPE_fixed || t == TYPE_float;
}

static inline bool basictype_is_numeric(BasicType t) {
    return basictype_is_integral(t) || basictype_is_decimal(t);
}

/* Convert unsigned type to signed equivalent */
static inline BasicType basictype_to_signed(BasicType t) {
    switch (t) {
        case TYPE_boolean:  return TYPE_byte;
        case TYPE_ubyte:    return TYPE_byte;
        case TYPE_uinteger: return TYPE_integer;
        case TYPE_ulong:    return TYPE_long;
        default:
            if (basictype_is_signed(t) || basictype_is_decimal(t))
                return t;
            return TYPE_unknown;
    }
}

/* ----------------------------------------------------------------
 * Symbol class constants (from CLASS StrEnum in constants.py)
 * ---------------------------------------------------------------- */
typedef enum {
    CLASS_unknown  = 0,
    CLASS_var      = 1,
    CLASS_array    = 2,
    CLASS_function = 3,
    CLASS_label    = 4,
    CLASS_const    = 5,
    CLASS_sub      = 6,
    CLASS_type     = 7,
} SymbolClass;

static inline const char *symbolclass_to_string(SymbolClass c) {
    static const char *names[] = {
        "unknown", "var", "array", "function", "label", "const", "sub", "type",
    };
    return (c >= 0 && c <= CLASS_type) ? names[c] : "unknown";
}

/* ----------------------------------------------------------------
 * Scope constants (from SCOPE Enum in constants.py)
 * ---------------------------------------------------------------- */
typedef enum {
    SCOPE_global    = 0,
    SCOPE_local     = 1,
    SCOPE_parameter = 2,
} Scope;

static inline const char *scope_to_string(Scope s) {
    static const char *names[] = { "global", "local", "parameter" };
    return (s >= 0 && s <= SCOPE_parameter) ? names[s] : "global";
}

/* ----------------------------------------------------------------
 * Calling convention (from CONVENTION Enum in constants.py)
 * ---------------------------------------------------------------- */
typedef enum {
    CONV_unknown   = 0,
    CONV_fastcall  = 1,
    CONV_stdcall   = 2,
} Convention;

static inline const char *convention_to_string(Convention c) {
    static const char *names[] = { "unknown", "__fastcall__", "__stdcall__" };
    return (c >= 0 && c <= CONV_stdcall) ? names[c] : "unknown";
}

/* ----------------------------------------------------------------
 * Loop type (from LoopType Enum in constants.py)
 * ---------------------------------------------------------------- */
typedef enum {
    LOOP_DO    = 0,
    LOOP_FOR   = 1,
    LOOP_WHILE = 2,
} LoopType;

/* ----------------------------------------------------------------
 * Array constants (from ARRAY class in constants.py)
 * ---------------------------------------------------------------- */
#define ARRAY_BOUND_SIZE      2  /* bytes per bound entry */
#define ARRAY_BOUND_COUNT     2  /* bytes for bounds counter */
#define ARRAY_TYPE_SIZE       1  /* bytes for array type marker */

/* ----------------------------------------------------------------
 * Deprecated suffixes for variable names (a$, a%, a&)
 * ---------------------------------------------------------------- */
static inline BasicType suffix_to_type(char suffix) {
    switch (suffix) {
        case '$': return TYPE_string;
        case '%': return TYPE_integer;
        case '&': return TYPE_long;
        default:  return TYPE_unknown;
    }
}

static inline bool is_deprecated_suffix(char c) {
    return c == '$' || c == '%' || c == '&';
}

#endif /* ZXBC_TYPES_H */
