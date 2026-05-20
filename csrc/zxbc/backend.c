/*
 * Z80 backend emitter — port of src/arch/z80/backend/main.py + generic.py
 * + _16bit.py (the slice the S5.2 calibration exercises, ported faithfully
 * incl. branches calib does not hit so later S5.x build on real code).
 *
 *   Bits16.get_oper        _16bit.py:26-116   (full, op1+op2)
 *   _end                   generic.py:64-89
 *   _inline                generic.py:552-583
 *   _QUAD_TABLE dispatch   main.py:151-611    (END/INLINE here; grows S5.x)
 *   Backend.emit           main.py:766-785
 *   _output_join           main.py:746-764    (inline peephole)
 *   remove_unused_labels   main.py:700-743
 *   emit_prologue          main.py:638-681
 *   emit_epilogue          main.py:684-697
 *
 * Lines are arena-owned char*; a returned StrVec is a heap VEC the caller
 * vec_free()s (strings outlive it in the arena).
 */
#include "backend.h"
#include "z80asm.h"
#include "peephole/engine.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* common.MEMINITS (common.py:22-31) — for emit_prologue's heap branch. */
static const char *const MEMINITS[] = {
    "mem/alloc.asm", "loadstr.asm", "storestr2.asm", "storestr.asm",
    "strcat.asm", "strcpy.asm", "string.asm", "strslice.asm",
};

/* ---- StrVec helpers (arena strings, heap container) ------------------- */

static StrVec sv_new(void) { StrVec v; vec_init(v); return v; }

static void sv_push(Backend *b, StrVec *v, const char *s) {
    char *c = arena_strdup(b->arena, s);
    vec_push(*v, c);
}

static void sv_pushf(Backend *b, StrVec *v, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sv_push(b, v, buf);
}

/* Forward decl: tmp_labels.tmp_label() (src/api/tmp_labels.py:16-25).
 * Defined further down; the 8/16-bit shift/and8 emitters need it before
 * its definition point. */
static char *s_tmp_label(Backend *b);

/* ---- Bits16.get_oper (_16bit.py:26-116) ------------------------------ */
/*
 * Faithful op1 (+op2) pop/load sequence. The S5.2 calibration only ever
 * reaches the op1 is_int path ("0" -> "ld hl, 0"); the rest is ported
 * verbatim-faithfully for the integer-scalar sprints (S5.3+).
 */
static StrVec bits16_get_oper(Backend *b, const char *op1, const char *op2,
                              bool reversed) {
    StrVec out = sv_new();
    const char *o1 = op1, *o2 = op2;
    if (o2 != NULL && reversed) { const char *t = o1; o1 = o2; o2 = t; }

    const char *op = o1;
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    int iv;
    if (z80h_is_int(op)) {
        z80h_int16(op, &iv);
        if (indirect) sv_pushf(b, &out, "ld hl, (%d)", iv);
        else          sv_pushf(b, &out, "ld hl, %d", iv);
    } else if (immediate) {
        if (indirect) sv_pushf(b, &out, "ld hl, (%s)", op);
        else          sv_pushf(b, &out, "ld hl, %s", op);
    } else {
        if (op[0] == '_') sv_pushf(b, &out, "ld hl, (%s)", op);
        else              sv_push(b, &out, "pop hl");
        if (indirect) {
            sv_push(b, &out, "ld a, (hl)");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld h, (hl)");
            sv_push(b, &out, "ld l, a");
        }
    }

    if (o2 == NULL) return out;

    StrVec tmp = sv_new();
    if (!reversed) { tmp = out; out = sv_new(); }

    op = o2;
    indirect = op[0] == '*';
    if (indirect) op++;
    immediate = op[0] == '#';
    if (immediate) op++;

    if (z80h_is_int(op)) {
        z80h_int16(op, &iv);
        if (indirect) sv_pushf(b, &out, "ld de, (%d)", iv);
        else          sv_pushf(b, &out, "ld de, %d", iv);
    } else if (immediate) {
        sv_pushf(b, &out, "ld de, %s", op);
    } else {
        if (op[0] == '_') sv_pushf(b, &out, "ld de, (%s)", op);
        else              sv_push(b, &out, "pop de");
        if (indirect) sv_push(b, &out, "call __LOAD_DE_DE"); /* RuntimeLabel.LOAD_DE_DE */
    }

    if (!reversed) {
        for (int i = 0; i < tmp.len; i++) vec_push(out, tmp.data[i]);
        vec_free(tmp);
    }
    return out;
}

/* ---- _end (generic.py:64-89) ----------------------------------------- */
static StrVec emit_end(Backend *b, Quad *ins) {
    /* output = Bits16.get_oper(ins[1]) ; ins[1] == args[0] */
    StrVec out = bits16_get_oper(b, ins->nargs > 0 ? ins->args[0] : "0",
                                 NULL, false);
    sv_push(b, &out, "ld b, h");
    sv_push(b, &out, "ld c, l");

    if (b->flag_end_emitted) {
        sv_pushf(b, &out, "jp %s", LBL_END);
        return out;
    }
    b->flag_end_emitted = true;

    sv_pushf(b, &out, "%s:", LBL_END);
    if (b->headerless) { sv_push(b, &out, "ret"); return out; }

    sv_push(b, &out, "di");
    sv_pushf(b, &out, "ld hl, (%s)", LBL_CALLBACK);
    sv_push(b, &out, "ld sp, hl");
    sv_push(b, &out, "exx");
    sv_push(b, &out, "pop hl");
    sv_push(b, &out, "exx");
    sv_push(b, &out, "pop iy");
    sv_push(b, &out, "pop ix");
    sv_push(b, &out, "ei");
    sv_push(b, &out, "ret");
    return out;
}

/* RE_LABEL = ^[ \t]*[a-zA-Z_][_a-zA-Z\d]*:  (generic.py:42) */
static bool re_label_match(const char *s) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (!(*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')))
        return false;
    p++;
    while (*p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9'))
        p++;
    return *p == ':';
}

/* strip(" \t\r") both ends, into the arena */
static char *strip_sp(Backend *b, const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r')) n--;
    char *r = arena_alloc(b->arena, n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* ---- _inline (generic.py:552-583) ------------------------------------ */
static StrVec emit_inline(Backend *b, Quad *ins) {
    const char *code = ins->nargs > 0 ? ins->args[0] : "";

    /* tmp = [x.strip(" \t\r") for x in ins[1].split("\n")] */
    StrVec tmp = sv_new();
    {
        const char *start = code;
        for (;;) {
            const char *nl = strchr(start, '\n');
            size_t seg = nl ? (size_t)(nl - start) : strlen(start);
            char *line = arena_alloc(b->arena, seg + 1);
            memcpy(line, start, seg);
            line[seg] = '\0';
            vec_push(tmp, strip_sp(b, line));
            if (!nl) break;
            start = nl + 1;
        }
    }

    for (int i = 0; i < tmp.len; i++) {
        char *t = tmp.data[i];
        if (t[0] == '\0' || t[0] == ';') continue;   /* empty / comment */
        if (t[0] == '#') continue;                     /* preproc directive */
        if (re_label_match(t)) continue;               /* starts with a label */
        /* not a label -> tabulate */
        size_t n = strlen(t);
        char *tabbed = arena_alloc(b->arena, n + 2);
        tabbed[0] = '\t';
        memcpy(tabbed + 1, t, n + 1);
        tmp.data[i] = tabbed;
    }

    StrVec out = sv_new();
    if (tmp.len == 0) { vec_free(tmp); return out; }

    char *asmid = backend_new_asmid(b);                /* "##ASM<n>" */
    AsmsBody *body = arena_alloc(b->arena, sizeof(AsmsBody));
    body->n = tmp.len;
    body->lines = arena_alloc(b->arena, (size_t)tmp.len * sizeof(char *));
    for (int i = 0; i < tmp.len; i++) body->lines[i] = tmp.data[i];
    vec_free(tmp);
    hashmap_set(&b->asms, asmid, body);                /* ASMS[asmid] = tmp */
    sv_push(b, &out, asmid);
    return out;
}

/* ====================================================================
 * S5.3 — 8/16-bit integer-scalar arithmetic & assignment emitters.
 * Faithful ports of src/arch/z80/backend/_8bit.py + _16bit.py +
 * generic.py (_var/_vard/_varx/_deflabel/_label/_cast) + common.py
 * (_int_ops / is_int / runtime_call) + runtime/core.py (RuntimeLabel +
 * LABEL_REQUIRED_MODULES). ==================================================*/

/* backend.common.is_int — z80h_is_int (Python int() decimal parse). */
static bool s_is_int(const char *op) { return z80h_is_int(op); }

/* Python int(op) (decimal, signed) -> long. Caller ensures s_is_int. */
static long s_int_val(const char *op) { return strtol(op, NULL, 10); }

/* Bits8.int8 (_8bit.py:18-23): int(op) & 0xFF. */
static int s_int8(long op) { return (int)(op & 0xFF); }
/* Bits16.int16 (_16bit.py:18-23): int(op) & 0xFFFF. */
static int s_int16(long op) { return (int)(op & 0xFFFF); }

/* _int_ops (common.py:197-213): returns true + (op_other, int_val) where
 * the integer operand is the 2nd. is_int(op1) -> (op2, int(op1)); elif
 * is_int(op2) -> (op1, int(op2)); else None. */
static bool s_int_ops(const char *op1, const char *op2,
                      const char **other, long *ival) {
    if (s_is_int(op1)) { *other = op2; *ival = s_int_val(op1); return true; }
    if (s_is_int(op2)) { *other = op1; *ival = s_int_val(op2); return true; }
    return false;
}

/* is_2n (common.py:138-146): x>=1, x==int(x), log2(x) integral. */
static bool s_is_2n(long x) {
    if (x < 1) return false;
    long v = x;
    while (v > 1) { if (v & 1) return false; v >>= 1; }
    return v == 1;
}
static int s_log2(long x) { int n = 0; while (x > 1) { x >>= 1; n++; } return n; }

/* RuntimeLabel string values (runtime/core.py; NAMESPACE == ".core"). */
#define RL_MUL8_FAST  ZXBC_NAMESPACE ".__MUL8_FAST"
#define RL_MUL16_FAST ZXBC_NAMESPACE ".__MUL16_FAST"
#define RL_DIVU8_FAST ZXBC_NAMESPACE ".__DIVU8_FAST"
#define RL_DIVI8_FAST ZXBC_NAMESPACE ".__DIVI8_FAST"
#define RL_DIVU16     ZXBC_NAMESPACE ".__DIVU16"
#define RL_DIVI16     ZXBC_NAMESPACE ".__DIVI16"
/* S7.x — 8/16-bit mod + bitwise/logical runtime helpers
 * (runtime/core.py CoreLabels; values + REQUIRED_MODULES verified). */
#define RL_MODU8_FAST ZXBC_NAMESPACE ".__MODU8_FAST"
#define RL_MODI8_FAST ZXBC_NAMESPACE ".__MODI8_FAST"
#define RL_MODU16     ZXBC_NAMESPACE ".__MODU16"
#define RL_MODI16     ZXBC_NAMESPACE ".__MODI16"
#define RL_XOR8       ZXBC_NAMESPACE ".__XOR8"
#define RL_XOR16      ZXBC_NAMESPACE ".__XOR16"
#define RL_BOR16      ZXBC_NAMESPACE ".__BOR16"
#define RL_BXOR16     ZXBC_NAMESPACE ".__BXOR16"
#define RL_AND16      ZXBC_NAMESPACE ".__AND16"
#define RL_BAND16     ZXBC_NAMESPACE ".__BAND16"
#define RL_NEGHL      ZXBC_NAMESPACE ".__NEGHL"
#define RL_LOAD_DE_DE ZXBC_NAMESPACE ".__LOAD_DE_DE"

/* S5.4 — wide (i32/u32/f16) + float (5-byte) RuntimeLabels
 * (runtime/core.py; values + REQUIRED_MODULES verified vs live Python). */
#define RL_ADDF       ZXBC_NAMESPACE ".__ADDF"
#define RL_SUBF       ZXBC_NAMESPACE ".__SUBF"
#define RL_MULF       ZXBC_NAMESPACE ".__MULF"
#define RL_DIVF       ZXBC_NAMESPACE ".__DIVF"
#define RL_MODF       ZXBC_NAMESPACE ".__MODF"
#define RL_POWF       ZXBC_NAMESPACE ".__POW"     /* NB: __POW, not __POWF */
#define RL_MULF16     ZXBC_NAMESPACE ".__MULF16"
#define RL_DIVF16     ZXBC_NAMESPACE ".__DIVF16"
#define RL_MODF16     ZXBC_NAMESPACE ".__MODF16"
#define RL_MUL32      ZXBC_NAMESPACE ".__MUL32"
#define RL_SUB32      ZXBC_NAMESPACE ".__SUB32"
#define RL_DIVU32     ZXBC_NAMESPACE ".__DIVU32"
#define RL_DIVI32     ZXBC_NAMESPACE ".__DIVI32"
#define RL_MODU32     ZXBC_NAMESPACE ".__MODU32"
#define RL_MODI32     ZXBC_NAMESPACE ".__MODI32"
#define RL_NEG32      ZXBC_NAMESPACE ".__NEG32"
#define RL_ABS32      ZXBC_NAMESPACE ".__ABS32"
#define RL_SWAP32     ZXBC_NAMESPACE ".__SWAP32"
#define RL_SHL32      ZXBC_NAMESPACE ".__SHL32"
#define RL_SHRL32     ZXBC_NAMESPACE ".__SHRL32"
#define RL_SHRA32     ZXBC_NAMESPACE ".__SHRA32"
#define RL_EQ32       ZXBC_NAMESPACE ".__EQ32"
#define RL_LTI32      ZXBC_NAMESPACE ".__LTI32"
#define RL_LEI32      ZXBC_NAMESPACE ".__LEI32"
#define RL_AND32      ZXBC_NAMESPACE ".__AND32"
#define RL_OR32       ZXBC_NAMESPACE ".__OR32"
#define RL_XOR32      ZXBC_NAMESPACE ".__XOR32"
#define RL_NOT32      ZXBC_NAMESPACE ".__NOT32"
#define RL_BAND32     ZXBC_NAMESPACE ".__BAND32"
#define RL_BOR32      ZXBC_NAMESPACE ".__BOR32"
#define RL_BXOR32     ZXBC_NAMESPACE ".__BXOR32"
#define RL_BNOT32     ZXBC_NAMESPACE ".__BNOT32"
#define RL_PLOADF     ZXBC_NAMESPACE ".__PLOADF"  /* core.py:104 */
#define RL_PISTORE16  ZXBC_NAMESPACE ".__PISTORE16" /* core.py:99 */
#define RL_PISTORE32  ZXBC_NAMESPACE ".__PISTORE32" /* core.py:100 */
#define RL_PISTORE_STR  ZXBC_NAMESPACE ".__PISTORE_STR"  /* core.py:102 */
#define RL_PISTORE_STR2 ZXBC_NAMESPACE ".__PISTORE_STR2" /* core.py:103 */
#define RL_PSTORE32   ZXBC_NAMESPACE ".__PSTORE32"  /* core.py:107 */
#define RL_PSTORE_STR  ZXBC_NAMESPACE ".__PSTORE_STR"   /* core.py:108 */
#define RL_PSTORE_STR2 ZXBC_NAMESPACE ".__PSTORE_STR2"  /* core.py:109 */
#define RL_ILOAD32    ZXBC_NAMESPACE ".__ILOAD32"
#define RL_STORE32    ZXBC_NAMESPACE ".__STORE32"
#define RL_ISTORE32   ZXBC_NAMESPACE ".__ISTORE32"
#define RL_LOADF      ZXBC_NAMESPACE ".__LOADF"
#define RL_ILOADF     ZXBC_NAMESPACE ".__ILOADF"
#define RL_STOREF     ZXBC_NAMESPACE ".__STOREF"
#define RL_ISTOREF    ZXBC_NAMESPACE ".__ISTOREF"
#define RL_FP_PUSH_REV ZXBC_NAMESPACE ".__FP_PUSH_REV"
#define RL_FTOU32REG  ZXBC_NAMESPACE ".__FTOU32REG"
#define RL_FTOF16REG  ZXBC_NAMESPACE ".__FTOF16REG"
#define RL_F16TOFREG  ZXBC_NAMESPACE ".__F16TOFREG"
#define RL_I32TOFREG  ZXBC_NAMESPACE ".__I32TOFREG"
#define RL_U32TOFREG  ZXBC_NAMESPACE ".__U32TOFREG"
#define RL_I8TOFREG   ZXBC_NAMESPACE ".__I8TOFREG"
#define RL_U8TOFREG   ZXBC_NAMESPACE ".__U8TOFREG"
#define RL_GEF        ZXBC_NAMESPACE ".__GEF"
#define RL_GTF        ZXBC_NAMESPACE ".__GTF"
#define RL_LEF        ZXBC_NAMESPACE ".__LEF"
#define RL_LTF        ZXBC_NAMESPACE ".__LTF"
#define RL_EQF        ZXBC_NAMESPACE ".__EQF"
#define RL_NEF        ZXBC_NAMESPACE ".__NEF"
#define RL_ORF        ZXBC_NAMESPACE ".__ORF"
#define RL_XORF       ZXBC_NAMESPACE ".__XORF"
#define RL_ANDF       ZXBC_NAMESPACE ".__ANDF"
#define RL_NOTF       ZXBC_NAMESPACE ".__NOTF"
#define RL_NEGF       ZXBC_NAMESPACE ".__NEGF"

/* S5.5 — control-flow RuntimeLabels (runtime/core.py; NAMESPACE == .core).
 * ON_GOTO/ON_GOSUB -> "ongoto.asm"; STOP/ERROR -> "error.asm";
 * CHECK_BREAK -> "break.asm" (core.py:35,47,95-96,114; labels.py map). */
/* S5.5 — 8/16-bit signed comparison RuntimeLabels (core.py:44,65-72;
 * the Bits8/Bits16 signed lt/le/gt/ge paths runtime_call these). */
#define RL_LTI8       ZXBC_NAMESPACE ".__LTI8"
#define RL_LEI8       ZXBC_NAMESPACE ".__LEI8"
#define RL_LTI16      ZXBC_NAMESPACE ".__LTI16"
#define RL_LEI16      ZXBC_NAMESPACE ".__LEI16"
#define RL_EQ16       ZXBC_NAMESPACE ".__EQ16"

#define RL_ON_GOTO     ZXBC_NAMESPACE ".__ON_GOTO"
#define RL_ON_GOSUB    ZXBC_NAMESPACE ".__ON_GOSUB"
#define RL_STOP        ZXBC_NAMESPACE ".__STOP"
#define RL_ERROR       ZXBC_NAMESPACE ".__ERROR"
#define RL_CHECK_BREAK ZXBC_NAMESPACE ".CHECK_BREAK"

/* S5.7c — local-array alloc / free RuntimeLabels (runtime/core.py:17-20,
 * 26,74; NAMESPACE == .core). Consumed by _larrd (alloc) and the
 * FunctionTranslator stdcall epilogue (MEM_FREE / ARRAYSTR_FREE_MEM). */
#define RL_ALLOC_LOCAL_ARRAY \
    ZXBC_NAMESPACE ".__ALLOC_LOCAL_ARRAY"
#define RL_ALLOC_LOCAL_ARRAY_WITH_BOUNDS \
    ZXBC_NAMESPACE ".__ALLOC_LOCAL_ARRAY_WITH_BOUNDS"
#define RL_ALLOC_INITIALIZED_LOCAL_ARRAY \
    ZXBC_NAMESPACE ".__ALLOC_INITIALIZED_LOCAL_ARRAY"
#define RL_ALLOC_INITIALIZED_LOCAL_ARRAY_WITH_BOUNDS \
    ZXBC_NAMESPACE ".__ALLOC_INITIALIZED_LOCAL_ARRAY_WITH_BOUNDS"
#define RL_MEM_FREE          ZXBC_NAMESPACE ".__MEM_FREE"
#define RL_ARRAYSTR_FREE_MEM ZXBC_NAMESPACE ".__ARRAYSTR_FREE_MEM"

/* Array element-access RuntimeLabels (runtime/core.py:24-25,58,118;
 * NAMESPACE == .core). __ARRAY computes the element address from the
 * indices pushed on the stack + the array base in HL; __ARRAY_PTR is the
 * indirect (pointer-to-array) variant. STR_ARRAYCOPY is NOT in the .core
 * namespace prefix in core.py — it is f"{NAMESPACE}.STR_ARRAYCOPY". */
#define RL_ARRAY      ZXBC_NAMESPACE ".__ARRAY"
#define RL_ARRAY_PTR  ZXBC_NAMESPACE ".__ARRAY_PTR"
#define RL_ILOADSTR   ZXBC_NAMESPACE ".__ILOADSTR"
#define RL_STR_ARRAYCOPY ZXBC_NAMESPACE ".STR_ARRAYCOPY"

/* S5.8bc — string RuntimeLabels (runtime/core.py:16,62-63,68,74,116-127;
 * NAMESPACE == .core). Spelling EXACT per core.py. */
#define RL_ADDSTR    ZXBC_NAMESPACE ".__ADDSTR"
#define RL_LOADSTR   ZXBC_NAMESPACE ".__LOADSTR"
#define RL_STORE_STR  ZXBC_NAMESPACE ".__STORE_STR"
#define RL_STORE_STR2 ZXBC_NAMESPACE ".__STORE_STR2"
#define RL_STREQ     ZXBC_NAMESPACE ".__STREQ"
#define RL_STRNE     ZXBC_NAMESPACE ".__STRNE"
#define RL_STRLT     ZXBC_NAMESPACE ".__STRLT"
#define RL_STRGT     ZXBC_NAMESPACE ".__STRGT"
#define RL_STRLE     ZXBC_NAMESPACE ".__STRLE"
#define RL_STRGE     ZXBC_NAMESPACE ".__STRGE"
#define RL_STRLEN    ZXBC_NAMESPACE ".__STRLEN"
#define RL_LETSUBSTR ZXBC_NAMESPACE ".__LETSUBSTR"
#define RL_STRSLICE  ZXBC_NAMESPACE ".__STRSLICE"

/* S5.8d — DATA/READ/RESTORE RuntimeLabels
 * (runtime/datarestore.py:12-13; NAMESPACE == .core). REQUIRED_MODULES
 * (datarestore.py:16-19) maps both to "read_restore.asm". */
#define RL_READ      ZXBC_NAMESPACE ".__READ"
#define RL_RESTORE   ZXBC_NAMESPACE ".__RESTORE"

/* S7.1b-i — PRINT-family RuntimeLabels + REQUIRED_MODULES, taken VERBATIM
 * from src/arch/z80/backend/runtime/io.py:11-59 (label string values) and
 * io.py:70-113 (REQUIRED_MODULES). NAMESPACE == .core (namespace.py:10 ->
 * global_.py:129 CORE_NAMESPACE). Numeric PRINT* labels mangle with a
 * double-underscore (e.g. .core.__PRINTU8); COPY_ATTR / PRINT_COMMA /
 * PRINT_EOL do not. */
#define RL_COPY_ATTR   ZXBC_NAMESPACE ".COPY_ATTR"
#define RL_PRINTI8     ZXBC_NAMESPACE ".__PRINTI8"
#define RL_PRINTU8     ZXBC_NAMESPACE ".__PRINTU8"
#define RL_PRINTI16    ZXBC_NAMESPACE ".__PRINTI16"
#define RL_PRINTU16    ZXBC_NAMESPACE ".__PRINTU16"
#define RL_PRINTI32    ZXBC_NAMESPACE ".__PRINTI32"
#define RL_PRINTU32    ZXBC_NAMESPACE ".__PRINTU32"
#define RL_PRINTF16    ZXBC_NAMESPACE ".__PRINTF16"
#define RL_PRINTF      ZXBC_NAMESPACE ".__PRINTF"
#define RL_PRINTSTR    ZXBC_NAMESPACE ".__PRINTSTR"
#define RL_PRINT_AT    ZXBC_NAMESPACE ".PRINT_AT"
#define RL_PRINT_COMMA ZXBC_NAMESPACE ".PRINT_COMMA"
#define RL_PRINT_EOL   ZXBC_NAMESPACE ".PRINT_EOL"
#define RL_PRINT_TAB   ZXBC_NAMESPACE ".PRINT_TAB"

/* S7.1b-iii — in-PRINT temporary-attr RuntimeLabels, VERBATIM from
 * io.py:25-32. NAMESPACE == .core; NO double-underscore mangle (same
 * convention as RL_PRINT_COMMA). */
#define RL_BOLD_TMP    ZXBC_NAMESPACE ".BOLD_TMP"
#define RL_BRIGHT_TMP  ZXBC_NAMESPACE ".BRIGHT_TMP"
#define RL_FLASH_TMP   ZXBC_NAMESPACE ".FLASH_TMP"
#define RL_INK_TMP     ZXBC_NAMESPACE ".INK_TMP"
#define RL_INVERSE_TMP ZXBC_NAMESPACE ".INVERSE_TMP"
#define RL_ITALIC_TMP  ZXBC_NAMESPACE ".ITALIC_TMP"
#define RL_OVER_TMP    ZXBC_NAMESPACE ".OVER_TMP"
#define RL_PAPER_TMP   ZXBC_NAMESPACE ".PAPER_TMP"

/* Simple statement RuntimeLabels (io.py:13,34 / misc.py:14 /
 * random.py:12-13). NAMESPACE == .core. CLS/BORDER have no
 * double-underscore; PAUSE does (misc.py:14 -> .core.__PAUSE). */
#define RL_CLS         ZXBC_NAMESPACE ".CLS"
#define RL_BORDER      ZXBC_NAMESPACE ".BORDER"

/* Drawing-primitive RuntimeLabels, VERBATIM from io.py:37-40. NAMESPACE
 * == .core; NO double-underscore mangle (same convention as CLS/BORDER). */
#define RL_CIRCLE      ZXBC_NAMESPACE ".CIRCLE"
#define RL_DRAW        ZXBC_NAMESPACE ".DRAW"
#define RL_DRAW3       ZXBC_NAMESPACE ".DRAW3"
#define RL_PLOT        ZXBC_NAMESPACE ".PLOT"
/* SAVE/LOAD/VERIFY RuntimeLabels (io.py:62-63). NAMESPACE == .core; NO
 * double-underscore mangle (same convention as CLS/BORDER). */
#define RL_LOAD_CODE   ZXBC_NAMESPACE ".LOAD_CODE"
#define RL_SAVE_CODE   ZXBC_NAMESPACE ".SAVE_CODE"
#define RL_PAUSE       ZXBC_NAMESPACE ".__PAUSE"
#define RL_RANDOMIZE   ZXBC_NAMESPACE ".RANDOMIZE"
#define RL_RND         ZXBC_NAMESPACE ".RND"

/* Standalone ATTR-sentence RuntimeLabels (io.py:16-23). NAMESPACE ==
 * .core; NO double-underscore (same convention as the _TMP set). */
#define RL_INK      ZXBC_NAMESPACE ".INK"
#define RL_PAPER    ZXBC_NAMESPACE ".PAPER"
#define RL_BRIGHT   ZXBC_NAMESPACE ".BRIGHT"
#define RL_FLASH    ZXBC_NAMESPACE ".FLASH"
#define RL_INVERSE  ZXBC_NAMESPACE ".INVERSE"
#define RL_OVER     ZXBC_NAMESPACE ".OVER"
#define RL_BOLD     ZXBC_NAMESPACE ".BOLD"
#define RL_ITALIC   ZXBC_NAMESPACE ".ITALIC"

/* BEEP RuntimeLabels (io.py:66-67). BEEP -> .core.BEEP (no mangle);
 * BEEPER -> .core.__BEEPER (double-underscore). */
#define RL_BEEP     ZXBC_NAMESPACE ".BEEP"
#define RL_BEEPER   ZXBC_NAMESPACE ".__BEEPER"

/* Math-function RuntimeLabels (math.py:12-29). Note ACS/ASN/SQR map to
 * ACOS/ASIN/SQRT label spellings; SGN* carry the double-underscore. */
#define RL_ACS    ZXBC_NAMESPACE ".ACOS"
#define RL_ASN    ZXBC_NAMESPACE ".ASIN"
#define RL_ATN    ZXBC_NAMESPACE ".ATAN"
#define RL_COS    ZXBC_NAMESPACE ".COS"
#define RL_SIN    ZXBC_NAMESPACE ".SIN"
#define RL_TAN    ZXBC_NAMESPACE ".TAN"
#define RL_EXP    ZXBC_NAMESPACE ".EXP"
#define RL_LN     ZXBC_NAMESPACE ".LN"
#define RL_SQR    ZXBC_NAMESPACE ".SQRT"
#define RL_SGNI8  ZXBC_NAMESPACE ".__SGNI8"
#define RL_SGNU8  ZXBC_NAMESPACE ".__SGNU8"
#define RL_SGNI16 ZXBC_NAMESPACE ".__SGNI16"
#define RL_SGNU16 ZXBC_NAMESPACE ".__SGNU16"
#define RL_SGNI32 ZXBC_NAMESPACE ".__SGNI32"
#define RL_SGNU32 ZXBC_NAMESPACE ".__SGNU32"
#define RL_SGNF16 ZXBC_NAMESPACE ".__SGNF16"
#define RL_SGNF   ZXBC_NAMESPACE ".__SGNF"

/* INKEY (io.py:43,94). */
#define RL_INKEY  ZXBC_NAMESPACE ".INKEY"

/* Builtin-function RuntimeLabels (misc.py:12-17 / core.py:61,119,133). */
#define RL_ASC      ZXBC_NAMESPACE ".__ASC"
#define RL_CHR      ZXBC_NAMESPACE ".CHR"
#define RL_USR      ZXBC_NAMESPACE ".USR"
#define RL_USR_STR  ZXBC_NAMESPACE ".USR_STR"
#define RL_VAL      ZXBC_NAMESPACE ".VAL"
#define RL_LBOUND   ZXBC_NAMESPACE ".__LBOUND"
#define RL_UBOUND   ZXBC_NAMESPACE ".__UBOUND"
#define RL_STR_FAST ZXBC_NAMESPACE ".__STR_FAST"

/* 8/16-bit unary runtime helpers (core.py:12-13,29). NEGHL (RL_NEGHL)
 * already defined above (neg16.asm). */
#define RL_ABS8    ZXBC_NAMESPACE ".__ABS8"
#define RL_ABS16   ZXBC_NAMESPACE ".__ABS16"
#define RL_BNOT16  ZXBC_NAMESPACE ".__BNOT16"

/* runtime_call (common.py:156-161): REQUIRES.add(LABEL_REQUIRED_MODULES
 * [label]) if present; returns "call {label}". The label->module map is
 * runtime/core.py:160-225 (only the S5.3-reachable labels). */
static const char *s_required_module(const char *label) {
    if (strcmp(label, RL_MUL8_FAST)  == 0) return "arith/mul8.asm";
    if (strcmp(label, RL_MUL16_FAST) == 0) return "arith/mul16.asm";
    if (strcmp(label, RL_DIVU8_FAST) == 0) return "arith/div8.asm";
    if (strcmp(label, RL_DIVI8_FAST) == 0) return "arith/div8.asm";
    if (strcmp(label, RL_DIVU16)     == 0) return "arith/div16.asm";
    if (strcmp(label, RL_DIVI16)     == 0) return "arith/div16.asm";
    /* S7.x — 8/16-bit mod + bitwise/logical modules (core.py:206-211,
     * 150,156,160,162,263-264 LABEL_REQUIRED_MODULES; verbatim). */
    if (strcmp(label, RL_MODU8_FAST) == 0) return "arith/div8.asm";
    if (strcmp(label, RL_MODI8_FAST) == 0) return "arith/div8.asm";
    if (strcmp(label, RL_MODU16)     == 0) return "arith/div16.asm";
    if (strcmp(label, RL_MODI16)     == 0) return "arith/div16.asm";
    if (strcmp(label, RL_XOR8)       == 0) return "bool/xor8.asm";
    if (strcmp(label, RL_XOR16)      == 0) return "bool/xor16.asm";
    if (strcmp(label, RL_AND16)      == 0) return "bool/and16.asm";
    if (strcmp(label, RL_BOR16)      == 0) return "bitwise/bor16.asm";
    if (strcmp(label, RL_BXOR16)     == 0) return "bitwise/bxor16.asm";
    if (strcmp(label, RL_BAND16)     == 0) return "bitwise/band16.asm";
    if (strcmp(label, RL_NEGHL)      == 0) return "neg16.asm";
    if (strcmp(label, RL_LOAD_DE_DE) == 0) return "lddede.asm";
    /* S5.4 wide/float module map (LABEL_REQUIRED_MODULES, live-verified). */
    if (strcmp(label, RL_ADDF)       == 0) return "arith/addf.asm";
    if (strcmp(label, RL_SUBF)       == 0) return "arith/subf.asm";
    if (strcmp(label, RL_MULF)       == 0) return "arith/mulf.asm";
    if (strcmp(label, RL_DIVF)       == 0) return "arith/divf.asm";
    if (strcmp(label, RL_MODF)       == 0) return "arith/modf.asm";
    if (strcmp(label, RL_POWF)       == 0) return "math/pow.asm";
    if (strcmp(label, RL_MULF16)     == 0) return "arith/mulf16.asm";
    if (strcmp(label, RL_DIVF16)     == 0) return "arith/divf16.asm";
    if (strcmp(label, RL_MODF16)     == 0) return "arith/modf16.asm";
    if (strcmp(label, RL_MUL32)      == 0) return "arith/mul32.asm";
    if (strcmp(label, RL_SUB32)      == 0) return "arith/sub32.asm";
    if (strcmp(label, RL_DIVU32)     == 0) return "arith/div32.asm";
    if (strcmp(label, RL_DIVI32)     == 0) return "arith/div32.asm";
    if (strcmp(label, RL_MODU32)     == 0) return "arith/div32.asm";
    if (strcmp(label, RL_MODI32)     == 0) return "arith/div32.asm";
    if (strcmp(label, RL_NEG32)      == 0) return "neg32.asm";
    if (strcmp(label, RL_ABS32)      == 0) return "abs32.asm";
    if (strcmp(label, RL_SWAP32)     == 0) return "swap32.asm";
    if (strcmp(label, RL_SHL32)      == 0) return "bitwise/shl32.asm";
    if (strcmp(label, RL_SHRL32)     == 0) return "bitwise/shrl32.asm";
    if (strcmp(label, RL_SHRA32)     == 0) return "bitwise/shra32.asm";
    if (strcmp(label, RL_EQ32)       == 0) return "cmp/eq32.asm";
    if (strcmp(label, RL_LTI32)      == 0) return "cmp/lti32.asm";
    if (strcmp(label, RL_LEI32)      == 0) return "cmp/lei32.asm";
    if (strcmp(label, RL_AND32)      == 0) return "bool/and32.asm";
    if (strcmp(label, RL_OR32)       == 0) return "bool/or32.asm";
    if (strcmp(label, RL_XOR32)      == 0) return "bool/xor32.asm";
    if (strcmp(label, RL_NOT32)      == 0) return "bool/not32.asm";
    if (strcmp(label, RL_BAND32)     == 0) return "bitwise/band32.asm";
    if (strcmp(label, RL_BOR32)      == 0) return "bitwise/bor32.asm";
    if (strcmp(label, RL_BXOR32)     == 0) return "bitwise/bxor32.asm";
    if (strcmp(label, RL_BNOT32)     == 0) return "bitwise/bnot32.asm";
    if (strcmp(label, RL_PLOADF)     == 0) return "ploadf.asm"; /* core.py:233 */
    if (strcmp(label, RL_PISTORE16)  == 0) return "istore16.asm"; /* core.py:228 */
    if (strcmp(label, RL_PISTORE32)  == 0) return "pistore32.asm"; /* core.py:229 */
    if (strcmp(label, RL_PISTORE_STR)  == 0) return "storestr.asm";  /* core.py:231 */
    if (strcmp(label, RL_PISTORE_STR2) == 0) return "storestr2.asm"; /* core.py:232 */
    if (strcmp(label, RL_PSTORE32)   == 0) return "pstore32.asm"; /* core.py:236 */
    if (strcmp(label, RL_PSTORE_STR)  == 0) return "pstorestr.asm";  /* core.py:237 */
    if (strcmp(label, RL_PSTORE_STR2) == 0) return "pstorestr2.asm"; /* core.py:238 */
    if (strcmp(label, RL_ILOAD32)    == 0) return "iload32.asm";
    if (strcmp(label, RL_STORE32)    == 0) return "store32.asm";
    if (strcmp(label, RL_ISTORE32)   == 0) return "store32.asm";
    if (strcmp(label, RL_LOADF)      == 0) return "iloadf.asm";
    if (strcmp(label, RL_ILOADF)     == 0) return "iloadf.asm";
    if (strcmp(label, RL_STOREF)     == 0) return "storef.asm";
    if (strcmp(label, RL_ISTOREF)    == 0) return "storef.asm";
    if (strcmp(label, RL_FP_PUSH_REV)== 0) return "pushf.asm";
    if (strcmp(label, RL_FTOU32REG)  == 0) return "ftou32reg.asm";
    if (strcmp(label, RL_FTOF16REG)  == 0) return "ftof16reg.asm";
    if (strcmp(label, RL_F16TOFREG)  == 0) return "f16tofreg.asm";
    if (strcmp(label, RL_I32TOFREG)  == 0) return "u32tofreg.asm";
    if (strcmp(label, RL_U32TOFREG)  == 0) return "u32tofreg.asm";
    if (strcmp(label, RL_I8TOFREG)   == 0) return "u32tofreg.asm";
    if (strcmp(label, RL_U8TOFREG)   == 0) return "u32tofreg.asm";
    if (strcmp(label, RL_GEF)        == 0) return "cmp/gef.asm";
    if (strcmp(label, RL_GTF)        == 0) return "cmp/gtf.asm";
    if (strcmp(label, RL_LEF)        == 0) return "cmp/lef.asm";
    if (strcmp(label, RL_LTF)        == 0) return "cmp/ltf.asm";
    if (strcmp(label, RL_EQF)        == 0) return "cmp/eqf.asm";
    if (strcmp(label, RL_NEF)        == 0) return "cmp/nef.asm";
    if (strcmp(label, RL_ORF)        == 0) return "bool/orf.asm";
    if (strcmp(label, RL_XORF)       == 0) return "bool/xorf.asm";
    if (strcmp(label, RL_ANDF)       == 0) return "bool/andf.asm";
    if (strcmp(label, RL_NOTF)       == 0) return "bool/notf.asm";
    if (strcmp(label, RL_NEGF)       == 0) return "negf.asm";
    /* S5.5 control-flow modules (runtime/labels.py map, live-verified). */
    if (strcmp(label, RL_ON_GOTO)    == 0) return "ongoto.asm";
    if (strcmp(label, RL_ON_GOSUB)   == 0) return "ongoto.asm";
    if (strcmp(label, RL_STOP)       == 0) return "error.asm";
    if (strcmp(label, RL_ERROR)      == 0) return "error.asm";
    if (strcmp(label, RL_CHECK_BREAK)== 0) return "break.asm";
    if (strcmp(label, RL_LTI8)       == 0) return "cmp/lti8.asm";
    if (strcmp(label, RL_LEI8)       == 0) return "cmp/lei8.asm";
    if (strcmp(label, RL_LTI16)      == 0) return "cmp/lti16.asm";
    if (strcmp(label, RL_LEI16)      == 0) return "cmp/lei16.asm";
    if (strcmp(label, RL_EQ16)       == 0) return "cmp/eq16.asm";
    /* S5.7c — local-array alloc/free modules (runtime/core.py:146-149,
     * 155,203 LABEL_REQUIRED_MODULES; live-verified). */
    if (strcmp(label, RL_ALLOC_LOCAL_ARRAY) == 0 ||
        strcmp(label, RL_ALLOC_LOCAL_ARRAY_WITH_BOUNDS) == 0 ||
        strcmp(label, RL_ALLOC_INITIALIZED_LOCAL_ARRAY) == 0 ||
        strcmp(label, RL_ALLOC_INITIALIZED_LOCAL_ARRAY_WITH_BOUNDS) == 0)
        return "array/arrayalloc.asm";
    if (strcmp(label, RL_ARRAYSTR_FREE_MEM) == 0) return "array/arraystrfree.asm";
    if (strcmp(label, RL_MEM_FREE)   == 0) return "mem/free.asm";
    /* Array element-access modules (runtime/core.py:153-154,179,247
     * REQUIRED_MODULES). array/array.asm self-#include-once's
     * arith/fmul16.asm + error.asm (resolved by the preprocessor). */
    if (strcmp(label, RL_ARRAY)      == 0) return "array/array.asm";
    if (strcmp(label, RL_ARRAY_PTR)  == 0) return "array/array.asm";
    if (strcmp(label, RL_ILOADSTR)   == 0) return "loadstr.asm";
    if (strcmp(label, RL_STR_ARRAYCOPY) == 0) return "array/strarraycpy.asm";
    /* S5.8bc — string modules (runtime/core.py:145,179,195,197-198,
     * 245-256 REQUIRED_MODULES; live-verified). */
    if (strcmp(label, RL_ADDSTR)     == 0) return "strcat.asm";
    if (strcmp(label, RL_LOADSTR)    == 0) return "loadstr.asm";
    if (strcmp(label, RL_STORE_STR)  == 0) return "storestr.asm";
    if (strcmp(label, RL_STORE_STR2) == 0) return "storestr2.asm";
    if (strcmp(label, RL_STREQ)      == 0) return "string.asm";
    if (strcmp(label, RL_STRNE)      == 0) return "string.asm";
    if (strcmp(label, RL_STRLT)      == 0) return "string.asm";
    if (strcmp(label, RL_STRGT)      == 0) return "string.asm";
    if (strcmp(label, RL_STRLE)      == 0) return "string.asm";
    if (strcmp(label, RL_STRGE)      == 0) return "string.asm";
    if (strcmp(label, RL_STRLEN)     == 0) return "strlen.asm";
    if (strcmp(label, RL_LETSUBSTR)  == 0) return "letsubstr.asm";
    if (strcmp(label, RL_STRSLICE)   == 0) return "strslice.asm";
    /* S5.8d — DATA/READ/RESTORE (datarestore.py:16-19). */
    if (strcmp(label, RL_READ)       == 0) return "read_restore.asm";
    if (strcmp(label, RL_RESTORE)    == 0) return "read_restore.asm";
    /* S7.1b-i — PRINT family (io.py:70-113 REQUIRED_MODULES, verbatim). */
    if (strcmp(label, RL_COPY_ATTR)  == 0) return "copy_attr.asm";
    if (strcmp(label, RL_PRINTI8)    == 0) return "printi8.asm";
    if (strcmp(label, RL_PRINTU8)    == 0) return "printu8.asm";
    if (strcmp(label, RL_PRINTI16)   == 0) return "printi16.asm";
    if (strcmp(label, RL_PRINTU16)   == 0) return "printu16.asm";
    if (strcmp(label, RL_PRINTI32)   == 0) return "printi32.asm";
    if (strcmp(label, RL_PRINTU32)   == 0) return "printu32.asm";
    if (strcmp(label, RL_PRINTF16)   == 0) return "printf16.asm";
    if (strcmp(label, RL_PRINTF)     == 0) return "printf.asm";
    if (strcmp(label, RL_PRINTSTR)   == 0) return "printstr.asm";
    if (strcmp(label, RL_PRINT_AT)   == 0) return "print.asm";
    if (strcmp(label, RL_PRINT_COMMA)== 0) return "print.asm";
    if (strcmp(label, RL_PRINT_EOL)  == 0) return "print.asm";
    if (strcmp(label, RL_PRINT_TAB)  == 0) return "print.asm";
    /* S7.1b-iii — in-PRINT temporary attrs (io.py:81-88 REQUIRED_MODULES,
     * verbatim). */
    if (strcmp(label, RL_BOLD_TMP)   == 0) return "bold.asm";
    if (strcmp(label, RL_BRIGHT_TMP) == 0) return "bright.asm";
    if (strcmp(label, RL_FLASH_TMP)  == 0) return "flash.asm";
    if (strcmp(label, RL_INK_TMP)    == 0) return "ink.asm";
    if (strcmp(label, RL_INVERSE_TMP)== 0) return "inverse.asm";
    if (strcmp(label, RL_ITALIC_TMP) == 0) return "italic.asm";
    if (strcmp(label, RL_OVER_TMP)   == 0) return "over.asm";
    if (strcmp(label, RL_PAPER_TMP)  == 0) return "paper.asm";
    /* Simple statements (io.py:71,89 / misc.py:23 / random.py:16). */
    if (strcmp(label, RL_CLS)        == 0) return "cls.asm";
    if (strcmp(label, RL_BORDER)     == 0) return "border.asm";
    /* Drawing primitives (io.py:90-93 REQUIRED_MODULES, verbatim). */
    if (strcmp(label, RL_CIRCLE)     == 0) return "circle.asm";
    if (strcmp(label, RL_DRAW)       == 0) return "draw.asm";
    if (strcmp(label, RL_DRAW3)      == 0) return "draw3.asm";
    if (strcmp(label, RL_PLOT)       == 0) return "plot.asm";
    /* SAVE/LOAD/VERIFY (io.py:95-96 REQUIRED_MODULES). */
    if (strcmp(label, RL_LOAD_CODE)  == 0) return "load.asm";
    if (strcmp(label, RL_SAVE_CODE)  == 0) return "save.asm";
    if (strcmp(label, RL_PAUSE)      == 0) return "pause.asm";
    if (strcmp(label, RL_RANDOMIZE)  == 0) return "random.asm";
    if (strcmp(label, RL_RND)        == 0) return "random.asm";
    /* Standalone ATTR sentences (io.py:76-80,73-75,78). */
    if (strcmp(label, RL_INK)        == 0) return "ink.asm";
    if (strcmp(label, RL_PAPER)      == 0) return "paper.asm";
    if (strcmp(label, RL_BRIGHT)     == 0) return "bright.asm";
    if (strcmp(label, RL_FLASH)      == 0) return "flash.asm";
    if (strcmp(label, RL_INVERSE)    == 0) return "inverse.asm";
    if (strcmp(label, RL_OVER)       == 0) return "over.asm";
    if (strcmp(label, RL_BOLD)       == 0) return "bold.asm";
    if (strcmp(label, RL_ITALIC)     == 0) return "italic.asm";
    /* BEEP (io.py:111-112). */
    if (strcmp(label, RL_BEEP)       == 0) return "io/sound/beep.asm";
    if (strcmp(label, RL_BEEPER)     == 0) return "io/sound/beeper.asm";
    /* Math functions (math.py:33-50). */
    if (strcmp(label, RL_ACS)        == 0) return "math/acos.asm";
    if (strcmp(label, RL_ASN)        == 0) return "math/asin.asm";
    if (strcmp(label, RL_ATN)        == 0) return "math/atan.asm";
    if (strcmp(label, RL_COS)        == 0) return "math/cos.asm";
    if (strcmp(label, RL_SIN)        == 0) return "math/sin.asm";
    if (strcmp(label, RL_TAN)        == 0) return "math/tan.asm";
    if (strcmp(label, RL_EXP)        == 0) return "math/exp.asm";
    if (strcmp(label, RL_LN)         == 0) return "math/logn.asm";
    if (strcmp(label, RL_SQR)        == 0) return "math/sqrt.asm";
    if (strcmp(label, RL_SGNI8)      == 0) return "sgni8.asm";
    if (strcmp(label, RL_SGNU8)      == 0) return "sgnu8.asm";
    if (strcmp(label, RL_SGNI16)     == 0) return "sgni16.asm";
    if (strcmp(label, RL_SGNU16)     == 0) return "sgnu16.asm";
    if (strcmp(label, RL_SGNI32)     == 0) return "sgni32.asm";
    if (strcmp(label, RL_SGNU32)     == 0) return "sgnu32.asm";
    if (strcmp(label, RL_SGNF16)     == 0) return "sgnf16.asm";
    if (strcmp(label, RL_SGNF)       == 0) return "sgnf.asm";
    if (strcmp(label, RL_INKEY)      == 0) return "io/keyboard/inkey.asm";
    /* 8/16-bit unary helpers (core.py:141-142,158). */
    if (strcmp(label, RL_ABS8)       == 0) return "abs8.asm";
    if (strcmp(label, RL_ABS16)      == 0) return "abs16.asm";
    if (strcmp(label, RL_BNOT16)     == 0) return "bitwise/bnot16.asm";
    /* Builtin functions (misc.py:21-26 / core.py:190,248,262). */
    if (strcmp(label, RL_ASC)        == 0) return "asc.asm";
    if (strcmp(label, RL_CHR)        == 0) return "chr.asm";
    if (strcmp(label, RL_USR)        == 0) return "usr.asm";
    if (strcmp(label, RL_USR_STR)    == 0) return "usr_str.asm";
    if (strcmp(label, RL_VAL)        == 0) return "val.asm";
    if (strcmp(label, RL_LBOUND)     == 0) return "array/arraybound.asm";
    if (strcmp(label, RL_UBOUND)     == 0) return "array/arraybound.asm";
    if (strcmp(label, RL_STR_FAST)   == 0) return "str.asm";
    return NULL;
}

/* S7.1b-i — translator-time REQUIRES registration. Python's
 * common.runtime_call (translator_visitor.py:119-123) does
 * backend.REQUIRES.add(LABEL_REQUIRED_MODULES[label]) AT TRANSLATOR TIME
 * (not at backend call-Quad emission — emit_call never calls
 * s_runtime_call). This is the faithful seam: the C tr_runtime_call calls
 * here so the label's asm module lands in b->requires_ exactly when the IC
 * `call` is emitted, mirroring the Python ordering and driving the heap /
 * #include-once branch. No-ops for labels with no module mapping (same as
 * Python's `if label in LABEL_REQUIRED_MODULES`). */
void backend_register_runtime_module(Backend *b, const char *label) {
    const char *mod = s_required_module(label);
    if (mod != NULL)
        hashmap_set(&b->requires_, mod, (void *)1);
}
static char *s_runtime_call(Backend *b, const char *label) {
    const char *mod = s_required_module(label);
    if (mod != NULL)
        hashmap_set(&b->requires_, mod, (void *)1);   /* common.REQUIRES.add */
    char buf[256];
    snprintf(buf, sizeof(buf), "call %s", label);
    return arena_strdup(b->arena, buf);
}

/* Bits8.get_oper (_8bit.py:25-125). 1st operand -> A, 2nd -> H. */
static StrVec bits8_get_oper(Backend *b, const char *op1, const char *op2,
                             bool reversed) {
    StrVec out = sv_new();
    const char *o1 = op1, *o2 = op2;
    if (o2 != NULL && reversed) { const char *t = o1; o1 = o2; o2 = t; }

    const char *op = o1;
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op)) {
        long opv = s_int_val(op);
        if (indirect)        sv_pushf(b, &out, "ld a, (%ld)", opv);
        else if (opv == 0)   sv_push(b, &out, "xor a");
        else                 sv_pushf(b, &out, "ld a, %d", s_int8(opv));
    } else {
        if (immediate) {
            if (indirect) sv_pushf(b, &out, "ld a, (%s)", op);
            else          sv_pushf(b, &out, "ld a, %s", op);
        } else if (op[0] == '_') {
            if (indirect) {
                const char *idx = reversed ? "bc" : "hl";
                sv_pushf(b, &out, "ld %s, (%s)", idx, op);
                sv_pushf(b, &out, "ld a, (%s)", idx);
            } else {
                sv_pushf(b, &out, "ld a, (%s)", op);
            }
        } else {
            if (immediate) {           /* (unreachable: immediate handled) */
                sv_pushf(b, &out, "ld a, %s", op);
            } else if (indirect) {
                const char *idx = reversed ? "bc" : "hl";
                sv_pushf(b, &out, "pop %s", idx);
                sv_pushf(b, &out, "ld a, (%s)", idx);
            } else {
                sv_push(b, &out, "pop af");
            }
        }
    }

    if (o2 == NULL) return out;

    StrVec tmp = sv_new();
    if (!reversed) { tmp = out; out = sv_new(); }

    op = o2;
    indirect = op[0] == '*';
    if (indirect) op++;
    immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op)) {
        long opv = s_int_val(op);
        if (indirect) sv_pushf(b, &out, "ld hl, (%ld - 1)", opv);
        else          sv_pushf(b, &out, "ld h, %d", s_int8(opv));
    } else {
        if (immediate) {
            if (indirect) {
                sv_pushf(b, &out, "ld hl, %s", op);
                sv_push(b, &out, "ld h, (hl)");
            } else {
                sv_pushf(b, &out, "ld h, %s", op);
            }
        } else if (op[0] == '_') {
            if (indirect) {
                sv_pushf(b, &out, "ld hl, (%s)", op);
                sv_push(b, &out, "ld h, (hl)");
            } else {
                sv_pushf(b, &out, "ld hl, (%s - 1)", op);
            }
        } else {
            sv_push(b, &out, "pop hl");
        }
        if (indirect) sv_push(b, &out, "ld h, (hl)");
    }

    if (!reversed) {
        for (int i = 0; i < tmp.len; i++) vec_push(out, tmp.data[i]);
        vec_free(tmp);
    }
    return out;
}

/* ---- Bits8.add8 (_8bit.py:127-174) ----------------------------------- */
static StrVec emit_add8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0) { sv_push(b, &out, "push af"); return out; }
        int o2 = s_int8(iv);
        if (o2 == 1)    { sv_push(b, &out, "inc a"); sv_push(b, &out, "push af"); return out; }
        if (o2 == 0xFF) { sv_push(b, &out, "dec a"); sv_push(b, &out, "push af"); return out; }
        sv_pushf(b, &out, "add a, %d", s_int8(o2));
        sv_push(b, &out, "push af");
        return out;
    }
    if (op2[0] == '_') { const char *t = op1; op1 = op2; op2 = t; }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    sv_push(b, &out, "add a, h");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.sub8 (_8bit.py:176-242) ----------------------------------- */
static StrVec emit_sub8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        StrVec out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 0)    { sv_push(b, &out, "push af"); return out; }
        if (o2 == 1)    { sv_push(b, &out, "dec a"); sv_push(b, &out, "push af"); return out; }
        if (o2 == 0xFF) { sv_push(b, &out, "inc a"); sv_push(b, &out, "push af"); return out; }
        sv_pushf(b, &out, "sub %d", o2);
        sv_push(b, &out, "push af");
        return out;
    }
    if (s_is_int(op1) && s_int8(s_int_val(op1)) == 0) {
        StrVec out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "neg");
        sv_push(b, &out, "push af");
        return out;
    }
    bool rev = false;
    if (op2[0] == '_') { rev = true; const char *t = op1; op1 = op2; op2 = t; }
    StrVec out = bits8_get_oper(b, op1, op2, rev);
    sv_push(b, &out, "sub h");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.mul8 (_8bit.py:244-291) ----------------------------------- */
static StrVec emit_mul8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    StrVec out;
    if (s_int_ops(op1, op2, &other, &iv)) {
        out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0) { sv_push(b, &out, "xor a"); sv_push(b, &out, "push af"); return out; }
        if (iv == 1) { sv_push(b, &out, "push af"); return out; }
        if (iv == 2) { sv_push(b, &out, "add a, a"); sv_push(b, &out, "push af"); return out; }
        if (iv == 4) { sv_push(b, &out, "add a, a"); sv_push(b, &out, "add a, a"); sv_push(b, &out, "push af"); return out; }
        sv_pushf(b, &out, "ld h, %d", s_int8(iv));
    } else {
        if (op2[0] == '_') { const char *t = op1; op1 = op2; op2 = t; }
        out = bits8_get_oper(b, op1, op2, false);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MUL8_FAST));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.divu8 (_8bit.py:293-336) ---------------------------------- */
static StrVec emit_divu8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 1) { sv_push(b, &out, "push af"); return out; }
        if (o2 == 2) { sv_push(b, &out, "srl a"); sv_push(b, &out, "push af"); return out; }
        sv_pushf(b, &out, "ld h, %d", o2);
    } else {
        bool rev = false;
        if (op2[0] == '_') {
            if (s_is_int(op1) && s_int_val(op1) == 0) {
                out = sv_new();
                sv_push(b, &out, "xor a");
                sv_push(b, &out, "push af");
                return out;
            }
            rev = true; const char *t = op1; op1 = op2; op2 = t;
        }
        out = bits8_get_oper(b, op1, op2, rev);
    }
    sv_push(b, &out, s_runtime_call(b, RL_DIVU8_FAST));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.divi8 (_8bit.py:338-386) ---------------------------------- */
static StrVec emit_divi8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op2)) {
        long o2 = s_int_val(op2) & 0xFF;          /* int(op2) & 0xFF */
        out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 1)  { sv_push(b, &out, "push af"); return out; }
        if (o2 == -1) { sv_push(b, &out, "neg"); sv_push(b, &out, "push af"); return out; }
        if (o2 == 2)  { sv_push(b, &out, "sra a"); sv_push(b, &out, "push af"); return out; }
        sv_pushf(b, &out, "ld h, %d", s_int8(o2));
    } else {
        bool rev = false;
        if (op2[0] == '_') {
            if (s_is_int(op1) && s_int_val(op1) == 0) {
                out = sv_new();
                sv_push(b, &out, "xor a");
                sv_push(b, &out, "push af");
                return out;
            }
            rev = true; const char *t = op1; op1 = op2; op2 = t;
        }
        out = bits8_get_oper(b, op1, op2, rev);
    }
    sv_push(b, &out, s_runtime_call(b, RL_DIVI8_FAST));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.modu8 (_8bit.py:388-435) ---------------------------------- */
static StrVec emit_modu8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 1) {
            if (op1[0] == '_') { vec_free(out); out = sv_new(); }
            sv_push(b, &out, "xor a");
            sv_push(b, &out, "push af");
            return out;
        }
        if (s_is_2n(o2)) {
            sv_pushf(b, &out, "and %d", o2 - 1);
            sv_push(b, &out, "push af");
            return out;
        }
        sv_pushf(b, &out, "ld h, %d", s_int8(o2));
    } else {
        bool rev;
        if (op2[0] == '_') {
            if (s_is_int(op1) && s_int_val(op1) == 0) {
                out = sv_new();
                sv_push(b, &out, "xor a");
                sv_push(b, &out, "push af");
                return out;
            }
            rev = true; const char *t = op1; op1 = op2; op2 = t;
        } else {
            rev = false;
        }
        out = bits8_get_oper(b, op1, op2, rev);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MODU8_FAST));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.modi8 (_8bit.py:437-484) ---------------------------------- */
static StrVec emit_modi8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 1) {
            if (op1[0] == '_') { vec_free(out); out = sv_new(); }
            sv_push(b, &out, "xor a");
            sv_push(b, &out, "push af");
            return out;
        }
        if (s_is_2n(o2)) {
            sv_pushf(b, &out, "and %d", o2 - 1);
            sv_push(b, &out, "push af");
            return out;
        }
        sv_pushf(b, &out, "ld h, %d", s_int8(o2));
    } else {
        bool rev;
        if (op2[0] == '_') {
            if (s_is_int(op1) && s_int_val(op1) == 0) {
                out = sv_new();
                sv_push(b, &out, "xor a");
                sv_push(b, &out, "push af");
                return out;
            }
            rev = true; const char *t = op1; op1 = op2; op2 = t;
        } else {
            rev = false;
        }
        out = bits8_get_oper(b, op1, op2, rev);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MODI8_FAST));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.or8 (_8bit.py:664-690) ------------------------------------ */
static StrVec emit_or8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* X or False = X */
            sv_push(b, &out, "push af");
            return out;
        }
        sv_push(b, &out, "ld a, 1");  /* X or True = True */
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    sv_push(b, &out, "or h");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.bor8 (_8bit.py:692-720) ----------------------------------- */
static StrVec emit_bor8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* X | 0 = X */
            sv_push(b, &out, "push af");
            return out;
        }
        if (iv == 0xFF) {  /* X | 0xFF = 0xFF */
            sv_push(b, &out, "ld a, 0FFh");
            sv_push(b, &out, "push af");
            return out;
        }
        vec_free(out);
        op1 = q->args[1]; op2 = q->args[2];  /* op1, op2 = tuple(ins[2:]) */
    }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    sv_push(b, &out, "or h");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.and8 (_8bit.py:722-753) ----------------------------------- */
static StrVec emit_and8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv != 0) {  /* X and True = X */
            sv_push(b, &out, "push af");
            return out;
        }
        sv_push(b, &out, "xor a");  /* False and X = False */
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    char *lbl = s_tmp_label(b);
    sv_push(b, &out, "or a");
    sv_pushf(b, &out, "jr z, %s", lbl);
    sv_push(b, &out, "ld a, h");
    sv_pushf(b, &out, "%s:", lbl);
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.band8 (_8bit.py:755-783) ---------------------------------- */
static StrVec emit_band8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0xFF) {  /* X & 0xFF = X */
            sv_push(b, &out, "push af");
            return out;
        }
        if (iv == 0) {  /* X and 0 = 0 */
            sv_push(b, &out, "xor a");
            sv_push(b, &out, "push af");
            return out;
        }
        vec_free(out);
        op1 = q->args[1]; op2 = q->args[2];  /* op1, op2 = tuple(ins[2:]) */
    }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    sv_push(b, &out, "and h");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.xor8 (_8bit.py:785-811) ----------------------------------- */
static StrVec emit_xor8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* False xor X = X */
            sv_push(b, &out, "push af");
            return out;
        }
        sv_push(b, &out, "sub 1");
        sv_push(b, &out, "sbc a, a");
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    sv_push(b, &out, s_runtime_call(b, RL_XOR8));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.bxor8 (_8bit.py:813-841) ---------------------------------- */
static StrVec emit_bxor8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits8_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* 0 xor X = X */
            sv_push(b, &out, "push af");
            return out;
        }
        if (iv == 0xFF) {  /* X xor 0xFF = ~X */
            sv_push(b, &out, "cpl");
            sv_push(b, &out, "push af");
            return out;
        }
        vec_free(out);
        op1 = q->args[1]; op2 = q->args[2];  /* op1, op2 = tuple(ins[2:]) */
    }
    StrVec out = bits8_get_oper(b, op1, op2, false);
    sv_push(b, &out, "xor h");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.shru8 (_8bit.py:879-931) ---------------------------------- */
static StrVec emit_shru8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        StrVec out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 0) { sv_push(b, &out, "push af"); return out; }
        if (o2 < 4) {
            for (int i = 0; i < o2; i++) sv_push(b, &out, "srl a");
            sv_push(b, &out, "push af");
            return out;
        }
        char *label = s_tmp_label(b);
        sv_pushf(b, &out, "ld b, %d", s_int8(o2));
        sv_pushf(b, &out, "%s:", label);
        sv_push(b, &out, "srl a");
        sv_pushf(b, &out, "djnz %s", label);
        sv_push(b, &out, "push af");
        return out;
    }
    if (s_is_int(op1) && s_int_val(op1) == 0) {
        StrVec out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "xor a");
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits8_get_oper(b, op1, op2, true);
    char *label = s_tmp_label(b);
    char *label2 = s_tmp_label(b);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "ld b, a");
    sv_push(b, &out, "ld a, h");
    sv_pushf(b, &out, "jr z, %s", label2);
    sv_pushf(b, &out, "%s:", label);
    sv_push(b, &out, "srl a");
    sv_pushf(b, &out, "djnz %s", label);
    sv_pushf(b, &out, "%s:", label2);
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.shri8 (_8bit.py:933-985) ---------------------------------- */
static StrVec emit_shri8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        StrVec out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 0) { sv_push(b, &out, "push af"); return out; }
        if (o2 < 4) {
            for (int i = 0; i < o2; i++) sv_push(b, &out, "sra a");
            sv_push(b, &out, "push af");
            return out;
        }
        char *label = s_tmp_label(b);
        sv_pushf(b, &out, "ld b, %d", s_int8(o2));
        sv_pushf(b, &out, "%s:", label);
        sv_push(b, &out, "sra a");
        sv_pushf(b, &out, "djnz %s", label);
        sv_push(b, &out, "push af");
        return out;
    }
    if (s_is_int(op1) && s_int_val(op1) == 0) {
        StrVec out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "xor a");
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits8_get_oper(b, op1, op2, true);
    char *label = s_tmp_label(b);
    char *label2 = s_tmp_label(b);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "ld b, a");
    sv_push(b, &out, "ld a, h");
    sv_pushf(b, &out, "jr z, %s", label2);
    sv_pushf(b, &out, "%s:", label);
    sv_push(b, &out, "sra a");
    sv_pushf(b, &out, "djnz %s", label);
    sv_pushf(b, &out, "%s:", label2);
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.shl8 (_8bit.py:987-1038) ---------------------------------- */
static StrVec emit_shl8(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    if (s_is_int(op2)) {
        int o2 = s_int8(s_int_val(op2));
        StrVec out = bits8_get_oper(b, op1, NULL, false);
        if (o2 == 0) { sv_push(b, &out, "push af"); return out; }
        if (o2 < 6) {
            for (int i = 0; i < o2; i++) sv_push(b, &out, "add a, a");
            sv_push(b, &out, "push af");
            return out;
        }
        char *label = s_tmp_label(b);
        sv_pushf(b, &out, "ld b, %d", s_int8(o2));
        sv_pushf(b, &out, "%s:", label);
        sv_push(b, &out, "add a, a");
        sv_pushf(b, &out, "djnz %s", label);
        sv_push(b, &out, "push af");
        return out;
    }
    if (s_is_int(op1) && s_int_val(op1) == 0) {
        StrVec out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "xor a");
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits8_get_oper(b, op1, op2, true);
    char *label = s_tmp_label(b);
    char *label2 = s_tmp_label(b);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "ld b, a");
    sv_push(b, &out, "ld a, h");
    sv_pushf(b, &out, "jr z, %s", label2);
    sv_pushf(b, &out, "%s:", label);
    sv_push(b, &out, "add a, a");
    sv_pushf(b, &out, "djnz %s", label);
    sv_pushf(b, &out, "%s:", label2);
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.load8 (_8bit.py:1040-1048) -------------------------------- */
static StrVec emit_load8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q->args[1], NULL, false);  /* ins[2] */
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits8.store8 (_8bit.py:1050-1100) ------------------------------- */
static StrVec emit_store8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q->args[1], NULL, false);  /* ins[2] */
    const char *op = q->args[0];                              /* ins[1] */
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op) || op[0] == '_' || op[0] == '.') {
        char ibuf[32];
        if (s_is_int(op)) {
            snprintf(ibuf, sizeof(ibuf), "%ld", s_int_val(op) & 0xFFFF);
            op = ibuf;
        }
        if (immediate) {
            sv_pushf(b, &out, "ld (%s), a", op);
        } else if (indirect) {
            sv_pushf(b, &out, "ld hl, (%s)", op);
            sv_push(b, &out, "ld (hl), a");
        } else {
            sv_pushf(b, &out, "ld (%s), a", op);
        }
    } else {
        if (immediate) {
            if (indirect) {
                sv_pushf(b, &out, "ld hl, (%s)", op);
                sv_push(b, &out, "ld (hl), a");
            } else {
                sv_pushf(b, &out, "ld (%s), a", op);
            }
            return out;
        }
        sv_push(b, &out, "pop hl");
        if (indirect) {
            sv_push(b, &out, "ld e, (hl)");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld d, (hl)");
            sv_push(b, &out, "ld (de), a");
        } else {
            sv_push(b, &out, "ld (hl), a");
        }
    }
    return out;
}

/* ---- Bits16.add16 (_16bit.py:122-169) -------------------------------- */
static StrVec emit_add16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        int o2 = s_int16(iv);
        StrVec out = bits16_get_oper(b, other, NULL, false);
        if (o2 == 0) { sv_push(b, &out, "push hl"); return out; }
        if (o2 < 4)  { for (int i = 0; i < o2; i++) sv_push(b, &out, "inc hl");
                       sv_push(b, &out, "push hl"); return out; }
        if (o2 > 65531) { int n = 0x10000 - o2;
                          for (int i = 0; i < n; i++) sv_push(b, &out, "dec hl");
                          sv_push(b, &out, "push hl"); return out; }
        sv_pushf(b, &out, "ld de, %d", o2);
        sv_push(b, &out, "add hl, de");
        sv_push(b, &out, "push hl");
        return out;
    }
    if (op2[0] == '_') { const char *t = op1; op1 = op2; op2 = t; }
    StrVec out = bits16_get_oper(b, op1, op2, false);
    sv_push(b, &out, "add hl, de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.sub16 (_16bit.py:171-222) -------------------------------- */
static StrVec emit_sub16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    if (s_is_int(op2)) {
        int o = s_int16(s_int_val(op2));
        StrVec out = bits16_get_oper(b, op1, NULL, false);
        if (o == 0) { sv_push(b, &out, "push hl"); return out; }
        if (o < 4)  { for (int i = 0; i < o; i++) sv_push(b, &out, "dec hl");
                      sv_push(b, &out, "push hl"); return out; }
        if (o > 65531) { int n = 0x10000 - o;
                         for (int i = 0; i < n; i++) sv_push(b, &out, "inc hl");
                         sv_push(b, &out, "push hl"); return out; }
        sv_pushf(b, &out, "ld de, -%d", o);
        sv_push(b, &out, "add hl, de");
        sv_push(b, &out, "push hl");
        return out;
    }
    bool rev = false;
    if (op2[0] == '_') { rev = true; const char *t = op1; op1 = op2; op2 = t; }
    StrVec out = bits16_get_oper(b, op1, op2, rev);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.mul16 (_16bit.py:224-273) -------------------------------- */
static StrVec emit_mul16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    StrVec out;
    if (s_int_ops(op1, op2, &other, &iv)) {
        out = bits16_get_oper(b, other, NULL, false);
        if (iv == 0) {
            if (other[0] == '_' || other[0] == '$') { vec_free(out); out = sv_new(); }
            sv_push(b, &out, "ld hl, 0");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (iv == 1) { sv_push(b, &out, "push hl"); return out; }
        if (iv == 0xFFFF) {
            sv_push(b, &out, s_runtime_call(b, RL_NEGHL));
            sv_push(b, &out, "push hl");
            return out;
        }
        if (s_is_2n(iv) && s_log2(iv) < 4) {
            int k = s_log2(iv);
            for (int i = 0; i < k; i++) sv_push(b, &out, "add hl, hl");
            sv_push(b, &out, "push hl");
            return out;
        }
        sv_pushf(b, &out, "ld de, %ld", iv);
    } else {
        if (op2[0] == '_') { const char *t = op1; op1 = op2; op2 = t; }
        out = bits16_get_oper(b, op1, op2, false);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MUL16_FAST));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.divu16 (_16bit.py:275-329) ------------------------------- */
static StrVec emit_divu16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op1) && s_int_val(op1) == 0) {
        if (op2[0] == '_' || op2[0] == '$') out = sv_new();
        else out = bits16_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "ld hl, 0");
        sv_push(b, &out, "push hl");
        return out;
    }
    if (s_is_int(op2)) {
        long o = s_int16(s_int_val(op2));
        out = bits16_get_oper(b, op1, NULL, false);
        if (s_int_val(op2) == 0) {                    /* op2 == 0 */
            if (op1[0] == '_' || op1[0] == '$') { vec_free(out); out = sv_new(); }
            sv_push(b, &out, "ld hl, 0");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (o == 1) { sv_push(b, &out, "push hl"); return out; }
        if (o == 2) { sv_push(b, &out, "srl h"); sv_push(b, &out, "rr l");
                      sv_push(b, &out, "push hl"); return out; }
        sv_pushf(b, &out, "ld de, %ld", o);
    } else {
        bool rev = false;
        if (op2[0] == '_') { rev = true; const char *t = op1; op1 = op2; op2 = t; }
        out = bits16_get_oper(b, op1, op2, rev);
    }
    sv_push(b, &out, s_runtime_call(b, RL_DIVU16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.divi16 (_16bit.py:331-386) ------------------------------- */
static StrVec emit_divi16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op1) && s_int_val(op1) == 0) {
        if (op2[0] == '_' || op2[0] == '$') out = sv_new();
        else out = bits16_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "ld hl, 0");
        sv_push(b, &out, "push hl");
        return out;
    }
    if (s_is_int(op2)) {
        long o = s_int16(s_int_val(op2));
        out = bits16_get_oper(b, op1, NULL, false);
        if (o == 1)  { sv_push(b, &out, "push hl"); return out; }
        if (o == -1) { sv_push(b, &out, s_runtime_call(b, RL_NEGHL));
                       sv_push(b, &out, "push hl"); return out; }
        if (o == 2)  { sv_push(b, &out, "sra h"); sv_push(b, &out, "rr l");
                       sv_push(b, &out, "push hl"); return out; }
        sv_pushf(b, &out, "ld de, %ld", o);
    } else {
        bool rev = false;
        if (op2[0] == '_') { rev = true; const char *t = op1; op1 = op2; op2 = t; }
        out = bits16_get_oper(b, op1, op2, rev);
    }
    sv_push(b, &out, s_runtime_call(b, RL_DIVI16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.modu16 (_16bit.py:388-431) ------------------------------- */
static StrVec emit_modu16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op2)) {
        long o2 = s_int16(s_int_val(op2));
        out = bits16_get_oper(b, op1, NULL, false);
        if (o2 == 1) {
            /* Faithful port of Python's `if op2[0] in ("_", "$")`:
             * op2 has been reassigned to int (cls.int16), so op2[0]
             * raises TypeError in CPython — this branch never produces
             * output (Python aborts). Mirror by not emitting here. */
            sv_push(b, &out, "ld hl, 0");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (s_is_2n(o2)) {
            long k = o2 - 1;
            if (o2 > 255) {  /* only affects H */
                sv_push(b, &out, "ld a, h");
                sv_pushf(b, &out, "and %ld", k >> 8);
                sv_push(b, &out, "ld h, a");
            } else {
                sv_push(b, &out, "ld h, 0");  /* High part goes 0 */
                sv_push(b, &out, "ld a, l");
                sv_pushf(b, &out, "and %ld", k % 0xFF);
                sv_push(b, &out, "ld l, a");
            }
            sv_push(b, &out, "push hl");
            return out;
        }
        sv_pushf(b, &out, "ld de, %ld", o2);
    } else {
        out = bits16_get_oper(b, op1, op2, false);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MODU16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.modi16 (_16bit.py:433-476) ------------------------------- */
static StrVec emit_modi16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    StrVec out;
    if (s_is_int(op2)) {
        long o2 = s_int16(s_int_val(op2));
        out = bits16_get_oper(b, op1, NULL, false);
        if (o2 == 1) {
            if (strcmp(op1, "_") == 0 || strcmp(op1, "$") == 0) {
                vec_free(out); out = sv_new();  /* discard previous op */
            }
            sv_push(b, &out, "ld hl, 0");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (s_is_2n(o2)) {
            long k = o2 - 1;
            if (o2 > 255) {  /* only affects H */
                sv_push(b, &out, "ld a, h");
                sv_pushf(b, &out, "and %ld", k >> 8);
                sv_push(b, &out, "ld h, a");
            } else {
                sv_push(b, &out, "ld h, 0");  /* High part goes 0 */
                sv_push(b, &out, "ld a, l");
                sv_pushf(b, &out, "and %ld", k % 0xFF);
                sv_push(b, &out, "ld l, a");
            }
            sv_push(b, &out, "push hl");
            return out;
        }
        sv_pushf(b, &out, "ld de, %ld", o2);
    } else {
        out = bits16_get_oper(b, op1, op2, false);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MODI16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.or16 (_16bit.py:623-659) --------------------------------- */
static StrVec emit_or16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        if (iv == 0) {
            StrVec out = bits16_get_oper(b, other, NULL, false);
            sv_push(b, &out, "ld a, h");
            sv_push(b, &out, "or l");  /* Convert x to Boolean */
            sv_push(b, &out, "push af");
            return out;  /* X or False = X */
        }
        StrVec out = bits16_get_oper(b, other, NULL, false);
        sv_push(b, &out, "ld a, 0FFh");  /* X or True = True */
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits16_get_oper(b, q->args[1], q->args[2], false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or d");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits16.bor16 (_16bit.py:661-692) -------------------------------- *
 * Faithful to Python: after a non-special _int_ops match, op1/op2 are the
 * swapped (other, int) pair and get_oper(op1, op2) is called on them (the
 * int via str()). For the no-match path op1/op2 stay as ins[2]/ins[3]. */
static StrVec emit_bor16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits16_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* X | 0 = X */
            sv_push(b, &out, "push hl");
            return out;
        }
        if (iv == 0xFFFF) {  /* X & 0xFFFF = 0xFFFF */
            sv_push(b, &out, "ld hl, 0FFFFh");
            sv_push(b, &out, "push hl");
            return out;
        }
        vec_free(out);
        char ibuf[24];
        snprintf(ibuf, sizeof(ibuf), "%ld", iv);
        op1 = other; op2 = arena_strdup(b->arena, ibuf);
    }
    StrVec out = bits16_get_oper(b, op1, op2, false);
    sv_push(b, &out, s_runtime_call(b, RL_BOR16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.xor16 (_16bit.py:694-730) -------------------------------- */
static StrVec emit_xor16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits16_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* X xor False = X */
            sv_push(b, &out, "ld a, h");
            sv_push(b, &out, "or l");
            sv_push(b, &out, "push af");
            return out;
        }
        sv_push(b, &out, "ld a, h");  /* X xor True = NOT X */
        sv_push(b, &out, "or l");
        sv_push(b, &out, "sub 1");
        sv_push(b, &out, "sbc a, a");
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits16_get_oper(b, q->args[1], q->args[2], false);
    sv_push(b, &out, s_runtime_call(b, RL_XOR16));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits16.bxor16 (_16bit.py:732-763) ------------------------------- */
static StrVec emit_bxor16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits16_get_oper(b, other, NULL, false);
        if (iv == 0) {  /* X ^ 0 = X */
            sv_push(b, &out, "push hl");
            return out;
        }
        if (iv == 0xFFFF) {  /* X ^ 0xFFFF = bNOT X */
            sv_push(b, &out, s_runtime_call(b, RL_NEGHL));
            sv_push(b, &out, "push hl");
            return out;
        }
        vec_free(out);
        char ibuf[24];
        snprintf(ibuf, sizeof(ibuf), "%ld", iv);
        op1 = other; op2 = arena_strdup(b->arena, ibuf);
    }
    StrVec out = bits16_get_oper(b, op1, op2, false);
    sv_push(b, &out, s_runtime_call(b, RL_BXOR16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.and16 (_16bit.py:765-798) -------------------------------- */
static StrVec emit_and16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        StrVec out = bits16_get_oper(b, other, NULL, false);
        if (iv != 0) {
            sv_push(b, &out, "ld a, h");
            sv_push(b, &out, "or l");
            sv_push(b, &out, "push af");
            return out;  /* X and True = X */
        }
        vec_free(out);
        out = bits16_get_oper(b, other, NULL, false);
        sv_push(b, &out, "xor a");  /* X and False = False */
        sv_push(b, &out, "push af");
        return out;
    }
    StrVec out = bits16_get_oper(b, op1, op2, false);
    sv_push(b, &out, s_runtime_call(b, RL_AND16));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits16.band16 (_16bit.py:800-830) ------------------------------- */
static StrVec emit_band16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        if (iv == 0xFFFF) {  /* X & 0xFFFF = X */
            return sv_new();
        }
        if (iv == 0) {  /* X & 0 = 0 */
            StrVec out = bits16_get_oper(b, other, NULL, false);
            sv_push(b, &out, "ld hl, 0");
            sv_push(b, &out, "push hl");
            return out;
        }
        char ibuf[24];
        snprintf(ibuf, sizeof(ibuf), "%ld", iv);
        op1 = other; op2 = arena_strdup(b->arena, ibuf);
    }
    StrVec out = bits16_get_oper(b, op1, op2, false);
    sv_push(b, &out, s_runtime_call(b, RL_BAND16));
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.shru16 (_16bit.py:867-909) ------------------------------- */
static StrVec emit_shru16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    char *label = s_tmp_label(b);
    char *label2 = s_tmp_label(b);
    StrVec out;
    if (s_is_int(op2)) {
        long op = s_int16(s_int_val(op2));
        if (op == 0) return sv_new();
        out = bits16_get_oper(b, op1, NULL, false);
        if (op == 1) {
            sv_push(b, &out, "srl h");
            sv_push(b, &out, "rr l");
            sv_push(b, &out, "push hl");
            return out;
        }
        sv_pushf(b, &out, "ld b, %ld", op);
    } else {
        out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "ld b, a");
        StrVec p = bits16_get_oper(b, op1, NULL, false);
        for (int i = 0; i < p.len; i++) vec_push(out, p.data[i]);
        vec_free(p);
        sv_push(b, &out, "or a");
        sv_pushf(b, &out, "jr z, %s", label2);
    }
    sv_pushf(b, &out, "%s:", label);
    sv_push(b, &out, "srl h");
    sv_push(b, &out, "rr l");
    sv_pushf(b, &out, "djnz %s", label);
    sv_pushf(b, &out, "%s:", label2);
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.shri16 (_16bit.py:911-953) ------------------------------- */
static StrVec emit_shri16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    char *label = s_tmp_label(b);
    char *label2 = s_tmp_label(b);
    StrVec out;
    if (s_is_int(op2)) {
        long op = s_int16(s_int_val(op2));
        if (op == 0) return sv_new();
        out = bits16_get_oper(b, op1, NULL, false);
        if (op == 1) {
            sv_push(b, &out, "srl h");
            sv_push(b, &out, "rr l");
            sv_push(b, &out, "push hl");
            return out;
        }
        sv_pushf(b, &out, "ld b, %ld", op);
    } else {
        out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "ld b, a");
        StrVec p = bits16_get_oper(b, op1, NULL, false);
        for (int i = 0; i < p.len; i++) vec_push(out, p.data[i]);
        vec_free(p);
        sv_push(b, &out, "or a");
        sv_pushf(b, &out, "jr z, %s", label2);
    }
    sv_pushf(b, &out, "%s:", label);
    sv_push(b, &out, "sra h");
    sv_push(b, &out, "rr l");
    sv_pushf(b, &out, "djnz %s", label);
    sv_pushf(b, &out, "%s:", label2);
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.shl16 (_16bit.py:955-995) -------------------------------- */
static StrVec emit_shl16(Backend *b, Quad *q) {
    const char *op1 = q->args[1], *op2 = q->args[2]; /* ins[2],ins[3] */
    char *label = s_tmp_label(b);
    char *label2 = s_tmp_label(b);
    StrVec out;
    if (s_is_int(op2)) {
        long op = s_int16(s_int_val(op2));
        if (op == 0) return sv_new();
        out = bits16_get_oper(b, op1, NULL, false);
        if (op < 6) {
            for (long i = 0; i < op; i++) sv_push(b, &out, "add hl, hl");
            sv_push(b, &out, "push hl");
            return out;
        }
        sv_pushf(b, &out, "ld b, %ld", op);
    } else {
        out = bits8_get_oper(b, op2, NULL, false);
        sv_push(b, &out, "ld b, a");
        StrVec p = bits16_get_oper(b, op1, NULL, false);
        for (int i = 0; i < p.len; i++) vec_push(out, p.data[i]);
        vec_free(p);
        sv_push(b, &out, "or a");
        sv_pushf(b, &out, "jr z, %s", label2);
    }
    sv_pushf(b, &out, "%s:", label);
    sv_push(b, &out, "add hl, hl");
    sv_pushf(b, &out, "djnz %s", label);
    sv_pushf(b, &out, "%s:", label2);
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.load16 (_16bit.py:997-1005) ------------------------------ */
static StrVec emit_load16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q->args[1], NULL, false);
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits16.store16 (_16bit.py:1007-1067) ---------------------------- */
static StrVec emit_store16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q->args[1], NULL, false);  /* ins[2] */
    const char *value = q->args[0];                            /* ins[1] */
    bool indirect = false;
    if (value[0] == '*') { indirect = true; value++; }

    if (s_is_int(value)) {
        long v = s_int_val(value) & 0xFFFF;
        if (indirect) {
            sv_push(b, &out, "ex de, hl");
            sv_pushf(b, &out, "ld hl, (%ld)", v);
            sv_push(b, &out, "ld (hl), e");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld (hl), d");
        } else {
            sv_pushf(b, &out, "ld (%ld), hl", v);
        }
        return out;
    }
    if (value[0] == '_' || value[0] == '.') {
        if (indirect) {
            sv_push(b, &out, "ex de, hl");
            sv_pushf(b, &out, "ld hl, (%s)", value);
            sv_push(b, &out, "ld (hl), e");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld (hl), d");
        } else {
            sv_pushf(b, &out, "ld (%s), hl", value);
        }
    } else if (value[0] == '#') {
        value++;
        if (indirect) {
            sv_push(b, &out, "ex de, hl");
            sv_pushf(b, &out, "ld hl, (%s)", value);
            sv_push(b, &out, "ld (hl), e");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld (hl), d");
        } else {
            sv_pushf(b, &out, "ld (%s), hl", value);
        }
    } else {
        sv_push(b, &out, "ex de, hl");
        if (indirect) {
            sv_push(b, &out, "pop hl");
            sv_push(b, &out, "ld a, (hl)");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld h, (hl)");
            sv_push(b, &out, "ld l, a");
        } else {
            sv_push(b, &out, "pop hl");
        }
        sv_push(b, &out, "ld (hl), e");
        sv_push(b, &out, "inc hl");
        sv_push(b, &out, "ld (hl), d");
    }
    return out;
}

/* ====================================================================
 * S5.4 — 32-bit (i32/u32), fixed-point (f16) & float (5-byte) scalar
 * emitters. Faithful ports of src/arch/z80/backend/_32bit.py (Bits32),
 * _f16.py (Fixed16), _float.py (Float), generic.py _cast, common.py
 * to_long/to_fixed/to_float, src/api/fp.py (via z80h_immediate_float).
 * ==================================================================== */

/* backend.common.is_float / float(op) — Python float() string parse. */
static bool s_is_float(const char *op) { return z80h_is_float(op); }
static double s_float_val(const char *op) {
    double v = 0.0; z80h_float(op, &v); return v;
}

/* _f_ops (common.py:216-235): like _int_ops but for floats. swap=True:
 *   is_float(op1) -> (op2, float(op1)); elif is_float(op2) -> (op1,
 *   float(op2)); else None. (swap=False unused by the S5.4 ops here.) */
static bool s_f_ops(const char *op1, const char *op2,
                    const char **other, double *fval) {
    if (s_is_float(op1)) { *other = op2; *fval = s_float_val(op1); return true; }
    if (s_is_float(op2)) { *other = op1; *fval = s_float_val(op2); return true; }
    return false;
}

/* tmp_labels.tmp_label() (src/api/tmp_labels.py:16-25):
 *   ".LABEL.__LABEL<LABEL_COUNTER>"; records in TMP_LABELS; ++counter.
 * gl.LABELS_NAMESPACE == ".LABEL" (global_.py:139). */
static char *s_tmp_label(Backend *b) {
    char buf[64];
    snprintf(buf, sizeof(buf), ".LABEL.__LABEL%d", b->label_counter);
    b->label_counter++;
    char *r = arena_strdup(b->arena, buf);
    hashmap_set(&b->tmp_labels, r, (void *)1);
    return r;
}

/* Public tmp_label() accessor — the Translator's control-flow visitors
 * (S5.5) allocate generated loop/if labels through the one Backend-scoped
 * counter + TMP_LABELS set (src/api/tmp_labels.py:16-25). */
char *backend_tmp_label(Backend *b) { return s_tmp_label(b); }

/* Bits32.int32 (_32bit.py:24-32): (int(op) & 0xFFFFFFFF) -> (DE,HL). */
static void s_int32(const char *op, unsigned *de, unsigned *hl) {
    z80h_int32(op, de, hl);
}

/* Fixed16.f16 (_f16.py:21-44): float -> (DE,HL) 16.16, C2 for negatives. */
static void s_f16(double op, unsigned *DE_out, unsigned *HL_out) {
    bool negative = op < 0.0;
    if (negative) op = -op;
    long long DEl = (long long)op;                 /* int(op) */
    unsigned HL = (unsigned)((long long)((op - (double)DEl) * 65536.0) & 0xFFFF);
    unsigned DE = (unsigned)(DEl & 0xFFFF);
    if (negative) {                                 /* Do C2 */
        DE ^= 0xFFFF;
        HL ^= 0xFFFF;
        unsigned long DEHL = (((unsigned long)DE << 16) | HL) + 1UL;
        HL = (unsigned)(DEHL & 0xFFFF);
        DE = (unsigned)((DEHL >> 16) & 0xFFFF);
    }
    *DE_out = DE; *HL_out = HL;
}

/* Quad arg accessor matching Python ins[N] (1-based: ins[1]==args[0]). */
static const char *q_ins(Quad *q, int n) {
    int idx = n - 1;
    if (idx < 0 || idx >= q->nargs) return "";
    return q->args[idx];
}

/* ---- Bits32.get_oper (_32bit.py:34-184) ------------------------------ */
static StrVec bits32_get_oper(Backend *b, const char *op1, const char *op2,
                              bool reversed, bool preserveHL) {
    StrVec out = sv_new();
    const char *op = (op2 != NULL) ? op2 : op1;
    bool int1 = false;                              /* op1 (2nd) is integer */

    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    const char *hl = preserveHL ? "bc" : "hl";

    if (s_is_int(op)) {
        int1 = true;
        if (indirect) {
            if (immediate) sv_pushf(b, &out, "ld hl, %s", op);
            else           sv_pushf(b, &out, "ld hl, (%s)", op);
            sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
            if (preserveHL) { sv_push(b, &out, "ld b, h");
                              sv_push(b, &out, "ld c, l"); }
        } else {
            unsigned DE, HL; s_int32(op, &DE, &HL);
            sv_pushf(b, &out, "ld de, %u", DE);
            sv_pushf(b, &out, "ld %s, %u", hl, HL);
        }
    } else {
        if (op[0] == '_') {
            if (immediate) sv_pushf(b, &out, "ld %s, %s", hl, op);
            else           sv_pushf(b, &out, "ld %s, (%s)", hl, op);
        } else {
            if (immediate) sv_pushf(b, &out, "ld %s, (%s) & 0xFFFF", hl, op);
            else           sv_pushf(b, &out, "pop %s", hl);
        }
        if (indirect) {
            sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
            if (preserveHL) { sv_push(b, &out, "ld b, h");
                              sv_push(b, &out, "ld c, l"); }
        } else {
            if (op[0] == '_') {
                sv_pushf(b, &out, "ld de, (%s + 2)", op);
            } else {
                if (immediate) sv_pushf(b, &out, "ld de, (%s) >> 16", op);
                else           sv_push(b, &out, "pop de");
            }
        }
    }

    if (op2 != NULL) {
        op = op1;
        indirect = op[0] == '*';
        if (indirect) op++;
        immediate = op[0] == '#';
        if (immediate) op++;

        if (s_is_int(op)) {
            long opv = s_int_val(op);
            if (indirect) {
                sv_push(b, &out, "exx");
                if (immediate) sv_pushf(b, &out, "ld hl, %ld", opv & 0xFFFF);
                else           sv_pushf(b, &out, "ld hl, (%ld)", opv & 0xFFFF);
                sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
                sv_push(b, &out, "push de");
                sv_push(b, &out, "push hl");
                sv_push(b, &out, "exx");
            } else {
                unsigned DE, HL; char ob[24];
                snprintf(ob, sizeof(ob), "%ld", opv);
                s_int32(ob, &DE, &HL);
                sv_pushf(b, &out, "ld bc, %u", DE);
                sv_push(b, &out, "push bc");
                sv_pushf(b, &out, "ld bc, %u", HL);
                sv_push(b, &out, "push bc");
            }
        } else {
            if (indirect) {
                sv_push(b, &out, "exx");
                if (op[0] == '_') {
                    if (immediate) sv_pushf(b, &out, "ld hl, %s", op);
                    else           sv_pushf(b, &out, "ld hl, (%s)", op);
                } else {
                    sv_push(b, &out, "pop hl");
                }
                sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
                sv_push(b, &out, "push de");
                sv_push(b, &out, "push hl");
                sv_push(b, &out, "exx");
            } else if (immediate) {
                sv_pushf(b, &out, "ld bc, (%s) >> 16", op);
                sv_push(b, &out, "push bc");
                sv_pushf(b, &out, "ld bc, (%s) & 0xFFFF", op);
                sv_push(b, &out, "push bc");
            } else if (op[0] == '_') {              /* an address */
                if (int1 || op1[0] == '_') {
                    StrVec tmp = out; out = sv_new();
                    sv_pushf(b, &out, "ld hl, (%s + 2)", op);
                    sv_push(b, &out, "push hl");
                    sv_pushf(b, &out, "ld hl, (%s)", op);
                    sv_push(b, &out, "push hl");
                    for (int i = 0; i < tmp.len; i++) vec_push(out, tmp.data[i]);
                    vec_free(tmp);
                } else {
                    sv_pushf(b, &out, "ld bc, (%s + 2)", op);
                    sv_push(b, &out, "push bc");
                    sv_pushf(b, &out, "ld bc, (%s)", op);
                    sv_push(b, &out, "push bc");
                }
            } else {
                /* 2nd operand remains in the stack */
            }
        }
    }

    if (op2 != NULL && reversed)
        sv_push(b, &out, s_runtime_call(b, RL_SWAP32));

    return out;
}

/* ---- Bits32.load32 (_32bit.py:862-871) ------------------------------- */
static StrVec emit_load32(Backend *b, Quad *q) {
    StrVec out = bits32_get_oper(b, q_ins(q, 2), NULL, false, false);
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32.store32 (_32bit.py:873-913) ------------------------------ */
static StrVec emit_store32(Backend *b, Quad *q) {
    const char *op = q_ins(q, 1);
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op) || op[0] == '_' || op[0] == '.' || immediate) {
        StrVec out = bits32_get_oper(b, q_ins(q, 2), NULL, false, indirect);
        char ibuf[24];
        if (s_is_int(op)) {
            snprintf(ibuf, sizeof(ibuf), "%ld", s_int_val(op) & 0xFFFF);
            op = ibuf;
        }
        if (indirect) {
            sv_pushf(b, &out, "ld hl, (%s)", op);
            sv_push(b, &out, s_runtime_call(b, RL_STORE32));
            return out;
        }
        sv_pushf(b, &out, "ld (%s), hl", op);
        sv_pushf(b, &out, "ld (%s + 2), de", op);
        return out;
    }

    StrVec out = bits32_get_oper(b, q_ins(q, 2), NULL, false, true);
    sv_push(b, &out, "pop hl");
    if (indirect) {
        sv_push(b, &out, s_runtime_call(b, RL_ISTORE32));
        return out;
    }
    sv_push(b, &out, s_runtime_call(b, RL_STORE32));
    return out;
}

/* ---- Bits32.add32 (_32bit.py:229-272) -------------------------------- */
static StrVec emit_add32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv)) {
        if (iv == 0) {                              /* int(o2)==0 -> nop */
            StrVec out = bits32_get_oper(b, other, NULL, false, false);
            sv_push(b, &out, "push de");
            sv_push(b, &out, "push hl");
            return out;
        }
    }
    if (op1[0] == '_' && op2[0] != '_') {
        const char *t = op1; op1 = op2; op2 = t;
    }
    if (op2[0] == '_') {
        StrVec out = bits32_get_oper(b, op1, NULL, false, false);
        sv_pushf(b, &out, "ld bc, (%s)", op2);
        sv_push(b, &out, "add hl, bc");
        sv_push(b, &out, "ex de, hl");
        sv_pushf(b, &out, "ld bc, (%s + 2)", op2);
        sv_push(b, &out, "adc hl, bc");
        sv_push(b, &out, "push hl");
        sv_push(b, &out, "push de");
        return out;
    }
    StrVec out = bits32_get_oper(b, op1, op2, false, false);
    sv_push(b, &out, "pop bc");
    sv_push(b, &out, "add hl, bc");
    sv_push(b, &out, "ex de, hl");
    sv_push(b, &out, "pop bc");
    sv_push(b, &out, "adc hl, bc");
    sv_push(b, &out, "push hl");
    sv_push(b, &out, "push de");
    return out;
}

/* ---- Bits32.sub32 (_32bit.py:274-297) -------------------------------- */
static StrVec emit_sub32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_int(op2) && s_int_val(op2) == 0) {
        StrVec out = bits32_get_oper(b, op1, NULL, false, false);
        sv_push(b, &out, "push de");
        sv_push(b, &out, "push hl");
        return out;
    }
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_SUB32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32.mul32 (_32bit.py:299-332) -------------------------------- */
static StrVec emit_mul32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    const char *other; long iv;
    StrVec out;
    if (s_int_ops(op1, op2, &other, &iv)) {
        out = bits32_get_oper(b, other, NULL, false, false);
        if (iv == 1) { sv_push(b, &out, "push de");
                       sv_push(b, &out, "push hl"); return out; }
        if (iv == 0) { sv_push(b, &out, "ld hl, 0");
                       sv_push(b, &out, "push hl");
                       sv_push(b, &out, "push hl"); return out; }
        char ob[24]; snprintf(ob, sizeof(ob), "%ld", iv);
        char *op2s = arena_strdup(b->arena, ob);
        vec_free(out);
        out = bits32_get_oper(b, other, op2s, false, false);
    } else {
        out = bits32_get_oper(b, op1, op2, false, false);
    }
    sv_push(b, &out, s_runtime_call(b, RL_MUL32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32.divu32 (_32bit.py:334-356) ------------------------------- */
static StrVec emit_divu32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_int(op2) && s_int_val(op2) == 1) {
        StrVec out = bits32_get_oper(b, op1, NULL, false, false);
        sv_push(b, &out, "push de");
        sv_push(b, &out, "push hl");
        return out;
    }
    bool rev = s_is_int(op1) || op1[0] == 't' || op2[0] != 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_DIVU32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

static StrVec emit_neg32(Backend *b, Quad *q);

/* ---- Bits32.divi32 (_32bit.py:358-384) ------------------------------- */
static StrVec emit_divi32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_int(op2)) {
        if (s_int_val(op2) == 1) {
            StrVec out = bits32_get_oper(b, op1, NULL, false, false);
            sv_push(b, &out, "push de");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (s_int_val(op2) == -1) return emit_neg32(b, q);
    }
    bool rev = s_is_int(op1) || op1[0] == 't' || op2[0] != 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_DIVI32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32.modu32 (_32bit.py:386-409) ------------------------------- */
static StrVec emit_modu32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_int(op2) && s_int_val(op2) == 1) {
        StrVec out = bits32_get_oper(b, op1, NULL, false, false);
        sv_push(b, &out, "ld hl, 0");
        sv_push(b, &out, "push hl");
        sv_push(b, &out, "push hl");
        return out;
    }
    bool rev = s_is_int(op1) || op1[0] == 't' || op2[0] != 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_MODU32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32.modi32 (_32bit.py:411-434) ------------------------------- */
static StrVec emit_modi32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_int(op2) && s_int_val(op2) == 1) {
        StrVec out = bits32_get_oper(b, op1, NULL, false, false);
        sv_push(b, &out, "ld hl, 0");
        sv_push(b, &out, "push hl");
        sv_push(b, &out, "push hl");
        return out;
    }
    bool rev = s_is_int(op1) || op1[0] == 't' || op2[0] != 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_MODI32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32.unary (_32bit.py:207-213) -------------------------------- */
static StrVec bits32_unary(Backend *b, Quad *q, const char *label) {
    StrVec out = bits32_get_oper(b, q_ins(q, 2), NULL, false, false);
    sv_push(b, &out, s_runtime_call(b, label));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_neg32(Backend *b, Quad *q) {
    return bits32_unary(b, q, RL_NEG32);            /* _32bit.py:711-714 */
}
static StrVec emit_abs32(Backend *b, Quad *q) {
    return bits32_unary(b, q, RL_ABS32);            /* _32bit.py:716-719 */
}

/* ---- Bits32.bool_binary (_32bit.py:186-205) -------------------------- */
static StrVec bits32_bool_binary(Backend *b, Quad *q, const char *label,
                                 bool commutative) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = commutative && op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    const char *other; long iv;
    if (commutative && s_int_ops(op1, op2, &other, &iv)) {
        char ob[24]; snprintf(ob, sizeof(ob), "%ld", iv);
        op1 = other; op2 = arena_strdup(b->arena, ob);
    }
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, label));
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits32 comparisons (_32bit.py:436-600) -------------------------- */
static StrVec emit_ltu32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_SUB32));
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_lti32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_LTI32));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gtu32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, "pop bc");
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, bc");
    sv_push(b, &out, "ex de, hl");
    sv_push(b, &out, "pop de");
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gti32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_LEI32));
    sv_push(b, &out, "sub 1");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_leu32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, "pop bc");
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, bc");
    sv_push(b, &out, "ex de, hl");
    sv_push(b, &out, "pop de");
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "ccf");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_lei32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_LEI32));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_geu32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_SUB32));
    sv_push(b, &out, "ccf");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gei32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = op1[0] != 't' && !s_is_int(op1) && op2[0] == 't';
    StrVec out = bits32_get_oper(b, op1, op2, rev, false);
    sv_push(b, &out, s_runtime_call(b, RL_LTI32));
    sv_push(b, &out, "sub 1");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_eq32(Backend *b, Quad *q) {
    return bits32_bool_binary(b, q, RL_EQ32, false);
}
static StrVec emit_ne32(Backend *b, Quad *q) {
    /* eq32(ins)[:-1] + sub 1 / sbc a,a / push af  (_32bit.py:589-600) */
    StrVec out = bits32_bool_binary(b, q, RL_EQ32, false);
    if (out.len > 0) out.len--;                     /* drop trailing push af */
    sv_push(b, &out, "sub 1");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_or32(Backend *b, Quad *q) {
    return bits32_bool_binary(b, q, RL_OR32, false);
}
static StrVec emit_xor32(Backend *b, Quad *q) {
    return bits32_bool_binary(b, q, RL_XOR32, false);
}
static StrVec emit_and32(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    const char *other; long iv;
    if (s_int_ops(op1, op2, &other, &iv) && iv == 0) {
        StrVec out;
        if (other[0] == 't') out = bits32_get_oper(b, other, NULL, false, false);
        else                 out = sv_new();
        sv_push(b, &out, "xor a");
        sv_push(b, &out, "push af");
        return out;
    }
    return bits32_bool_binary(b, q, RL_AND32, true);
}
static StrVec emit_not32(Backend *b, Quad *q) {
    StrVec out = bits32_get_oper(b, q_ins(q, 2), NULL, false, false);
    sv_push(b, &out, s_runtime_call(b, RL_NOT32));
    sv_push(b, &out, "push af");
    return out;
}
/* Bitwise band/bor/bxor/bnot32 (_32bit.py:613-709) */
static StrVec bits32_bitwise(Backend *b, Quad *q, const char *label) {
    StrVec out = bits32_get_oper(b, q_ins(q, 2), q_ins(q, 3), false, false);
    sv_push(b, &out, s_runtime_call(b, label));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_band32(Backend *b, Quad *q) { return bits32_bitwise(b, q, RL_BAND32); }
static StrVec emit_bor32(Backend *b, Quad *q)  { return bits32_bitwise(b, q, RL_BOR32); }
static StrVec emit_bxor32(Backend *b, Quad *q) { return bits32_bitwise(b, q, RL_BXOR32); }
static StrVec emit_bnot32(Backend *b, Quad *q) {
    StrVec out = bits32_get_oper(b, q_ins(q, 2), NULL, false, false);
    sv_push(b, &out, s_runtime_call(b, RL_BNOT32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

/* ---- Bits32 shifts (_32bit.py:721-860) ------------------------------- */
static StrVec bits32_shift(Backend *b, Quad *q, const char *label) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_int(op2)) {
        StrVec out = bits32_get_oper(b, op1, NULL, false, false);
        if (s_int_val(op2) == 0) {
            sv_push(b, &out, "push de");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (s_int_val(op2) > 1) {
            char *lab = s_tmp_label(b);
            sv_pushf(b, &out, "ld b, %s", op2);
            sv_pushf(b, &out, "%s:", lab);
            sv_push(b, &out, s_runtime_call(b, label));
            sv_pushf(b, &out, "djnz %s", lab);
        } else {
            sv_push(b, &out, s_runtime_call(b, label));
        }
        sv_push(b, &out, "push de");
        sv_push(b, &out, "push hl");
        return out;
    }
    StrVec out = bits8_get_oper(b, op2, NULL, false);
    sv_push(b, &out, "ld b, a");
    {
        StrVec g = bits32_get_oper(b, op1, NULL, false, false);
        for (int i = 0; i < g.len; i++) vec_push(out, g.data[i]);
        vec_free(g);
    }
    char *lab = s_tmp_label(b);
    char *lab2 = s_tmp_label(b);
    sv_push(b, &out, "or a");
    sv_pushf(b, &out, "jr z, %s", lab2);
    sv_pushf(b, &out, "%s:", lab);
    sv_push(b, &out, s_runtime_call(b, label));
    sv_pushf(b, &out, "djnz %s", lab);
    sv_pushf(b, &out, "%s:", lab2);
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_shru32(Backend *b, Quad *q) { return bits32_shift(b, q, RL_SHRL32); }
static StrVec emit_shri32(Backend *b, Quad *q) { return bits32_shift(b, q, RL_SHRA32); }
static StrVec emit_shl32(Backend *b, Quad *q)  { return bits32_shift(b, q, RL_SHL32); }

/* ---- Bits32.param32/ret32 (_32bit.py:961-974) ------------------------ */
static StrVec emit_param32(Backend *b, Quad *q) {
    StrVec out = bits32_get_oper(b, q_ins(q, 1), NULL, false, false);
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_ret32(Backend *b, Quad *q) {
    StrVec out = bits32_get_oper(b, q_ins(q, 1), NULL, false, false);
    sv_push(b, &out, "#pragma opt require hl,de");
    sv_pushf(b, &out, "jp %s", q_ins(q, 2));
    return out;
}

/* ====================================================================
 * S5.7b — caller/callee ABI emitters (params, fparams, per-width ret,
 * paddr, pload). Verbatim ports.
 * ==================================================================== */

/* Bits8.param8/ret8/fparam8 (_8bit.py:1158-1179). get_oper(ins[1]) ->
 * value in A; param8 then `push af`; ret8 `#pragma opt require a` +
 * `jp leave`; fparam8 is just get_oper (value already where needed). */
static StrVec emit_param8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 1), NULL, false);
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_ret8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 1), NULL, false);
    sv_push(b, &out, "#pragma opt require a");
    sv_pushf(b, &out, "jp %s", q_ins(q, 2));
    return out;
}
static StrVec emit_fparam8(Backend *b, Quad *q) {
    return bits8_get_oper(b, q_ins(q, 1), NULL, false);
}

/* Bits16.param16/ret16/fparam16 (_16bit.py:1112-1133). get_oper -> HL;
 * param16 `push hl`; ret16 `#pragma opt require hl` + `jp leave`. */
static StrVec emit_param16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 1), NULL, false);
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_ret16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 1), NULL, false);
    sv_push(b, &out, "#pragma opt require hl");
    sv_pushf(b, &out, "jp %s", q_ins(q, 2));
    return out;
}
static StrVec emit_fparam16(Backend *b, Quad *q) {
    return bits16_get_oper(b, q_ins(q, 1), NULL, false);
}

/* Bits32.fparam32 (_32bit.py:977-983): get_oper only (value in DEHL). */
static StrVec emit_fparam32(Backend *b, Quad *q) {
    return bits32_get_oper(b, q_ins(q, 1), NULL, false, false);
}

/* _paddr (_pload.py:19-46): code that points HL at a local/param.
 * oper may be `*`-indirect; i = int(oper); i>=0 -> i += 4 (ret addr +
 * pushed IX); push IX/pop HL; ld de,i; add hl,de; indirect -> deref;
 * push hl. IDX_REG == "ix". */
static StrVec emit_paddr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    const char *oper = q_ins(q, 1);
    bool indirect = (oper[0] == '*');
    if (indirect) oper++;
    long i = s_int_val(oper);
    if (i >= 0) i += 4;  /* Return Address + "push IX" */
    sv_push(b, &out, "push ix");
    sv_push(b, &out, "pop hl");
    sv_pushf(b, &out, "ld de, %ld", i);
    sv_push(b, &out, "add hl, de");
    if (indirect) {
        sv_push(b, &out, "ld e, (hl)");
        sv_push(b, &out, "inc hl");
        sv_push(b, &out, "ld h, (hl)");
        sv_push(b, &out, "ld l, e");
    }
    sv_push(b, &out, "push hl");
    return out;
}

/* _pload(offset, size) (_pload.py:49-114): the shared (IX+offset)
 * loader. Verbatim port; size in {1,2,4,5}. Used by _pload8/16/32 (and
 * _ploadf for size 5). IDX_REG == "ix". */
static void s_pload(Backend *b, StrVec *out, const char *offset, int size) {
    bool indirect = (offset[0] == '*');
    if (indirect) offset++;
    long i = s_int_val(offset);
    if (i >= 0)  /* parameter: round up to even bytes (ret addr + push IX) */
        i += 4 + (indirect ? 0 : (size % 2));
    bool ix_changed = (indirect || size < 5) &&
                      ((labs(i) + size) > 127);
    if (ix_changed) {
        sv_push(b, out, "push ix");
        sv_pushf(b, out, "ld de, %ld", i);
        sv_push(b, out, "add ix, de");
        i = 0;
    } else if (size == 5) {
        sv_push(b, out, "push ix");
        sv_push(b, out, "pop hl");
        sv_pushf(b, out, "ld de, %ld", i);
        sv_push(b, out, "add hl, de");
        i = 0;
    }
    if (indirect) {
        sv_pushf(b, out, "ld h, (ix%+ld)", i + 1);
        sv_pushf(b, out, "ld l, (ix%+ld)", i);
        if (size == 1) {
            sv_push(b, out, "ld a, (hl)");
        } else if (size == 2) {
            sv_push(b, out, "ld c, (hl)");
            sv_push(b, out, "inc hl");
            sv_push(b, out, "ld h, (hl)");
            sv_push(b, out, "ld l, c");
        } else if (size == 4) {
            sv_push(b, out, s_runtime_call(b, RL_ILOAD32));
        } else {
            sv_push(b, out, s_runtime_call(b, RL_ILOADF));
        }
    } else {
        if (size == 1) {
            sv_pushf(b, out, "ld a, (ix%+ld)", i);
        } else if (size <= 4) {
            sv_pushf(b, out, "ld l, (ix%+ld)", i);
            sv_pushf(b, out, "ld h, (ix%+ld)", i + 1);
            if (size > 2) {
                sv_pushf(b, out, "ld e, (ix%+ld)", i + 2);
                sv_pushf(b, out, "ld d, (ix%+ld)", i + 3);
            }
        } else {
            sv_push(b, out, s_runtime_call(b, RL_PLOADF));
        }
    }
    if (ix_changed)
        sv_push(b, out, "pop ix");
}
/* _pload8/16/32 (_pload.py:117-152): _pload(ins[2], N) then push. */
static StrVec emit_pload8(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_pload(b, &out, q_ins(q, 2), 1);
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_pload16(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_pload(b, &out, q_ins(q, 2), 2);
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_pload32(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_pload(b, &out, q_ins(q, 2), 4);
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
/* float_fpush body is defined later (with the Float emitters); the
 * existing forward decl is later still, so declare here for emit_ploadf. */
static void float_fpush(Backend *b, StrVec *out);
/* _ploadf (_pload.py:155-160): _pload(ins[2], 5); Float.fpush().
 * 5-byte float local-load. PLOADF16 (main.py:532) routes to _pload32. */
static StrVec emit_ploadf(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_pload(b, &out, q_ins(q, 2), 5);
    float_fpush(b, &out);
    return out;
}
/* _ploadstr (_pload.py:166-178): _pload(ins[2], 2); if ins[1][0] != '$':
 * runtime_call(LOADSTR); then push hl. ins[1] == q->args[0] (the temp). */
static StrVec emit_ploadstr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_pload(b, &out, q_ins(q, 2), 2);
    const char *t = q_ins(q, 1);
    if (t[0] != '$')
        sv_push(b, &out, s_runtime_call(b, RL_LOADSTR));
    sv_push(b, &out, "push hl");
    return out;
}
/* _fploadstr (_pload.py:181-193): _pload(ins[2], 2); if ins[1][0] != '$':
 * runtime_call(LOADSTR). Unlike ploadstr, no push hl. */
static StrVec emit_fploadstr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_pload(b, &out, q_ins(q, 2), 2);
    const char *t = q_ins(q, 1);
    if (t[0] != '$')
        sv_push(b, &out, s_runtime_call(b, RL_LOADSTR));
    return out;
}
/* _pstore8 (_pload.py:196-258): store ins[2] at IX + ins[1] (8-bit).
 * ins[1]==q->args[0] (offset, '*'=indirect), ins[2]==q->args[1] (value).
 * Verbatim port. */
static StrVec emit_pstore8(Backend *b, Quad *q) {
    const char *value  = q_ins(q, 2);
    const char *offset = q_ins(q, 1);
    bool indirect = offset[0] == '*';
    int size = 0;
    if (indirect) { offset++; size = 1; }
    long I = s_int_val(offset);
    if (I >= 0) {
        I += 4;                       /* ret addr + push IX */
        if (!indirect) I += 1;        /* F flag ignored */
    }
    StrVec out;
    bool vint = s_is_int(value);
    if (vint) out = sv_new();
    else      out = bits8_get_oper(b, value, NULL, false);

    bool ix_changed = !(-128 + size <= I && I <= 127 - size);
    if (ix_changed) {
        sv_push(b, &out, "push ix");
        sv_push(b, &out, "pop hl");
        sv_pushf(b, &out, "ld de, %ld", I);
        sv_push(b, &out, "add hl, de");
    }
    if (indirect) {
        if (ix_changed) {
            sv_push(b, &out, "ld c, (hl)");
            sv_push(b, &out, "inc hl");
            sv_push(b, &out, "ld h, (hl)");
            sv_push(b, &out, "ld l, c");
        } else {
            sv_pushf(b, &out, "ld h, (ix%+ld)", I + 1);
            sv_pushf(b, &out, "ld l, (ix%+ld)", I);
        }
        if (vint) sv_pushf(b, &out, "ld (hl), %d", s_int8(s_int_val(value)));
        else      sv_push(b, &out, "ld (hl), a");
        return out;
    }
    /* direct store */
    if (ix_changed) {
        if (vint) sv_pushf(b, &out, "ld (hl), %d", s_int8(s_int_val(value)));
        else      sv_push(b, &out, "ld (hl), a");
        return out;
    }
    if (vint)
        sv_pushf(b, &out, "ld (ix%+ld), %d", I, s_int8(s_int_val(value)));
    else
        sv_pushf(b, &out, "ld (ix%+ld), a", I);
    return out;
}
/* _pstore16 (_pload.py:262-323): verbatim port. */
static StrVec emit_pstore16(Backend *b, Quad *q) {
    const char *value  = q_ins(q, 2);
    const char *offset = q_ins(q, 1);
    bool indirect = offset[0] == '*';
    int size = 1;
    if (indirect) offset++;
    long i = s_int_val(offset);
    if (i >= 0) i += 4;
    StrVec out;
    bool vint = s_is_int(value);
    if (vint) out = sv_new();
    else      out = bits16_get_oper(b, value, NULL, false);

    bool ix_changed = !(-128 + size <= i && i <= 127 - size);
    if (indirect) {
        if (vint) sv_pushf(b, &out, "ld hl, %d", s_int16(s_int_val(value)));
        sv_pushf(b, &out, "ld bc, %ld", i);
        sv_push(b, &out, s_runtime_call(b, RL_PISTORE16));
        return out;
    }
    if (ix_changed) {
        if (!vint) sv_push(b, &out, "ex de, hl");
        sv_push(b, &out, "push ix");
        sv_push(b, &out, "pop hl");
        sv_pushf(b, &out, "ld bc, %ld", i);
        sv_push(b, &out, "add hl, bc");
        if (vint) {
            int v = s_int16(s_int_val(value));
            sv_pushf(b, &out, "ld (hl), %d", v & 0xFF);
            sv_push(b, &out, "inc hl");
            sv_pushf(b, &out, "ld (hl), %d", (v >> 8) & 0xFF);
            return out;
        }
        sv_push(b, &out, "ld (hl), e");
        sv_push(b, &out, "inc hl");
        sv_push(b, &out, "ld (hl), d");
        return out;
    }
    if (vint) {
        int v = s_int16(s_int_val(value));
        sv_pushf(b, &out, "ld (ix%+ld), %d", i, v & 0xFF);
        sv_pushf(b, &out, "ld (ix%+ld), %d", i + 1, (v >> 8) & 0xFF);
    } else {
        sv_pushf(b, &out, "ld (ix%+ld), l", i);
        sv_pushf(b, &out, "ld (ix%+ld), h", i + 1);
    }
    return out;
}
/* _pstore32 (_pload.py:326-353): verbatim port. */
static StrVec emit_pstore32(Backend *b, Quad *q) {
    const char *value  = q_ins(q, 2);
    const char *offset = q_ins(q, 1);
    bool indirect = offset[0] == '*';
    if (indirect) offset++;
    long i = s_int_val(offset);
    if (i >= 0) i += 4;
    StrVec out = bits32_get_oper(b, value, NULL, false, false);
    sv_pushf(b, &out, "ld bc, %ld", i);
    sv_push(b, &out, s_runtime_call(b,
                                    indirect ? RL_PISTORE32 : RL_PSTORE32));
    return out;
}

/* _pstorestr (_pload.py:416-475): verbatim port. Stores a STRING value
 * (2nd operand) into a local/param string slot at offset (1st operand).
 * Note: STRINGS are 16-bit pointers, but the heap-aware copy is dispatched
 * via PSTORE_STR / PSTORE_STR2 (immediate / from-heap value) and their
 * indirect (PISTORE_STR / PISTORE_STR2) twins when the offset is '*'. */
static StrVec emit_pstorestr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    /* 2nd operand first (must go into DE / onto stack-pop). */
    const char *value = q_ins(q, 2);
    bool v_indirect = (value[0] == '*');
    if (v_indirect) value++;
    bool temporal = false;
    if (value[0] == '_') {
        sv_pushf(b, &out, "ld de, (%s)", value);
        if (v_indirect)
            sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
    } else if (value[0] == '#') {
        sv_pushf(b, &out, "ld de, %s", value + 1);
    } else {
        sv_push(b, &out, "pop de");
        temporal = (value[0] != '$');
        if (v_indirect)
            sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
    }

    /* 1st operand: offset, possibly '*' indirect. */
    const char *offset = q_ins(q, 1);
    bool indirect = (offset[0] == '*');
    if (indirect) offset++;
    long i = s_int_val(offset);
    if (i >= 0) i += 4;  /* Return Address + "push IX" */
    sv_pushf(b, &out, "ld bc, %ld", i);

    if (!temporal) {
        sv_push(b, &out, s_runtime_call(b,
            indirect ? RL_PISTORE_STR : RL_PSTORE_STR));
    } else {
        sv_push(b, &out, s_runtime_call(b,
            indirect ? RL_PISTORE_STR2 : RL_PSTORE_STR2));
    }
    return out;
}

/* ====================================================================
 * Array element access — _array.py / _parray.py.
 * __ARRAY computes the element address (HL) from the array base (in HL)
 * + the index values pushed on the stack.
 * ==================================================================== */

/* Forward decls — Float helpers are defined further down. */
static void float_fpush(Backend *b, StrVec *out);
static StrVec float_get_oper(Backend *b, const char *op1, const char *op2);

/* _array.py:_addr (17-49): common array-address subroutine.  Tries
 * int(value) & 0xFFFF (immediate); on ValueError, a '_' label loads
 * directly, anything else (a temp tN) is popped from the stack.  An
 * indirect ('*') value dereferences [HL] into HL.  Then call __ARRAY. */
static void s_array_addr(Backend *b, StrVec *out, const char *value) {
    bool indirect = (value[0] == '*');
    if (indirect) value++;

    if (s_is_int(value)) {
        long v = s_int_val(value) & 0xFFFF;
        if (indirect)
            sv_pushf(b, out, "ld hl, (%ld)", v);
        else
            sv_pushf(b, out, "ld hl, %ld", v);
    } else if (value[0] == '_') {
        sv_pushf(b, out, "ld hl, %s", value);
        if (indirect) {
            sv_push(b, out, "ld c, (hl)");
            sv_push(b, out, "inc hl");
            sv_push(b, out, "ld h, (hl)");
            sv_push(b, out, "ld l, c");
        }
    } else {
        sv_push(b, out, "pop hl");
        if (indirect) {
            sv_push(b, out, "ld c, (hl)");
            sv_push(b, out, "inc hl");
            sv_push(b, out, "ld h, (hl)");
            sv_push(b, out, "ld l, c");
        }
    }
    sv_push(b, out, s_runtime_call(b, RL_ARRAY));
}

/* _aaddr (_array.py:52-58): _addr(ins[2]) ; push hl */
static StrVec emit_aaddr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, "push hl");
    return out;
}
/* _aload8 (_array.py:61-69) */
static StrVec emit_aload8(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, "ld a, (hl)");
    sv_push(b, &out, "push af");
    return out;
}
/* _aload16 (_array.py:72-83) */
static StrVec emit_aload16(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, "ld e, (hl)");
    sv_push(b, &out, "inc hl");
    sv_push(b, &out, "ld d, (hl)");
    sv_push(b, &out, "ex de, hl");
    sv_push(b, &out, "push hl");
    return out;
}
/* _aload32 (_array.py:86-96) */
static StrVec emit_aload32(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
/* _aloadf (_array.py:99-108) */
static StrVec emit_aloadf(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, s_runtime_call(b, RL_LOADF));
    float_fpush(b, &out);
    return out;
}
/* _aloadstr (_array.py:111-118) */
static StrVec emit_aloadstr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, s_runtime_call(b, RL_ILOADSTR));
    sv_push(b, &out, "push hl");
    return out;
}
/* _astore8 (_array.py:121-167) */
static StrVec emit_astore8(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 1));
    const char *op = q_ins(q, 2);
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op)) {
        char ibuf[24];
        if (indirect) {
            if (immediate) {
                snprintf(ibuf, sizeof(ibuf), "%ld", s_int_val(op) & 0xFFFF);
                sv_pushf(b, &out, "ld a, (%s)", ibuf);
            } else {
                sv_pushf(b, &out, "ld de, (%s)", op);
                sv_push(b, &out, "ld a, (de)");
            }
        } else {
            snprintf(ibuf, sizeof(ibuf), "%ld", s_int_val(op) & 0xFF);
            sv_pushf(b, &out, "ld (hl), %s", ibuf);
            return out;
        }
    } else if (op[0] == '_') {
        if (indirect) {
            if (immediate) {
                sv_pushf(b, &out, "ld a, (%s)", op);
            } else {
                sv_pushf(b, &out, "ld de, (%s)", op);
                sv_push(b, &out, "ld a, (de)");
            }
        } else {
            if (immediate)
                sv_pushf(b, &out, "ld a, %s", op);
            else
                sv_pushf(b, &out, "ld a, (%s)", op);
        }
    } else {
        sv_push(b, &out, "pop af");
    }
    sv_push(b, &out, "ld (hl), a");
    return out;
}
/* _astore16 (_array.py:170-221) */
static StrVec emit_astore16(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 1));
    const char *op = q_ins(q, 2);
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op)) {
        long v = s_int_val(op) & 0xFFFF;
        char ibuf[24];
        snprintf(ibuf, sizeof(ibuf), "%ld", v);
        if (indirect) {
            if (immediate) {
                sv_pushf(b, &out, "ld de, (%s)", ibuf);
            } else {
                sv_pushf(b, &out, "ld de, (%s)", ibuf);
                sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
            }
        } else {
            long H = v >> 8, L = v & 0xFF;
            sv_pushf(b, &out, "ld (hl), %ld", L);
            sv_push(b, &out, "inc hl");
            sv_pushf(b, &out, "ld (hl), %ld", H);
            return out;
        }
    } else if (op[0] == '_') {
        if (indirect) {
            if (immediate) {
                sv_pushf(b, &out, "ld de, (%s)", op);
            } else {
                sv_pushf(b, &out, "ld de, (%s)", op);
                sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
            }
        } else {
            if (immediate)
                sv_pushf(b, &out, "ld de, %s", op);
            else
                sv_pushf(b, &out, "ld de, (%s)", op);
        }
    } else {
        sv_push(b, &out, "pop de");
    }
    sv_push(b, &out, "ld (hl), e");
    sv_push(b, &out, "inc hl");
    sv_push(b, &out, "ld (hl), d");
    return out;
}
/* _astore32 (_array.py:224-254) */
static StrVec emit_astore32(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 1));
    const char *value = q_ins(q, 2);
    bool indirect = (value[0] == '*');
    if (indirect) value++;

    if (s_is_int(value)) {
        long long v = (long long)(strtoll(value, NULL, 10)) & 0xFFFFFFFFLL;
        if (indirect) {
            sv_push(b, &out, "push hl");
            sv_pushf(b, &out, "ld hl, %lld", v & 0xFFFF);
            sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
            sv_push(b, &out, "ld b, h");
            sv_push(b, &out, "ld c, l");
            sv_push(b, &out, "pop hl");
        } else {
            sv_pushf(b, &out, "ld de, %lld", (v >> 16) & 0xFFFF);
            sv_pushf(b, &out, "ld bc, %lld", v & 0xFFFF);
        }
    } else {
        StrVec g = bits32_get_oper(b, value, NULL, false, true);
        for (int i = 0; i < g.len; i++) vec_push(out, g.data[i]);
    }
    sv_push(b, &out, s_runtime_call(b, RL_STORE32));
    return out;
}
/* _astoref (_array.py:287-305) */
static StrVec emit_astoref(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_array_addr(b, &out, q_ins(q, 1));
    const char *value = q_ins(q, 2);
    bool indirect = (value[0] == '*');
    const char *vraw = q_ins(q, 2);
    if (indirect) value++;
    if (indirect) {
        sv_push(b, &out, "push hl");
        StrVec g = float_get_oper(b, vraw, NULL);
        for (int i = 0; i < g.len; i++) vec_push(out, g.data[i]);
        sv_push(b, &out, "pop hl");
    } else {
        StrVec g = float_get_oper(b, vraw, NULL);
        for (int i = 0; i < g.len; i++) vec_push(out, g.data[i]);
    }
    sv_push(b, &out, s_runtime_call(b, RL_STOREF));
    return out;
}

/* _parray.py:_paddr (17-46): the IX-relative element-address helper.
 * Like _paddr (params) but ends with a __ARRAY/__ARRAY_PTR call. */
static void s_parray_addr(Backend *b, StrVec *out, const char *offset) {
    bool indirect = (offset[0] == '*');
    if (indirect) offset++;
    long i = s_int_val(offset);
    if (i >= 0) i += 4;  /* Return Address + "push IX" */
    sv_push(b, out, "push ix");
    sv_push(b, out, "pop hl");
    sv_pushf(b, out, "ld de, %ld", i);
    sv_push(b, out, "add hl, de");
    if (indirect)
        sv_push(b, out, s_runtime_call(b, RL_ARRAY_PTR));
    else
        sv_push(b, out, s_runtime_call(b, RL_ARRAY));
}
/* _paaddr (_parray.py:49-54) */
static StrVec emit_paaddr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, "push hl");
    return out;
}
/* _paload8 (_parray.py:57-65) */
static StrVec emit_paload8(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, "ld a, (hl)");
    sv_push(b, &out, "push af");
    return out;
}
/* _paload16 (_parray.py:68-79) */
static StrVec emit_paload16(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, "ld e, (hl)");
    sv_push(b, &out, "inc hl");
    sv_push(b, &out, "ld d, (hl)");
    sv_push(b, &out, "ex de, hl");
    sv_push(b, &out, "push hl");
    return out;
}
/* _paload32 (_parray.py:82-92) */
static StrVec emit_paload32(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
/* _paloadf (_parray.py:95-101) */
static StrVec emit_paloadf(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, s_runtime_call(b, RL_LOADF));
    float_fpush(b, &out);
    return out;
}
/* _paloadstr (_parray.py:104-110) */
static StrVec emit_paloadstr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 2));
    sv_push(b, &out, s_runtime_call(b, RL_ILOADSTR));
    sv_push(b, &out, "push hl");
    return out;
}
/* _pastore8 (_parray.py:113-141) */
static StrVec emit_pastore8(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 1));
    const char *value = q_ins(q, 2);
    bool indirect = (value[0] == '*');
    if (indirect) value++;
    if (s_is_int(value)) {
        long v = s_int_val(value) & 0xFFFF;
        if (indirect) {
            sv_pushf(b, &out, "ld a, (%ld)", v);
            sv_push(b, &out, "ld (hl), a");
        } else {
            sv_pushf(b, &out, "ld (hl), %ld", v & 0xFF);
        }
    } else {
        sv_push(b, &out, "pop af");
        sv_push(b, &out, "ld (hl), a");
    }
    return out;
}
/* _pastore16 (_parray.py:144-165) */
static StrVec emit_pastore16(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 1));
    const char *value = q_ins(q, 2);
    bool indirect = (value[0] == '*');
    if (indirect) value++;
    if (s_is_int(value)) {
        long v = s_int_val(value) & 0xFFFF;
        sv_pushf(b, &out, "ld de, %ld", v);
        if (indirect)
            sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
    } else {
        sv_push(b, &out, "pop de");
    }
    sv_push(b, &out, "ld (hl), e");
    sv_push(b, &out, "inc hl");
    sv_push(b, &out, "ld (hl), d");
    return out;
}
/* _pastore32 (_parray.py:168-194) */
static StrVec emit_pastore32(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 1));
    const char *value = q_ins(q, 2);
    bool indirect = (value[0] == '*');
    if (indirect) value++;
    if (s_is_int(value)) {
        long long v = (long long)strtoll(value, NULL, 10) & 0xFFFFFFFFLL;
        if (indirect) {
            sv_push(b, &out, "push hl");
            sv_pushf(b, &out, "ld hl, %lld", v & 0xFFFF);
            sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
            sv_push(b, &out, "ld b, h");
            sv_push(b, &out, "ld c, l");
            sv_push(b, &out, "pop hl");
        } else {
            sv_pushf(b, &out, "ld de, %lld", (v >> 16) & 0xFFFF);
            sv_pushf(b, &out, "ld bc, %lld", v & 0xFFFF);
        }
    } else {
        sv_push(b, &out, "pop bc");
        sv_push(b, &out, "pop de");
    }
    sv_push(b, &out, s_runtime_call(b, RL_STORE32));
    return out;
}

/* _pastorestr (_parray.py:285-332): verbatim port. Stores a STRING value
 * (2nd operand) into the array element at the IX-relative array offset
 * (1st operand). Uses __ARRAY/__ARRAY_PTR to compute the element address
 * (in HL), then __STORE_STR / __STORE_STR2 (heap-aware) to copy the
 * string. Allows immediate-string ('#') as the 2nd operand. */
static StrVec emit_pastorestr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    s_parray_addr(b, &out, q_ins(q, 1));

    const char *value = q_ins(q, 2);
    bool v_indirect = (value[0] == '*');
    if (v_indirect) value++;
    bool immediate = (value[0] == '#');
    if (immediate) value++;
    bool temporal = !immediate && (value[0] != '$');
    if (temporal) value++;

    if (value[0] == '_') {
        if (v_indirect) {
            if (immediate)
                sv_pushf(b, &out, "ld de, (%s)", value);
            else {
                sv_pushf(b, &out, "ld de, (%s)", value);
                sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
            }
        } else {
            if (immediate)
                sv_pushf(b, &out, "ld de, %s", value);
            else
                sv_pushf(b, &out, "ld de, (%s)", value);
        }
    } else {
        if (immediate)
            sv_pushf(b, &out, "ld de, %s", value);
        else
            sv_push(b, &out, "pop de");

        if (v_indirect)
            sv_push(b, &out, s_runtime_call(b, RL_LOAD_DE_DE));
    }

    if (!temporal)
        sv_push(b, &out, s_runtime_call(b, RL_STORE_STR));
    else
        sv_push(b, &out, s_runtime_call(b, RL_STORE_STR2));

    return out;
}

/* _exchg (generic.py:55-61): exchange ALL registers. */
static StrVec emit_exchg(Backend *b, Quad *q) {
    (void)q;
    StrVec out = sv_new();
    sv_push(b, &out, "ex af, af'");
    sv_push(b, &out, "exx");
    return out;
}

/* _memcopy (generic.py:539-550): block copy param2 -> param1, len param3. */
static StrVec emit_memcopy(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 3), NULL, false);
    sv_push(b, &out, "ld b, h");
    sv_push(b, &out, "ld c, l");
    StrVec g = bits16_get_oper(b, q_ins(q, 1), q_ins(q, 2), true);
    for (int i = 0; i < g.len; i++) vec_push(out, g.data[i]);
    sv_push(b, &out, "ldir");
    return out;
}

/* ====================================================================
 * Fixed16 (f16) — _f16.py. Most ops delegate to Bits32 after a numeric
 * f16_to_32bit rewrite of the Quad's operands.
 * ==================================================================== */

/* f16_to_32bit (_f16.py:189-202): for each numeric (is_float) arg,
 * replace it with str((de << 16) | hl) of Fixed16.f16(arg). Returns a
 * rewritten Quad (same instr, copied args). */
static Quad *f16_to_32bit(Backend *b, Quad *q) {
    Quad *r = arena_alloc(b->arena, sizeof(Quad));
    r->instr = q->instr;
    r->nargs = q->nargs;
    r->args = arena_alloc(b->arena, (size_t)q->nargs * sizeof(char *));
    for (int i = 0; i < q->nargs; i++) {
        const char *a = q->args[i];
        if (s_is_float(a)) {
            unsigned DE, HL; s_f16(s_float_val(a), &DE, &HL);
            unsigned long packed = ((unsigned long)DE << 16) | HL;
            char buf[24];
            snprintf(buf, sizeof(buf), "%lu", packed);
            r->args[i] = arena_strdup(b->arena, buf);
        } else {
            r->args[i] = (char *)a;
        }
    }
    return r;
}

/* ---- Fixed16.get_oper (_f16.py:46-187) — parallels Bits32.get_oper
 * but uses is_float/cls.f16 instead of is_int/int32, and use_bc==
 * preserveHL. The non-float (label/temp) branch is identical shape. */
static StrVec fixed16_get_oper(Backend *b, const char *op1, const char *op2,
                               bool use_bc, bool reversed) {
    StrVec out = sv_new();
    const char *op = (op2 != NULL) ? op2 : op1;
    bool float1 = false;

    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    const char *hl = use_bc ? "bc" : "hl";

    if (s_is_float(op)) {
        float1 = true;
        double opf = s_float_val(op);
        if (indirect) {
            long iop = (long)opf & 0xFFFF;          /* int(op) & 0xFFFF */
            if (immediate) sv_pushf(b, &out, "ld hl, %ld", iop);
            else           sv_pushf(b, &out, "ld hl, (%ld)", iop);
            sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
            if (use_bc) { sv_push(b, &out, "ld b, h");
                          sv_push(b, &out, "ld c, l"); }
        } else {
            unsigned DE, HL; s_f16(opf, &DE, &HL);
            sv_pushf(b, &out, "ld de, %u", DE);
            sv_pushf(b, &out, "ld %s, %u", hl, HL);
        }
    } else {
        if (op[0] == '_') {
            if (immediate) sv_pushf(b, &out, "ld %s, %s", hl, op);
            else           sv_pushf(b, &out, "ld %s, (%s)", hl, op);
        } else {
            sv_pushf(b, &out, "pop %s", hl);
        }
        if (indirect) {
            sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
            if (use_bc) { sv_push(b, &out, "ld b, h");
                          sv_push(b, &out, "ld c, l"); }
        } else {
            if (op[0] == '_') sv_pushf(b, &out, "ld de, (%s + 2)", op);
            else              sv_push(b, &out, "pop de");
        }
    }

    if (op2 != NULL) {
        op = op1;
        indirect = op[0] == '*';
        if (indirect) op++;
        immediate = op[0] == '#';
        if (immediate) op++;

        if (s_is_float(op)) {
            double opf = s_float_val(op);
            if (indirect) {
                long iop = (long)opf;               /* int(op) */
                sv_push(b, &out, "exx");
                if (immediate) sv_pushf(b, &out, "ld hl, %ld", iop & 0xFFFF);
                else           sv_pushf(b, &out, "ld hl, (%ld)", iop & 0xFFFF);
                sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
                sv_push(b, &out, "push de");
                sv_push(b, &out, "push hl");
                sv_push(b, &out, "exx");
            } else {
                unsigned DE, HL; s_f16(opf, &DE, &HL);
                sv_pushf(b, &out, "ld bc, %u", DE);
                sv_push(b, &out, "push bc");
                sv_pushf(b, &out, "ld bc, %u", HL);
                sv_push(b, &out, "push bc");
            }
        } else {
            if (indirect) {
                sv_push(b, &out, "exx");
                if (op[0] == '_') {
                    if (immediate) sv_pushf(b, &out, "ld hl, %s", op);
                    else           sv_pushf(b, &out, "ld hl, (%s)", op);
                } else {
                    sv_push(b, &out, "pop hl");
                }
                sv_push(b, &out, s_runtime_call(b, RL_ILOAD32));
                sv_push(b, &out, "push de");
                sv_push(b, &out, "push hl");
                sv_push(b, &out, "exx");
            } else if (op[0] == '_') {              /* an address */
                if (float1 || op1[0] == '_') {
                    StrVec tmp = out; out = sv_new();
                    sv_pushf(b, &out, "ld hl, (%s + 2)", op);
                    sv_push(b, &out, "push hl");
                    sv_pushf(b, &out, "ld hl, (%s)", op);
                    sv_push(b, &out, "push hl");
                    for (int i = 0; i < tmp.len; i++) vec_push(out, tmp.data[i]);
                    vec_free(tmp);
                } else {
                    sv_pushf(b, &out, "ld bc, (%s + 2)", op);
                    sv_push(b, &out, "push bc");
                    sv_pushf(b, &out, "ld bc, (%s)", op);
                    sv_push(b, &out, "push bc");
                }
            } else {
                /* 2nd operand remains in the stack */
            }
        }
    }

    if (op2 != NULL && reversed)
        sv_push(b, &out, s_runtime_call(b, RL_SWAP32));

    return out;
}

/* Fixed16.f16_binary (_f16.py:204-212). */
static StrVec fixed16_f16_binary(Backend *b, Quad *q, const char *label,
                                 bool reversible) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    bool rev = reversible && !s_is_float(op1) && op1[0] != 't' && op2[0] == 't';
    StrVec out = fixed16_get_oper(b, op1, op2, false, rev);
    sv_push(b, &out, s_runtime_call(b, label));
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}

static StrVec emit_negf(Backend *b, Quad *q);   /* fwd (Float.negf) */

/* addf16/subf16/negf16 -> Bits32.* (cls.f16_to_32bit(ins)) */
static StrVec emit_addf16(Backend *b, Quad *q) {
    return emit_add32(b, f16_to_32bit(b, q));
}
static StrVec emit_subf16(Backend *b, Quad *q) {
    return emit_sub32(b, f16_to_32bit(b, q));
}
static StrVec emit_negf16(Backend *b, Quad *q) {
    return emit_neg32(b, f16_to_32bit(b, q));
}
/* mulf16 (_f16.py:235-261) */
static StrVec emit_mulf16(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    const char *other; double fv;
    if (s_f_ops(op1, op2, &other, &fv)) {
        if (fv == 1.0) {
            StrVec out = fixed16_get_oper(b, other, NULL, false, false);
            sv_push(b, &out, "push de");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (fv == -1.0) return emit_neg32(b, q);
        StrVec out = fixed16_get_oper(b, other, NULL, false, false);
        if (fv == 0.0) {
            sv_push(b, &out, "ld hl, 0");
            sv_push(b, &out, "ld e, h");
            sv_push(b, &out, "ld d, l");
            sv_push(b, &out, "push de");
            sv_push(b, &out, "push hl");
            return out;
        }
        vec_free(out);
    }
    return fixed16_f16_binary(b, q, RL_MULF16, false);
}
/* divf16 (_f16.py:263-284) */
static StrVec emit_divf16(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_float(op2)) {
        if (s_float_val(op2) == 1.0) {
            StrVec out = fixed16_get_oper(b, op1, NULL, false, false);
            sv_push(b, &out, "push de");
            sv_push(b, &out, "push hl");
            return out;
        }
        if (s_float_val(op2) == -1.0) return emit_negf(b, q);
    }
    return fixed16_f16_binary(b, q, RL_DIVF16, true);
}
/* modf16 (_f16.py:286-301) */
static StrVec emit_modf16(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_float(op2) && s_float_val(op2) == 1.0) {
        StrVec out = fixed16_get_oper(b, op1, NULL, false, false);
        sv_push(b, &out, "ld hl, 0");
        sv_push(b, &out, "push hl");
        sv_push(b, &out, "push hl");
        return out;
    }
    return fixed16_f16_binary(b, q, RL_MODF16, true);
}
/* comparisons/bool -> Bits32.* on f16_to_32bit */
static StrVec emit_ltf16(Backend *b, Quad *q) { return emit_lti32(b, f16_to_32bit(b, q)); }
static StrVec emit_gtf16(Backend *b, Quad *q) { return emit_gti32(b, f16_to_32bit(b, q)); }
static StrVec emit_lef16(Backend *b, Quad *q) { return emit_lei32(b, f16_to_32bit(b, q)); }
static StrVec emit_gef16(Backend *b, Quad *q) { return emit_gei32(b, f16_to_32bit(b, q)); }
static StrVec emit_eqf16(Backend *b, Quad *q) { return emit_eq32(b, f16_to_32bit(b, q)); }
static StrVec emit_nef16(Backend *b, Quad *q) { return emit_ne32(b, f16_to_32bit(b, q)); }
static StrVec emit_orf16(Backend *b, Quad *q) { return emit_or32(b, f16_to_32bit(b, q)); }
static StrVec emit_xorf16(Backend *b, Quad *q){ return emit_xor32(b, f16_to_32bit(b, q)); }
static StrVec emit_andf16(Backend *b, Quad *q){ return emit_and32(b, f16_to_32bit(b, q)); }
static StrVec emit_notf16(Backend *b, Quad *q){ return emit_not32(b, f16_to_32bit(b, q)); }
static StrVec emit_absf16(Backend *b, Quad *q){ return emit_abs32(b, f16_to_32bit(b, q)); }
/* loadf16 (_f16.py:417-426) */
static StrVec emit_loadf16(Backend *b, Quad *q) {
    StrVec out = fixed16_get_oper(b, q_ins(q, 2), NULL, false, false);
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
/* storef16 (_f16.py:428-441): rewrite immediate float -> packed int,
 * then delegate to Bits32.store32. ins[2] (Python 1-based incl instr)
 * == q->args[1] (C, instruction excluded == q_ins(q, 2)). */
static StrVec emit_storef16(Backend *b, Quad *q) {
    const char *value = q_ins(q, 2);
    if (s_is_float(value)) {
        unsigned DE, HL; s_f16(s_float_val(value), &DE, &HL);
        unsigned long packed = ((unsigned long)DE << 16) | HL;
        Quad *r = arena_alloc(b->arena, sizeof(Quad));
        r->instr = q->instr;
        r->nargs = q->nargs;
        r->args = arena_alloc(b->arena, (size_t)q->nargs * sizeof(char *));
        for (int i = 0; i < q->nargs; i++) r->args[i] = q->args[i];
        char buf[24]; snprintf(buf, sizeof(buf), "%lu", packed);
        if (q->nargs >= 2) r->args[1] = arena_strdup(b->arena, buf);
        q = r;
    }
    return emit_store32(b, q);
}
static StrVec emit_paramf16(Backend *b, Quad *q) {
    StrVec out = fixed16_get_oper(b, q_ins(q, 1), NULL, false, false);
    sv_push(b, &out, "push de");
    sv_push(b, &out, "push hl");
    return out;
}
/* Fixed16.fparamf16 (_f16.py:511-518): get_oper only (no push). */
static StrVec emit_fparamf16(Backend *b, Quad *q) {
    return fixed16_get_oper(b, q_ins(q, 1), NULL, false, false);
}
static StrVec emit_retf16(Backend *b, Quad *q) {
    StrVec out = fixed16_get_oper(b, q_ins(q, 1), NULL, false, false);
    sv_push(b, &out, "#pragma opt require hl,de");
    sv_pushf(b, &out, "jp %s", q_ins(q, 2));
    return out;
}

/* ====================================================================
 * Float (5-byte ZX FP) — _float.py.
 * ==================================================================== */

/* Float.fpop/fpush (_float.py:23-39). */
static void float_fpop(Backend *b, StrVec *out) {
    sv_push(b, out, "pop af");
    sv_push(b, out, "pop de");
    sv_push(b, out, "pop bc");
}
static void float_fpush(Backend *b, StrVec *out) {
    sv_push(b, out, "push bc");
    sv_push(b, out, "push de");
    sv_push(b, out, "push af");
}

/* Float.get_oper (_float.py:41-128). NO operand inversion. 1st op -> A DE BC. */
static StrVec float_get_oper(Backend *b, const char *op1, const char *op2) {
    StrVec out = sv_new();
    const char *op = (op2 != NULL) ? op2 : op1;

    bool indirect = op[0] == '*';
    if (indirect) op++;

    if (s_is_float(op)) {
        double opf = s_float_val(op);
        if (indirect) {
            long iop = (long)opf & 0xFFFF;          /* int(op) & 0xFFFF */
            sv_pushf(b, &out, "ld hl, (%ld)", iop);
            sv_push(b, &out, s_runtime_call(b, RL_ILOADF));
        } else {
            char C[8], DE[8], BC[8];
            z80h_immediate_float(opf, C, DE, BC);
            sv_pushf(b, &out, "ld a, %s", C);
            sv_pushf(b, &out, "ld de, %s", DE);
            sv_pushf(b, &out, "ld bc, %s", BC);
        }
    } else {
        if (indirect) {
            if (op[0] == '_') sv_pushf(b, &out, "ld hl, (%s)", op);
            else              sv_push(b, &out, "pop hl");
            sv_push(b, &out, s_runtime_call(b, RL_LOADF));
        } else {
            if (op[0] == '_') {
                sv_pushf(b, &out, "ld a, (%s)", op);
                sv_pushf(b, &out, "ld de, (%s + 1)", op);
                sv_pushf(b, &out, "ld bc, (%s + 3)", op);
            } else {
                float_fpop(b, &out);
            }
        }
    }

    if (op2 != NULL) {
        op = op1;
        if (s_is_float(op)) {
            char C[8], DE[8], BC[8];
            z80h_immediate_float(s_float_val(op), C, DE, BC);
            sv_pushf(b, &out, "ld hl, %s", BC);
            sv_push(b, &out, "push hl");
            sv_pushf(b, &out, "ld hl, %s", DE);
            sv_push(b, &out, "push hl");
            sv_pushf(b, &out, "ld h, %s", C);
            sv_push(b, &out, "push hl");
        } else if (op[0] == '*') {                  /* Indirect */
            op++;
            sv_push(b, &out, "exx");
            sv_push(b, &out, "ex af, af'");
            if (s_is_int(op)) {
                sv_pushf(b, &out, "ld hl, %ld", s_int_val(op));
            } else if (op[0] == '_') {
                sv_pushf(b, &out, "ld hl, (%s)", op);
            } else {
                sv_push(b, &out, "pop hl");
            }
            sv_push(b, &out, s_runtime_call(b, RL_ILOADF));
            float_fpush(b, &out);
            sv_push(b, &out, "ex af, af'");
            sv_push(b, &out, "exx");
        } else if (op[0] == '_') {
            if (s_is_float(op2)) {
                StrVec tmp = out; out = sv_new();
                sv_pushf(b, &out, "ld hl, %s + 4", op);
                sv_push(b, &out, s_runtime_call(b, RL_FP_PUSH_REV));
                for (int i = 0; i < tmp.len; i++) vec_push(out, tmp.data[i]);
                vec_free(tmp);
            } else {
                sv_pushf(b, &out, "ld hl, %s + 4", op);
                sv_push(b, &out, s_runtime_call(b, RL_FP_PUSH_REV));
            }
        } else {
            /* leave the op onto the stack */
        }
    }
    return out;
}

/* Float.float_binary (_float.py:134-142). */
static StrVec float_binary(Backend *b, Quad *q, const char *label) {
    StrVec out = float_get_oper(b, q_ins(q, 2), q_ins(q, 3));
    sv_push(b, &out, s_runtime_call(b, label));
    float_fpush(b, &out);
    return out;
}
/* Float.bool_binary (_float.py:218-225). */
static StrVec float_bool_binary(Backend *b, Quad *q, const char *label) {
    StrVec out = float_get_oper(b, q_ins(q, 2), q_ins(q, 3));
    sv_push(b, &out, s_runtime_call(b, label));
    sv_push(b, &out, "push af");
    return out;
}
/* addf (_float.py:144-157) */
static StrVec emit_addf(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    const char *other; double fv;
    if (s_f_ops(op1, op2, &other, &fv) && fv == 0.0) {
        StrVec out = float_get_oper(b, other, NULL);
        float_fpush(b, &out);
        return out;
    }
    return float_binary(b, q, RL_ADDF);
}
/* subf (_float.py:159-170) */
static StrVec emit_subf(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_float(op2) && s_float_val(op2) == 0.0) {
        StrVec out = float_get_oper(b, op1, NULL);
        float_fpush(b, &out);
        return out;
    }
    return float_binary(b, q, RL_SUBF);
}
/* mulf (_float.py:172-185) */
static StrVec emit_mulf(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    const char *other; double fv;
    if (s_f_ops(op1, op2, &other, &fv) && fv == 1.0) {
        StrVec out = float_get_oper(b, other, NULL);
        float_fpush(b, &out);
        return out;
    }
    return float_binary(b, q, RL_MULF);
}
/* divf (_float.py:187-198) */
static StrVec emit_divf(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_float(op2) && s_float_val(op2) == 1.0) {
        StrVec out = float_get_oper(b, op1, NULL);
        float_fpush(b, &out);
        return out;
    }
    return float_binary(b, q, RL_DIVF);
}
/* modf (_float.py:200-203) */
static StrVec emit_modf(Backend *b, Quad *q) {
    return float_binary(b, q, RL_MODF);
}
/* powf (_float.py:205-216) */
static StrVec emit_powf(Backend *b, Quad *q) {
    const char *op1 = q_ins(q, 2), *op2 = q_ins(q, 3);
    if (s_is_float(op2) && s_float_val(op2) == 1.0) {
        StrVec out = float_get_oper(b, op1, NULL);
        float_fpush(b, &out);
        return out;
    }
    return float_binary(b, q, RL_POWF);
}
static StrVec emit_ltf(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_LTF); }
static StrVec emit_gtf(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_GTF); }
static StrVec emit_lef(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_LEF); }
static StrVec emit_gef(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_GEF); }
static StrVec emit_eqf(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_EQF); }
static StrVec emit_nef(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_NEF); }
static StrVec emit_orf(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_ORF); }
static StrVec emit_xorf(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_XORF); }
static StrVec emit_andf(Backend *b, Quad *q) { return float_bool_binary(b, q, RL_ANDF); }
/* notf (_float.py:317-323) */
static StrVec emit_notf(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 2), NULL);
    sv_push(b, &out, s_runtime_call(b, RL_NOTF));
    sv_push(b, &out, "push af");
    return out;
}
/* negf (_float.py:325-331) */
static StrVec emit_negf(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 2), NULL);
    sv_push(b, &out, s_runtime_call(b, RL_NEGF));
    float_fpush(b, &out);
    return out;
}
/* absf (_float.py:333-339) */
static StrVec emit_absf(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 2), NULL);
    sv_push(b, &out, "res 7, e");
    float_fpush(b, &out);
    return out;
}
/* loadf (_float.py:341-349) */
static StrVec emit_loadf(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 2), NULL);
    float_fpush(b, &out);
    return out;
}
/* storef (_float.py:351-383) */
static StrVec emit_storef(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 2), NULL);
    const char *op = q_ins(q, 1);
    bool indirect = op[0] == '*';
    if (indirect) op++;
    bool immediate = op[0] == '#';
    if (immediate) op++;

    if (s_is_int(op) || op[0] == '_' || op[0] == '.') {
        char ibuf[24];
        if (s_is_int(op)) {
            snprintf(ibuf, sizeof(ibuf), "%ld", s_int_val(op) & 0xFFFF);
            op = ibuf;
        }
        if (indirect) sv_pushf(b, &out, "ld hl, (%s)", op);
        else          sv_pushf(b, &out, "ld hl, %s", op);
    } else {
        sv_push(b, &out, "pop hl");
        if (indirect) {
            sv_push(b, &out, s_runtime_call(b, RL_ISTOREF));
            return out;
        }
    }
    sv_push(b, &out, s_runtime_call(b, RL_STOREF));
    return out;
}
static StrVec emit_paramf(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 1), NULL);
    float_fpush(b, &out);
    return out;
}
/* Float.fparamf (_float.py:450-457): get_oper only — a __FASTCALL__
 * float param is loaded into A/ED/CB, not pushed. */
static StrVec emit_fparamf(Backend *b, Quad *q) {
    return float_get_oper(b, q_ins(q, 1), NULL);
}

/* _in (generic.py:328-335): Bits16.get_oper(ins[1]); ld b,h; ld c,l;
 * in a,(c); push af. */
static StrVec emit_in(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 1), NULL, false);
    sv_push(b, &out, "ld b, h");
    sv_push(b, &out, "ld c, l");
    sv_push(b, &out, "in a, (c)");
    sv_push(b, &out, "push af");
    return out;
}

/* _out (generic.py:317-325): Bits8.get_oper(ins[2]); extend
 * Bits16.get_oper(ins[1]); ld b,h; ld c,l; out (c),a. */
static StrVec emit_out(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
    StrVec p = bits16_get_oper(b, q_ins(q, 1), NULL, false);
    for (int i = 0; i < p.len; i++) vec_push(out, p.data[i]);
    vec_free(p);
    sv_push(b, &out, "ld b, h");
    sv_push(b, &out, "ld c, l");
    sv_push(b, &out, "out (c), a");
    return out;
}

/* Bits8 unary (_8bit.py:844-877): get_oper(ins[2]) in A, op, push af. */
static StrVec emit_not8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, "sub 1");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_bnot8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, "cpl");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_neg8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, "neg");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_abs8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, s_runtime_call(b, RL_ABS8));
    sv_push(b, &out, "push af");
    return out;
}

/* Bits16 unary (_16bit.py:833-865): get_oper(ins[2]) in HL, op, push hl
 * (not16 pushes af). */
static StrVec emit_not16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "sub 1");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_bnot16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, s_runtime_call(b, RL_BNOT16));
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_neg16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, s_runtime_call(b, RL_NEGHL));
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_abs16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), NULL, false);
    sv_push(b, &out, s_runtime_call(b, RL_ABS16));
    sv_push(b, &out, "push hl");
    return out;
}
static StrVec emit_retf(Backend *b, Quad *q) {
    StrVec out = float_get_oper(b, q_ins(q, 1), NULL);
    sv_push(b, &out, "#pragma opt require a,bc,de");
    sv_pushf(b, &out, "jp %s", q_ins(q, 2));
    return out;
}

/* ==================================================================== *
 *  S5.5 — 8/16-bit integer comparisons (the IF/FOR/WHILE condition      *
 *  producers; Quad("X", t, t1, t2): ins[2]=t1, ins[3]=t2 — result is    *
 *  PUSHed, not stored. _8bit.py:486-660 / _16bit.py:479-620 verbatim).  *
 * ==================================================================== */

/* ---- Bits8 comparisons (_8bit.py:486-660) --------------------------- */
static StrVec emit_ltu8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, "cp h");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_lti8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, s_runtime_call(b, RL_LTI8));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gtu8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, "cp h");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gti8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, s_runtime_call(b, RL_LTI8));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_eq8(Backend *b, Quad *q) {
    StrVec out;
    const char *op3 = q_ins(q, 3);
    if (s_is_int(op3)) {
        out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
        int n = s_int8(s_int_val(op3));
        if (n) {
            if (n == 1) sv_push(b, &out, "dec a");
            else        sv_pushf(b, &out, "sub %d", n);
        }
    } else {
        out = bits8_get_oper(b, q_ins(q, 2), op3, false);
        sv_push(b, &out, "sub h");
    }
    sv_push(b, &out, "sub 1");                      /* Carry only if 0 */
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_leu8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, "sub h");                       /* Carry if H > A */
    sv_push(b, &out, "ccf");                         /* Carry if H <= A */
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_lei8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, s_runtime_call(b, RL_LEI8));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_geu8(Backend *b, Quad *q) {
    StrVec out;
    const char *op3 = q_ins(q, 3);
    if (s_is_int(op3)) {
        out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
        int n = s_int8(s_int_val(op3));
        if (n) sv_pushf(b, &out, "sub %d", n);
        else   sv_push(b, &out, "cp a");
    } else {
        out = bits8_get_oper(b, q_ins(q, 2), op3, false);
        sv_push(b, &out, "sub h");
    }
    sv_push(b, &out, "ccf");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gei8(Backend *b, Quad *q) {
    StrVec out = bits8_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, s_runtime_call(b, RL_LEI8));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_ne8(Backend *b, Quad *q) {
    StrVec out;
    const char *op3 = q_ins(q, 3);
    if (s_is_int(op3)) {
        out = bits8_get_oper(b, q_ins(q, 2), NULL, false);
        int n = s_int8(s_int_val(op3));
        if (n) {
            if (n == 1) sv_push(b, &out, "dec a");
            else        sv_pushf(b, &out, "sub %d", n);
        }
    } else {
        out = bits8_get_oper(b, q_ins(q, 2), op3, false);
        sv_push(b, &out, "sub h");
    }
    sv_push(b, &out, "push af");
    return out;
}

/* ---- Bits16 comparisons (_16bit.py:479-620) ------------------------- */
static StrVec emit_ltu16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_lti16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, s_runtime_call(b, RL_LTI16));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gtu16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gti16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, s_runtime_call(b, RL_LTI16));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_leu16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, de");                  /* Carry if A > B */
    sv_push(b, &out, "ccf");                         /* Carry if A <= B */
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_lei16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, s_runtime_call(b, RL_LEI16));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_geu16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, "or a");
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "ccf");
    sv_push(b, &out, "sbc a, a");
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_gei16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), true);
    sv_push(b, &out, s_runtime_call(b, RL_LEI16));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_eq16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, s_runtime_call(b, RL_EQ16));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_ne16(Backend *b, Quad *q) {
    StrVec out = bits16_get_oper(b, q_ins(q, 2), q_ins(q, 3), false);
    sv_push(b, &out, "or a");                        /* reset carry */
    sv_push(b, &out, "sbc hl, de");
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "push af");
    return out;
}

/* ==================================================================== *
 *  S5.5 — FLOW CONTROL                                                  *
 * ==================================================================== */

/* 0-based Quad arg accessor (Python ins[1]==first arg==args[0]). The
 * S5.5 emitters below index args directly; qa(q,0)==ins[1], qa(q,1)==
 * ins[2] — equivalently q_ins(q,1)/q_ins(q,2). */
static const char *qa(Quad *q, int i) {
    if (i < 0 || i >= q->nargs) return "";
    return q->args[i];
}

/* ---- _jump (generic.py:393-395): ["jp %s" % str(ins[1])] ------------- */
static StrVec emit_jump(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "jp %s", qa(q, 0));
    return out;
}

/* ---- _ret (generic.py:398-400): ["jp %s" % str(ins[1])] -------------- *
 * The parameterless RET ic (Quad("ret", addr)). ins[1] == qa(q,0). */
static StrVec emit_ret(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "jp %s", qa(q, 0));
    return out;
}

/* ---- _call (generic.py:403-431) -------------------------------------- *
 * Quad("call", label, num). ins[1]=label (qa 0), ins[2]=num (qa 1).
 * int(ins[2]) drives the result-push: 1->push af; >4->Float.fpush();
 * >2->push de; >1->push hl. ValueError -> no push. */
static StrVec emit_call(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "call %s", qa(q, 0));
    const char *nstr = qa(q, 1);
    if (s_is_int(nstr)) {
        long val = s_int_val(nstr);
        if (val == 1) {
            sv_push(b, &out, "push af");          /* Byte */
        } else if (val > 4) {
            float_fpush(b, &out);
        } else {
            if (val > 2) sv_push(b, &out, "push de");
            if (val > 1) sv_push(b, &out, "push hl");
        }
    }
    return out;
}

/* The jzero/jnzero/jgezero idiom (capture §2c). Quad shape:
 *   Quad("jzeroX", t, label) -> ins[1]=t (qa 0), ins[2]=label (qa 1).
 * Integer modules gate the constant-fold on is_int (rejects "1.0"); the
 * float/f16 modules gate on is_float (accepts "1" and "1.0"). */

/* ---- Bits8.jzero8/jnzero8/jgezerou8/jgezeroi8 (_8bit.py:1103-1155) ---- */
static StrVec emit_jzero8(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) == 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;                               /* != 0 -> [] */
    }
    StrVec out = bits8_get_oper(b, value, NULL, false);
    sv_push(b, &out, "or a");
    sv_pushf(b, &out, "jp z, %s", qa(q, 1));
    return out;
}
static StrVec emit_jnzero8(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) != 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits8_get_oper(b, value, NULL, false);
    sv_push(b, &out, "or a");
    sv_pushf(b, &out, "jp nz, %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezerou8(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    StrVec out = sv_new();
    if (!s_is_int(value)) { vec_free(out); out = bits8_get_oper(b, value, NULL, false); }
    sv_pushf(b, &out, "jp %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezeroi8(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) >= 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits8_get_oper(b, value, NULL, false);
    sv_push(b, &out, "add a, a");                  /* sign -> carry */
    sv_pushf(b, &out, "jp nc, %s", qa(q, 1));
    return out;
}

/* ---- Bits16.jzero16/jnzero16/jgezerou16/jgezeroi16 (_16bit.py:1070-1148) */
static StrVec emit_jzero16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) == 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits16_get_oper(b, value, NULL, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_pushf(b, &out, "jp z, %s", qa(q, 1));
    return out;
}
static StrVec emit_jnzero16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) != 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits16_get_oper(b, value, NULL, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_pushf(b, &out, "jp nz, %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezerou16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    StrVec out = sv_new();
    if (!s_is_int(value)) { vec_free(out); out = bits16_get_oper(b, value, NULL, false); }
    sv_pushf(b, &out, "jp %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezeroi16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) >= 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits16_get_oper(b, value, NULL, false);
    sv_push(b, &out, "add hl, hl");                /* sign -> carry */
    sv_pushf(b, &out, "jp nc, %s", qa(q, 1));
    return out;
}

/* ---- Bits32.jzero32/jnzero32/jgezerou32/jgezeroi32 (_32bit.py:916-1000) */
static StrVec emit_jzero32(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) == 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits32_get_oper(b, value, NULL, false, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "or d");
    sv_pushf(b, &out, "jp z, %s", qa(q, 1));
    return out;
}
static StrVec emit_jnzero32(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) != 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits32_get_oper(b, value, NULL, false, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "or d");
    sv_pushf(b, &out, "jp nz, %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezerou32(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    StrVec out = sv_new();
    if (!s_is_int(value)) { vec_free(out); out = bits32_get_oper(b, value, NULL, false, false); }
    sv_pushf(b, &out, "jp %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezeroi32(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_int(value)) {
        StrVec out = sv_new();
        if (s_int_val(value) >= 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = bits32_get_oper(b, value, NULL, false, false);
    sv_push(b, &out, "ld a, d");
    sv_push(b, &out, "add a, a");                  /* sign -> carry */
    sv_pushf(b, &out, "jp nc, %s", qa(q, 1));
    return out;
}

/* ---- Fixed16.jzerof16/jnzerof16/jgezerof16 (_f16.py:444-493) --------- *
 * fixed-point gates on is_float; jgezerof16 has NO "<0 -> return []"
 * early-out (capture §2d quirk) — it falls through to get_oper. */
static StrVec emit_jzerof16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_float(value)) {
        StrVec out = sv_new();
        if (s_float_val(value) == 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = fixed16_get_oper(b, value, NULL, false, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "or d");
    sv_pushf(b, &out, "jp z, %s", qa(q, 1));
    return out;
}
static StrVec emit_jnzerof16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_float(value)) {
        StrVec out = sv_new();
        if (s_float_val(value) != 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = fixed16_get_oper(b, value, NULL, false, false);
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "or d");
    sv_pushf(b, &out, "jp nz, %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezerof16(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    StrVec out;
    if (s_is_float(value) && s_float_val(value) >= 0) {
        out = sv_new();
        sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;                               /* NB: no [] for <0 */
    }
    out = fixed16_get_oper(b, value, NULL, false, false);
    sv_push(b, &out, "ld a, d");
    sv_push(b, &out, "add a, a");                  /* sign -> carry */
    sv_pushf(b, &out, "jp nc, %s", qa(q, 1));
    return out;
}

/* ---- Float.jzerof/jnzerof/jgezerof (_float.py:386-433) --------------- *
 * the named S5.4 nef.bas residual. 5-byte ZX FP zero-test is
 * ld a,c / or l / or h / or e / or d; sign byte = e. jgezerof shares the
 * f16 fall-through quirk (no [] for <0). */
static StrVec emit_jzerof(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_float(value)) {
        StrVec out = sv_new();
        if (s_float_val(value) == 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = float_get_oper(b, value, NULL);
    sv_push(b, &out, "ld a, c");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or h");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "or d");
    sv_pushf(b, &out, "jp z, %s", qa(q, 1));
    return out;
}
static StrVec emit_jnzerof(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    if (s_is_float(value)) {
        StrVec out = sv_new();
        if (s_float_val(value) != 0) sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;
    }
    StrVec out = float_get_oper(b, value, NULL);
    sv_push(b, &out, "ld a, c");
    sv_push(b, &out, "or l");
    sv_push(b, &out, "or h");
    sv_push(b, &out, "or e");
    sv_push(b, &out, "or d");
    sv_pushf(b, &out, "jp nz, %s", qa(q, 1));
    return out;
}
static StrVec emit_jgezerof(Backend *b, Quad *q) {
    const char *value = qa(q, 0);
    StrVec out;
    if (s_is_float(value) && s_float_val(value) >= 0) {
        out = sv_new();
        sv_pushf(b, &out, "jp %s", qa(q, 1));
        return out;                               /* NB: no [] for <0 */
    }
    out = float_get_oper(b, value, NULL);
    sv_push(b, &out, "ld a, e");                   /* sign from mantissa */
    sv_push(b, &out, "add a, a");                  /* sign -> carry */
    sv_pushf(b, &out, "jp nc, %s", qa(q, 1));
    return out;
}

/* ---- _label (generic.py:92-94): ["%s:" % str(ins[1])] ----------------- */
static StrVec emit_label(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "%s:", q->args[0]);
    return out;
}

/* ---- _deflabel (generic.py:97-99): ["%s EQU %s"] --------------------- */
static StrVec emit_deflabel(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "%s EQU %s", q->args[0], q->args[1]);
    return out;
}

/* ---- _enter (generic.py:433-469) ------------------------------------- *
 * Quad("enter", N|"__fastcall__"). The FUNCTION/SUB prologue. fastcall ->
 * no frame ([]). Otherwise build the IX frame (push ix; ld ix,0;
 * add ix,sp) then reserve `size_bytes` zeroed local space: <7 via
 * ld hl,0 + push hl repeated (+inc sp if odd); >=7 via ld hl,-N /
 * add hl,sp / ld sp,hl + an ldir zero-fill. IDX_REG == "ix"
 * (common.py:121). S5.7a port; faithful byte-for-byte to _enter. */
static StrVec emit_enter(Backend *b, Quad *q) {
    StrVec out = sv_new();
    const char *a = q_ins(q, 1);
    if (strcmp(a, "__fastcall__") == 0)
        return out;

    sv_push(b, &out, "push ix");
    sv_push(b, &out, "ld ix, 0");
    sv_push(b, &out, "add ix, sp");

    long size_bytes = s_int_val(a);
    if (size_bytes != 0) {
        if (size_bytes < 7) {
            sv_push(b, &out, "ld hl, 0");
            for (long i = 0; i < (size_bytes >> 1); i++)
                sv_push(b, &out, "push hl");
            if (size_bytes % 2) {
                sv_push(b, &out, "push hl");
                sv_push(b, &out, "inc sp");
            }
        } else {
            sv_pushf(b, &out, "ld hl, -%ld", size_bytes);
            sv_push(b, &out, "add hl, sp");
            sv_push(b, &out, "ld sp, hl");
            sv_push(b, &out, "ld (hl), 0");
            sv_pushf(b, &out, "ld bc, %ld", size_bytes - 1);
            sv_push(b, &out, "ld d, h");
            sv_push(b, &out, "ld e, l");
            sv_push(b, &out, "inc de");
            sv_push(b, &out, "ldir");
        }
    }
    return out;
}

/* ---- _leave (generic.py:472-535) ------------------------------------- *
 * Quad("leave", N|"__fastcall__"). "__fastcall__" -> ["ret"]. Otherwise
 * pop N param bytes off the stack then ret. IDX_REG == "ix"
 * (common.py:121). FLAG_use_function_exit gates the shared __EXIT_FUNCTION
 * trampoline for nbytes > 11. S5.5 RETURN only ever hits __fastcall__;
 * the byte-popping arms are ported faithfully for FunctionTranslator. */
static StrVec emit_leave(Backend *b, Quad *q) {
    StrVec out = sv_new();
    const char *a = q_ins(q, 1);
    if (strcmp(a, "__fastcall__") == 0) {
        sv_push(b, &out, "ret");
        return out;
    }
    long nbytes = s_int_val(a);
    if (nbytes == 0) {
        sv_push(b, &out, "ld sp, ix");
        sv_push(b, &out, "pop ix");
        sv_push(b, &out, "ret");
        return out;
    }
    if (nbytes == 1) {
        sv_push(b, &out, "ld sp, ix");
        sv_push(b, &out, "pop ix");
        sv_push(b, &out, "inc sp");
        sv_push(b, &out, "ret");
        return out;
    }
    if (nbytes <= 11) {
        sv_push(b, &out, "ld sp, ix");
        sv_push(b, &out, "pop ix");
        sv_push(b, &out, "exx");
        sv_push(b, &out, "pop hl");
        for (long i = 0; i < (nbytes >> 1) - 1; i++)
            sv_push(b, &out, "pop bc");
        if (nbytes & 1) sv_push(b, &out, "inc sp");
        sv_push(b, &out, "ex (sp), hl");
        sv_push(b, &out, "exx");
        sv_push(b, &out, "ret");
        return out;
    }
    if (!b->flag_use_function_exit) {
        b->flag_use_function_exit = true;
        sv_push(b, &out, "exx");
        sv_pushf(b, &out, "ld hl, %ld", nbytes);
        sv_push(b, &out, "__EXIT_FUNCTION:");
        sv_push(b, &out, "ld sp, ix");
        sv_push(b, &out, "pop ix");
        sv_push(b, &out, "pop de");
        sv_push(b, &out, "add hl, sp");
        sv_push(b, &out, "ld sp, hl");
        sv_push(b, &out, "push de");
        sv_push(b, &out, "exx");
        sv_push(b, &out, "ret");
    } else {
        sv_push(b, &out, "exx");
        sv_pushf(b, &out, "ld hl, %ld", nbytes);
        sv_push(b, &out, "jp __EXIT_FUNCTION");
    }
    return out;
}

/* ---- _var (generic.py:141-147) --------------------------------------- *
 * ["%s:" % ins[1], "DEFB %s" % ((int(ins[2])-1)*"00, " + "00")]. */
static StrVec emit_var(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "%s:", q->args[0]);
    long sz = s_int_val(q->args[1]);
    /* (sz-1) copies of "00, " then "00" */
    long reps = sz - 1; if (reps < 0) reps = 0;
    size_t need = (size_t)reps * 4 + 3;
    char *defb = arena_alloc(b->arena, need);
    size_t w = 0;
    for (long i = 0; i < reps; i++) { memcpy(defb + w, "00, ", 4); w += 4; }
    memcpy(defb + w, "00", 3); /* incl NUL */
    sv_pushf(b, &out, "DEFB %s", defb);
    return out;
}

/* Faithful evaluator for Python's str(list[str]) repr — exactly the form
 * generic._vard/_varx pass to eval(): "['a', 'b', ...]" (single-quoted
 * elements; the S5.3 element strings never embed quotes/backslashes).
 * Writes element pointers (arena copies) into *out, returns count. */
static int eval_str_list(Backend *b, const char *s, char **out, int cap) {
    int n = 0;
    const char *p = s;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p) {
        while (*p == ' ' || *p == ',' ) p++;
        if (*p == ']' || *p == '\0') break;
        if (*p != '\'') { p++; continue; }
        p++; /* opening quote */
        const char *start = p;
        while (*p && *p != '\'') p++;
        size_t len = (size_t)(p - start);
        if (*p == '\'') p++;
        if (n < cap) {
            char *e = arena_alloc(b->arena, len + 1);
            memcpy(e, start, len);
            e[len] = '\0';
            out[n++] = e;
        }
    }
    return n;
}

/* Dynamic variant — counts items first, allocates an arena array sized
 * to exactly that many. Used for array DATA images which can exceed the
 * 64-entry fixed-cap of the static callers (e.g. DIM a(10,10) = 605
 * elements). The returned array is arena-owned; *out_n is element count. */
static char **eval_str_list_dyn(Backend *b, const char *s, int *out_n) {
    /* Pass 1: count single-quoted tokens (mirrors the parse below). */
    int count = 0;
    const char *p = s;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    {
        const char *q = p;
        while (*q) {
            while (*q == ' ' || *q == ',') q++;
            if (*q == ']' || *q == '\0') break;
            if (*q != '\'') { q++; continue; }
            q++;
            while (*q && *q != '\'') q++;
            if (*q == '\'') q++;
            count++;
        }
    }
    char **arr = count > 0
        ? (char **)arena_alloc(b->arena, sizeof(char *) * (size_t)count)
        : NULL;
    int n = eval_str_list(b, s, arr, count);
    *out_n = n;
    return arr;
}

/* RE_HEXA (generic.py): ^[0-9A-F]+$ on the uppercased token. */
static bool s_re_hexa(const char *x) {
    if (!*x) return false;
    for (const char *p = x; *p; p++) {
        char c = *p;
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

/* ---- _data (generic.py:102-138) -------------------------------------- *
 * Quad("data", TSUFFIX, list-repr). i8/u8 -> DEFB; i16/u16 -> DEFW;
 * i32/u32 -> per expr lo/hi DEFW pair; str/f branches ported for
 * fidelity. The S5.6 array DATA bound-ptr row uses u16. */
static StrVec emit_data(Backend *b, Quad *q) {
    StrVec out = sv_new();
    const char *t = q->args[0];
    int n = 0;
    char **items = eval_str_list_dyn(b, q->args[1], &n);

    const char *size;
    if (strcmp(t, "i8") == 0 || strcmp(t, "u8") == 0) {
        size = "B";
    } else if (strcmp(t, "i16") == 0 || strcmp(t, "u16") == 0) {
        size = "W";
    } else if (strcmp(t, "i32") == 0 || strcmp(t, "u32") == 0) {
        for (int i = 0; i < n; i++) {
            sv_pushf(b, &out, "DEFW (%s) & 0xFFFF", items[i]);
            sv_pushf(b, &out, "DEFW (%s) >> 16", items[i]);
        }
        return out;
    } else if (strcmp(t, "str") == 0) {
        size = "B";
        for (int i = 0; i < n; i++) {
            /* '"%s"' % x.replace('"','""') */
            const char *x = items[i];
            size_t xl = strlen(x), q2 = 0;
            for (size_t k = 0; k < xl; k++) if (x[k] == '"') q2++;
            char *s = arena_alloc(b->arena, xl + q2 + 3);
            size_t w = 0; s[w++] = '"';
            for (size_t k = 0; k < xl; k++) {
                s[w++] = x[k];
                if (x[k] == '"') s[w++] = '"';
            }
            s[w++] = '"'; s[w] = '\0';
            sv_pushf(b, &out, "DEFB %s", s);
        }
        return out;
    } else if (strcmp(t, "f") == 0) {
        for (int i = 0; i < n; i++) {
            char Cs[8], DEs[8], HLs[8];
            z80h_immediate_float(strtod(items[i], NULL), Cs, DEs, HLs);
            sv_pushf(b, &out, "DEFB %s", Cs);
            sv_pushf(b, &out, "DEFW %s, %s", DEs, HLs);
        }
        return out;
    } else {
        fprintf(stderr, "zxbc: _data unimplemented size '%s'\n", t);
        return out;
    }
    for (int i = 0; i < n; i++) sv_pushf(b, &out, "DEF%s %s", size, items[i]);
    return out;
}

/* ---- _vard (generic.py:180-208) -------------------------------------- */
static StrVec emit_vard(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "%s:", q->args[0]);
    int n = 0;
    char **items = eval_str_list_dyn(b, q->args[1], &n);
    for (int i = 0; i < n; i++) {
        const char *x = items[i];
        if (x[0] == '#') {                       /* literal */
            const char *sizt = (x[1] == '#') ? "W" : "B";
            const char *q2 = x;
            while (*q2 == '#') q2++;              /* lstrip("#") */
            sv_pushf(b, &out, "DEF%s %s", sizt, q2);
            continue;
        }
        /* hex number — upper() then RE_HEXA assert */
        size_t l = strlen(x);
        char *u = arena_alloc(b->arena, l + 2);
        for (size_t k = 0; k < l; k++) {
            char c = x[k];
            u[k] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        u[l] = '\0';
        if (!s_re_hexa(u)) {
            fprintf(stderr, "zxbc: _vard expected hex, got \"%s\"\n", u);
            continue;
        }
        const char *sizt = (l <= 2) ? "B" : "W";
        if (u[0] > '9') {                        /* not a number? prefix 0 */
            memmove(u + 1, u, l + 1);
            u[0] = '0';
        }
        sv_pushf(b, &out, "DEF%s %sh", sizt, u);
    }
    return out;
}

/* ---- _varx (generic.py:150-177) -------------------------------------- */
static StrVec emit_varx(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "%s:", q->args[0]);
    const char *t = q->args[1];                  /* type-size */
    int n = 0;
    char **items = eval_str_list_dyn(b, q->args[2], &n);

    const char *size;
    if (strcmp(t, "i8") == 0 || strcmp(t, "u8") == 0) {
        size = "B";
    } else if (strcmp(t, "i16") == 0 || strcmp(t, "u16") == 0) {
        size = "W";
    } else if (strcmp(t, "i32") == 0 || strcmp(t, "u32") == 0) {
        size = "W";
        char *z[128]; int zn = 0;
        for (int i = 0; i < n && zn + 1 < 128; i++) {
            char buf[256];
            snprintf(buf, sizeof(buf), "(%s) & 0xFFFF", items[i]);
            z[zn++] = arena_strdup(b->arena, buf);
            snprintf(buf, sizeof(buf), "(%s) >> 16", items[i]);
            z[zn++] = arena_strdup(b->arena, buf);
        }
        for (int i = 0; i < zn; i++) sv_pushf(b, &out, "DEFW %s", z[i]);
        return out;
    } else {
        fprintf(stderr, "zxbc: _varx unimplemented vard size: %s\n", t);
        return out;
    }
    for (int i = 0; i < n; i++) sv_pushf(b, &out, "DEF%s %s", size, items[i]);
    return out;
}

/* YY_TYPES[tB] size (common.py:62-72) for the S5.3 numeric types. */
static int s_yy_size(const char *t) {
    if (!strcmp(t, "bool") || !strcmp(t, "u8") || !strcmp(t, "i8")) return 1;
    if (!strcmp(t, "u16") || !strcmp(t, "i16")) return 2;
    if (!strcmp(t, "u32") || !strcmp(t, "i32") || !strcmp(t, "f16")) return 4;
    if (!strcmp(t, "f")) return 5;
    return 0;
}

/* get_bytes_size (common.py:296-... == len(get_bytes(elements))). Per
 * element: "##x" -> 2 bytes; "#x" -> 1 byte; a hex token -> 1 byte if
 * len<=2 else 2 bytes (common.py:get_bytes: appends low byte always, a
 * second byte iff len(x) > 2). Faithful byte-count, no expansion. */
static int s_get_bytes_size(char **items, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) {
        const char *x = items[i];
        if (x[0] == '#' && x[1] == '#')      total += 2;
        else if (x[0] == '#')                total += 1;
        else                                 total += (strlen(x) > 2) ? 2 : 1;
    }
    return total;
}

/* AT_END.extend(_vard(Quad(VARD, label, listrepr))) — runs emit_vard on
 * a synthetic VARD quad and appends every produced line to common.AT_END
 * (b->at_end), exactly the Python deferral (generic.py:223/245/272 +
 * main.py:684-690 emit_epilogue flush; the C flush is
 * backend_emit_epilogue:3595). */
static void s_at_end_vard(Backend *b, const char *label,
                          const char *list_repr) {
    const char *vargs[2] = { label, list_repr };
    Quad *vq = quad_new(b->arena, "vard", 2, vargs);
    StrVec vd = emit_vard(b, vq);
    for (int i = 0; i < vd.len; i++) vec_push(b->at_end, vd.data[i]);
    vec_free(vd);
}

/* ---- _lvarx (generic.py:211-233) ------------------------------------- *
 * Defines a local variable (CONSTEXPR default). ins[1]=offset,
 * ins[2]=TSUFFIX, ins[3]=list-repr of value bytes. The backing varx
 * table is deferred to AT_END under a fresh tmp_label; the body code
 * ldir-copies len(l)*YY_TYPES[suffix] bytes from that label to
 * (ix - offset). IDX_REG == "ix" (common.py:121). */
static StrVec emit_lvarx(Backend *b, Quad *q) {
    StrVec out = sv_new();
    int n = 0;
    char **items = eval_str_list_dyn(b, q_ins(q, 3), &n);  /* l = eval(ins[3]) */
    (void)items;
    char *label = s_tmp_label(b);
    long offset = s_int_val(q_ins(q, 1));
    const char *suffix = q_ins(q, 2);

    /* AT_END.extend(_varx(ins-with-ins[1]:=label)) */
    const char *xargs[3] = { label, suffix, q_ins(q, 4) };
    Quad *xq = quad_new(b->arena, "varx", 3, xargs);
    StrVec xd = emit_varx(b, xq);
    for (int i = 0; i < xd.len; i++) vec_push(b->at_end, xd.data[i]);
    vec_free(xd);

    sv_push(b, &out, "push ix");
    sv_push(b, &out, "pop hl");
    sv_pushf(b, &out, "ld bc, %ld", -offset);
    sv_push(b, &out, "add hl, bc");
    sv_push(b, &out, "ex de, hl");
    sv_pushf(b, &out, "ld hl, %s", label);
    sv_pushf(b, &out, "ld bc, %d", n * s_yy_size(suffix));
    sv_push(b, &out, "ldir");
    return out;
}

/* ---- _lvard (generic.py:237-255) ------------------------------------- *
 * Defines a local variable (literal default). ins[1]=offset, ins[2]=
 * list-repr of value bytes. Backing vard table deferred to AT_END;
 * body ldir-copies get_bytes_size(eval(ins[2])) bytes to (ix - offset). */
static StrVec emit_lvard(Backend *b, Quad *q) {
    StrVec out = sv_new();
    char *label = s_tmp_label(b);
    long offset = s_int_val(q_ins(q, 1));
    int n = 0;
    char **items = eval_str_list_dyn(b, q_ins(q, 2), &n);  /* eval(ins[2]) */

    s_at_end_vard(b, label, q_ins(q, 2));

    sv_push(b, &out, "push ix");
    sv_push(b, &out, "pop hl");
    sv_pushf(b, &out, "ld bc, %ld", -offset);
    sv_push(b, &out, "add hl, bc");
    sv_push(b, &out, "ex de, hl");
    sv_pushf(b, &out, "ld hl, %s", label);
    sv_pushf(b, &out, "ld bc, %d", s_get_bytes_size(items, n));
    sv_push(b, &out, "ldir");
    return out;
}

/* ---- _larrd (generic.py:259-314) ------------------------------------- *
 * Defines a local array. ins[1]=offset, ins[2]=idx-table list-repr,
 * ins[3]=elements byte-size, ins[4]=init-image list-repr ("[]" if none),
 * ins[5]=[lbound,ubound] label list-repr (len 0 or 2). Idx table deferred
 * to AT_END; pushes UBOUND/LBOUND ptrs (if any), the init-image label (if
 * initializing), then HL=-offset / DE=idx-label / BC=elements_size and
 * tail-calls the matching __ALLOC[_INITIALIZED]_LOCAL_ARRAY[_WITH_BOUNDS]
 * runtime. The Python InvalidIC on a bad bounds length is a hard contract
 * the visitor upholds; the C port keeps the same 0/2 acceptance. */
static StrVec emit_larrd(Backend *b, Quad *q) {
    StrVec out = sv_new();
    char *label = s_tmp_label(b);
    long offset = s_int_val(q_ins(q, 1));
    const char *elements_size = q_ins(q, 3);

    s_at_end_vard(b, label, q_ins(q, 2));   /* idx table */

    char *bounds[8];
    int bn = eval_str_list(b, q_ins(q, 5), bounds, 8);
    if (bn != 0 && bn != 2) {
        fprintf(stderr,
                "zxbc: _larrd bounds list length must be 0 or 2, not %s\n",
                q_ins(q, 5));
        return out;
    }
    bool have_bounds = false;
    for (int i = 0; i < bn; i++)
        if (strcmp(bounds[i], "0") != 0) { have_bounds = true; break; }

    if (have_bounds) {
        sv_pushf(b, &out, "ld hl, %s", bounds[1]);   /* UBOUND ptr */
        sv_push(b, &out, "push hl");
        sv_pushf(b, &out, "ld hl, %s", bounds[0]);   /* LBOUND ptr */
        sv_push(b, &out, "push hl");
    }

    bool must_init = strcmp(q_ins(q, 4), "[]") != 0;
    if (must_init) {
        char *label2 = s_tmp_label(b);
        s_at_end_vard(b, label2, q_ins(q, 4));        /* init image */
        sv_pushf(b, &out, "ld hl, %s", label2);
        sv_push(b, &out, "push hl");
    }

    sv_pushf(b, &out, "ld hl, %ld", -offset);
    sv_pushf(b, &out, "ld de, %s", label);
    sv_pushf(b, &out, "ld bc, %s", elements_size);

    if (must_init) {
        sv_push(b, &out, s_runtime_call(b,
            have_bounds ? RL_ALLOC_INITIALIZED_LOCAL_ARRAY_WITH_BOUNDS
                        : RL_ALLOC_INITIALIZED_LOCAL_ARRAY));
    } else {
        sv_push(b, &out, s_runtime_call(b,
            have_bounds ? RL_ALLOC_LOCAL_ARRAY_WITH_BOUNDS
                        : RL_ALLOC_LOCAL_ARRAY));
    }
    return out;
}

/* is_int_type (common.py:238-240): stype[0] in ('u','i'). */
static bool s_is_int_type(const char *t) { return t[0] == 'u' || t[0] == 'i'; }

/* normalize_boolean (common.py:308-316): Size strategy -> a single
 * __NORMALIZE_BOOLEAN call; else the 3-instr sub/sbc/inc sequence. */
static void s_normalize_boolean(Backend *b, StrVec *out) {
    if (b->opt_strategy == 0 /* OPT_STRATEGY_SIZE */) {
        sv_push(b, out, s_runtime_call(b,
                ZXBC_NAMESPACE ".__NORMALIZE_BOOLEAN"));
        return;
    }
    sv_push(b, out, "sub 1");
    sv_push(b, out, "sbc a, a");
    sv_push(b, out, "inc a");
}

/* to_byte (common.py:319-339). */
static void cast_to_byte(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "bool")) { s_normalize_boolean(b, out); return; }
    if (!strcmp(tA, "i8") || !strcmp(tA, "u8")) return;     /* [] */
    if (s_is_int_type(tA)) { sv_push(b, out, "ld a, l"); return; }
    if (!strcmp(tA, "f16")) { sv_push(b, out, "ld a, e"); return; }
    if (!strcmp(tA, "f")) {
        sv_push(b, out, s_runtime_call(b, RL_FTOU32REG));
        sv_push(b, out, "ld a, l");
        return;
    }
}
/* to_word (common.py:342-367). */
static void cast_to_word(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "bool")) s_normalize_boolean(b, out);
    if (!strcmp(tA, "bool") || !strcmp(tA, "u8")) {
        sv_push(b, out, "ld l, a");
        sv_push(b, out, "ld h, 0");
        return;
    }
    if (!strcmp(tA, "i8")) {
        sv_push(b, out, "ld l, a");
        sv_push(b, out, "add a, a");
        sv_push(b, out, "sbc a, a");
        sv_push(b, out, "ld h, a");
        return;
    }
    if (!strcmp(tA, "f16")) { sv_push(b, out, "ex de, hl"); return; }
    if (!strcmp(tA, "f")) {
        sv_push(b, out, s_runtime_call(b, RL_FTOU32REG));
        return;
    }
    /* i16/u16/i32/u32 -> [] */
}
/* to_long (common.py:370-418). */
static void cast_to_long(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "bool")) {
        s_normalize_boolean(b, out);
        sv_push(b, out, "ld l, a");
        sv_push(b, out, "ld h, 0");
        sv_push(b, out, "ld e, h");
        sv_push(b, out, "ld d, h");
        return;
    }
    if (!strcmp(tA, "i8") || !strcmp(tA, "u8")) {            /* byte to word */
        cast_to_word(b, out, tA);
        sv_push(b, out, "ld e, h");
        sv_push(b, out, "ld d, h");
        return;
    }
    if (!strcmp(tA, "i16")) {
        sv_push(b, out, "ld a, h");
        sv_push(b, out, "add a, a");
        sv_push(b, out, "sbc a, a");
        sv_push(b, out, "ld e, a");
        sv_push(b, out, "ld d, a");
        return;
    }
    if (!strcmp(tA, "f16")) {
        sv_push(b, out, "ex de, hl");
        sv_push(b, out, "ld de, 0");
        return;
    }
    if (!strcmp(tA, "u32") || !strcmp(tA, "i32")) return;    /* [] */
    if (!strcmp(tA, "u16")) { sv_push(b, out, "ld de, 0"); return; }
    if (!strcmp(tA, "f")) {
        sv_push(b, out, s_runtime_call(b, RL_FTOU32REG));
        return;
    }
}
/* to_fixed (common.py:421-448). */
static void cast_to_fixed(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "bool")) {
        cast_to_word(b, out, tA);
        sv_push(b, out, "ex de, hl");
        sv_push(b, out, "ld hl, 0");
        return;
    }
    if (s_is_int_type(tA)) {
        cast_to_word(b, out, tA);
        sv_push(b, out, "ex de, hl");
        sv_push(b, out, "ld hl, 0");
        return;
    }
    if (!strcmp(tA, "f")) {
        sv_push(b, out, s_runtime_call(b, RL_FTOF16REG));
        return;
    }
}
/* to_float (common.py:451-483). */
static void cast_to_float(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "f")) return;                            /* nothing */
    if (!strcmp(tA, "f16")) {
        sv_push(b, out, s_runtime_call(b, RL_F16TOFREG));
        return;
    }
    if (!strcmp(tA, "bool")) s_normalize_boolean(b, out);
    if (!strcmp(tA, "bool") || !strcmp(tA, "u8")) {
        sv_push(b, out, s_runtime_call(b, RL_U8TOFREG));
        return;
    }
    if (!strcmp(tA, "i8")) {
        sv_push(b, out, s_runtime_call(b, RL_I8TOFREG));
        return;
    }
    if (!strcmp(tA, "i16") || !strcmp(tA, "i32") ||
        !strcmp(tA, "u16") || !strcmp(tA, "u32")) {
        if (!strcmp(tA, "i16") || !strcmp(tA, "u16"))
            cast_to_long(b, out, tA);
        if (!strcmp(tA, "i16") || !strcmp(tA, "i32"))
            sv_push(b, out, s_runtime_call(b, RL_I32TOFREG));
        else
            sv_push(b, out, s_runtime_call(b, RL_U32TOFREG));
        return;
    }
}

/* ---- _cast (generic.py:339-390) -------------------------------------- */
static StrVec emit_cast(Backend *b, Quad *q) {
    const char *tA = q->args[1];   /* ins[2] */
    const char *tB = q->args[2];   /* ins[3] */
    const char *src = q->args[3];  /* ins[4] */

    int sB = s_yy_size(tB);
    int xsB = sB;

    if ((!strcmp(tA, "u8") || !strcmp(tA, "i8")) && !strcmp(tB, "bool")) {
        return sv_new();   /* bytes are booleans already */
    }

    StrVec out;
    if (!strcmp(tA, "u8") || !strcmp(tA, "i8") || !strcmp(tA, "bool")) {
        out = bits8_get_oper(b, src, NULL, false);
    } else if (!strcmp(tA, "u16") || !strcmp(tA, "i16")) {
        out = bits16_get_oper(b, src, NULL, false);
    } else if (!strcmp(tA, "u32") || !strcmp(tA, "i32")) {
        out = bits32_get_oper(b, src, NULL, false, false);
    } else if (!strcmp(tA, "f16")) {
        out = fixed16_get_oper(b, src, NULL, false, false);
    } else if (!strcmp(tA, "f")) {
        out = float_get_oper(b, src, NULL);
    } else {
        fprintf(stderr, "zxbc: invalid typecast from %s to %s\n", tA, tB);
        return sv_new();
    }

    if (!strcmp(tB, "u8") || !strcmp(tB, "i8")) {
        cast_to_byte(b, &out, tA);
    } else if (!strcmp(tB, "u16") || !strcmp(tB, "i16")) {
        cast_to_word(b, &out, tA);
    } else if (!strcmp(tB, "u32") || !strcmp(tB, "i32")) {
        cast_to_long(b, &out, tA);
    } else if (!strcmp(tB, "f16")) {
        cast_to_fixed(b, &out, tA);
    } else if (!strcmp(tB, "f")) {
        cast_to_float(b, &out, tA);
    } else {
        fprintf(stderr, "zxbc: invalid typecast from %s to %s\n", tA, tB);
        return out;
    }

    xsB += sB % 2;                       /* round up to even */
    if (xsB > 4) {
        float_fpush(b, &out);
    } else {
        if (xsB > 2) sv_push(b, &out, "push de");
        if (sB > 1)  sv_push(b, &out, "push hl");
        else         sv_push(b, &out, "push af");
    }
    return out;
}

/* ====================================================================
 * String class — verbatim port of src/arch/z80/backend/_str.py.
 *
 *   String.get_oper       _str.py:16-100
 *   String.free_sequence  _str.py:102-121
 *   String.addstr         _str.py:123-139
 *   String.ltstr/gtstr/lestr/gestr/eqstr/nestr  _str.py:141-199
 *   String.lenstr         _str.py:201-211
 *   String.loadstr        _str.py:213-222
 *   String.storestr       _str.py:224-253
 *   String.jzerostr       _str.py:255-281
 *   String.retstr         _str.py:311-321
 *   String.paramstr       _str.py:323-337
 *   String.fparamstr      _str.py:339-348
 *
 * 1st operand -> HL, 2nd -> DE. tmp1/tmp2 flag a popped-from-stack
 * (temporary) operand that must later be MEM_FREEd. runtime_call() ->
 * s_runtime_call (call {label} + REQUIRES add), exactly as Python's
 * common.runtime_call does. ==================================================================== */

/* String.get_oper (_str.py:16-100). op2 == NULL => single-operand form
 * (tmp2 stays false). reversed swaps op1/op2 extraction order. no_exaf
 * suppresses the A'-flag (ld a,N / xor a) tail. */
static StrVec str_get_oper(Backend *b, const char *op1, const char *op2,
                           bool reversed, bool no_exaf,
                           bool *tmp1_out, bool *tmp2_out) {
    StrVec output = sv_new();
    const char *o1 = op1, *o2 = op2;
    if (o2 != NULL && reversed) { const char *t = o1; o1 = o2; o2 = t; }

    bool tmp2 = false;
    if (o2 != NULL) {
        const char *val = o2;
        bool indirect = (val[0] == '*');
        if (indirect) val++;

        if (val[0] == '_')        sv_pushf(b, &output, "ld de, (%s)", val);
        else if (val[0] == '#')   sv_pushf(b, &output, "ld de, %s", val + 1);
        else if (val[0] == '$')   sv_push(b, &output, "pop de");
        else { sv_push(b, &output, "pop de"); tmp2 = true; }

        if (indirect)
            sv_push(b, &output, s_runtime_call(b, RL_LOAD_DE_DE));
    }

    StrVec tmp = sv_new();
    bool have_tmp = false;
    if (reversed) { tmp = output; output = sv_new(); have_tmp = true; }

    const char *val = o1;
    bool tmp1 = false;
    bool indirect = (val[0] == '*');
    if (indirect) val++;

    if (val[0] == '_')        sv_pushf(b, &output, "ld hl, (%s)", val);
    else if (val[0] == '#')   sv_pushf(b, &output, "ld hl, %s", val + 1);
    else if (val[0] == '$')   sv_push(b, &output, "pop hl");
    else { sv_push(b, &output, "pop hl"); tmp1 = true; }

    if (indirect) {
        sv_push(b, &output, "ld c, (hl)");
        sv_push(b, &output, "inc hl");
        sv_push(b, &output, "ld h, (hl)");
        sv_push(b, &output, "ld l, c");
    }

    if (reversed && have_tmp) {
        for (int i = 0; i < tmp.len; i++) vec_push(output, tmp.data[i]);
        vec_free(tmp);
    }

    if (!no_exaf) {
        if (tmp1 && tmp2)      sv_push(b, &output, "ld a, 3");
        else if (tmp1)         sv_push(b, &output, "ld a, 1");
        else if (tmp2)         sv_push(b, &output, "ld a, 2");
        else                   sv_push(b, &output, "xor a");
    }

    if (tmp1_out) *tmp1_out = tmp1;
    if (tmp2_out) *tmp2_out = tmp2;
    return output;
}

/* String.free_sequence (_str.py:102-121). FREEMEM sequence for 1/2 ops. */
static void str_free_sequence(Backend *b, StrVec *out, bool tmp1, bool tmp2) {
    if (!tmp1 && !tmp2) return;
    if (tmp1 && tmp2) {
        sv_push(b, out, "pop de");
        sv_push(b, out, "ex (sp), hl");
        sv_push(b, out, "push de");
        sv_push(b, out, s_runtime_call(b, RL_MEM_FREE));
        sv_push(b, out, "pop hl");
        sv_push(b, out, s_runtime_call(b, RL_MEM_FREE));
    } else {
        sv_push(b, out, "ex (sp), hl");
        sv_push(b, out, s_runtime_call(b, RL_MEM_FREE));
    }
    sv_push(b, out, "pop hl");
}

/* String.addstr (_str.py:123-139). */
static StrVec emit_addstr(Backend *b, Quad *q) {
    bool t1, t2;
    StrVec out = str_get_oper(b, q->args[1], q->args[2], false, true,
                              &t1, &t2);
    if (t1) sv_push(b, &out, "push hl");
    if (t2) sv_push(b, &out, "push de");
    sv_push(b, &out, s_runtime_call(b, RL_ADDSTR));
    str_free_sequence(b, &out, t1, t2);
    sv_push(b, &out, "push hl");
    return out;
}

/* String.{lt,gt,le,ge,eq,ne}str (_str.py:141-199): get_oper(ins[2],ins[3]);
 * runtime_call(STRx); push af. */
static StrVec str_cmp(Backend *b, Quad *q, const char *rl) {
    bool t1, t2;
    StrVec out = str_get_oper(b, q->args[1], q->args[2], false, false,
                              &t1, &t2);
    sv_push(b, &out, s_runtime_call(b, rl));
    sv_push(b, &out, "push af");
    return out;
}
static StrVec emit_ltstr(Backend *b, Quad *q) { return str_cmp(b, q, RL_STRLT); }
static StrVec emit_gtstr(Backend *b, Quad *q) { return str_cmp(b, q, RL_STRGT); }
static StrVec emit_lestr(Backend *b, Quad *q) { return str_cmp(b, q, RL_STRLE); }
static StrVec emit_gestr(Backend *b, Quad *q) { return str_cmp(b, q, RL_STRGE); }
static StrVec emit_eqstr(Backend *b, Quad *q) { return str_cmp(b, q, RL_STREQ); }
static StrVec emit_nestr(Backend *b, Quad *q) { return str_cmp(b, q, RL_STRNE); }

/* String.lenstr (_str.py:201-211). */
static StrVec emit_lenstr(Backend *b, Quad *q) {
    bool t1;
    StrVec out = str_get_oper(b, q->args[1], NULL, false, true, &t1, NULL);
    if (t1) sv_push(b, &out, "push hl");
    sv_push(b, &out, s_runtime_call(b, RL_STRLEN));
    str_free_sequence(b, &out, t1, false);
    sv_push(b, &out, "push hl");
    return out;
}

/* String.loadstr (_str.py:213-222). */
static StrVec emit_loadstr(Backend *b, Quad *q) {
    bool temporal;
    StrVec out = str_get_oper(b, q->args[1], NULL, false, true,
                              &temporal, NULL);
    if (!temporal) sv_push(b, &out, s_runtime_call(b, RL_LOADSTR));
    sv_push(b, &out, "push hl");
    return out;
}

/* String.storestr (_str.py:224-253). ins[1]=dest, ins[2]=src. Must
 * prepend '#' to a non-indirect dest (need its & address). Immediate
 * non-indirect dest is an InvalidIC (loud, no silent emit). */
static StrVec emit_storestr(Backend *b, Quad *q) {
    const char *op1 = q->args[0];
    bool indirect = (op1[0] == '*');
    if (indirect) op1++;
    bool immediate = (op1[0] == '#');
    if (immediate && !indirect) {
        fprintf(stderr,
            "zxbc: storestr does not allow immediate destination\n");
        return sv_new();
    }
    char *dst;
    if (!indirect) {
        size_t l = strlen(op1);
        dst = arena_alloc(b->arena, l + 2);
        dst[0] = '#';
        memcpy(dst + 1, op1, l + 1);
    } else {
        dst = arena_strdup(b->arena, op1);
    }
    bool t1, t2;
    StrVec out = str_get_oper(b, dst, q->args[1], false, true, &t1, &t2);
    if (!t2) sv_push(b, &out, s_runtime_call(b, RL_STORE_STR));
    else     sv_push(b, &out, s_runtime_call(b, RL_STORE_STR2));
    return out;
}

/* String.jzerostr (_str.py:255-281). */
static StrVec emit_jzerostr(Backend *b, Quad *q) {
    StrVec out = sv_new();
    bool disposable = false;
    const char *o1 = q->args[0];
    if (o1[0] == '_') {
        sv_pushf(b, &out, "ld hl, (%c)", o1[0]); /* _str.py:265 verbatim */
    } else {
        sv_push(b, &out, "pop hl");
        sv_push(b, &out, "push hl");
        disposable = true;
    }
    sv_push(b, &out, s_runtime_call(b, RL_STRLEN));
    if (disposable) {
        sv_push(b, &out, "ex (sp), hl");
        sv_push(b, &out, s_runtime_call(b, RL_MEM_FREE));
        sv_push(b, &out, "pop hl");
    }
    sv_push(b, &out, "ld a, h");
    sv_push(b, &out, "or l");
    sv_pushf(b, &out, "jp z, %s", q->args[1]);
    return out;
}

/* String.retstr (_str.py:311-321). */
static StrVec emit_retstr(Backend *b, Quad *q) {
    bool tmp;
    StrVec out = str_get_oper(b, q->args[0], NULL, false, true, &tmp, NULL);
    if (!tmp) sv_push(b, &out, s_runtime_call(b, RL_LOADSTR));
    sv_push(b, &out, "#pragma opt require hl");
    sv_pushf(b, &out, "jp %s", q->args[1]);
    return out;
}

/* String.paramstr (_str.py:323-337). get_oper's tmp is discarded; the
 * keep/dup test is recomputed from ins[1][0] (Python :331). */
static StrVec emit_paramstr(Backend *b, Quad *q) {
    StrVec out = str_get_oper(b, q->args[0], NULL, false, false,
                              NULL, NULL);
    if (out.len > 0) out.len--;            /* output.pop() — drop reg flag */
    const char *a0 = q->args[0];
    bool dup = (a0[0] == '#' || a0[0] == '_');
    if (dup) sv_push(b, &out, s_runtime_call(b, RL_LOADSTR));
    sv_push(b, &out, "push hl");
    return out;
}

/* String.fparamstr (_str.py:339-348): just the get_oper sequence. */
static StrVec emit_fparamstr(Backend *b, Quad *q) {
    return str_get_oper(b, q->args[0], NULL, false, false, NULL, NULL);
}

/* ---- _QUAD_TABLE dispatch (main.py:151-611) -------------------------- */
static StrVec quad_emit(Backend *b, Quad *q) {
    const char *I = q->instr;
    if (strcmp(I, IC_END) == 0)    return emit_end(b, q);
    if (strcmp(I, IC_INLINE) == 0) return emit_inline(b, q);

    /* S5.8bc — string _QUAD_TABLE rows (main.py:162,217-262,309,318,
     * 345,355,387,427,465; icinstruction.py opcode strings). The String
     * class is the verbatim _str.py port above. */
    if (strcmp(I, "storestr")  == 0) return emit_storestr(b, q);
    if (strcmp(I, "loadstr")   == 0) return emit_loadstr(b, q);
    if (strcmp(I, "paramstr")  == 0) return emit_paramstr(b, q);
    if (strcmp(I, "fparamstr") == 0) return emit_fparamstr(b, q);
    if (strcmp(I, "retstr")    == 0) return emit_retstr(b, q);
    if (strcmp(I, "addstr")    == 0) return emit_addstr(b, q);
    if (strcmp(I, "eqstr")     == 0) return emit_eqstr(b, q);
    if (strcmp(I, "nestr")     == 0) return emit_nestr(b, q);
    if (strcmp(I, "ltstr")     == 0) return emit_ltstr(b, q);
    if (strcmp(I, "gtstr")     == 0) return emit_gtstr(b, q);
    if (strcmp(I, "lestr")     == 0) return emit_lestr(b, q);
    if (strcmp(I, "gestr")     == 0) return emit_gestr(b, q);
    if (strcmp(I, "lenstr")    == 0) return emit_lenstr(b, q);
    if (strcmp(I, "jzerostr")  == 0) return emit_jzerostr(b, q);

    /* S5.3 — _QUAD_TABLE entries (main.py:151-611). store/load/add/sub/
     * mul/div dispatch by suffix to the Bits8/Bits16 emitter (the Python
     * table maps STOREU8/STOREI8 -> Bits8.store8, etc.). */
    if (strcmp(I, "storeu8")  == 0 || strcmp(I, "storei8")  == 0) return emit_store8(b, q);
    if (strcmp(I, "storeu16") == 0 || strcmp(I, "storei16") == 0) return emit_store16(b, q);
    if (strcmp(I, "loadu8")   == 0 || strcmp(I, "loadi8")   == 0) return emit_load8(b, q);
    if (strcmp(I, "loadu16")  == 0 || strcmp(I, "loadi16")  == 0) return emit_load16(b, q);
    if (strcmp(I, "addu8")    == 0 || strcmp(I, "addi8")    == 0) return emit_add8(b, q);
    if (strcmp(I, "addu16")   == 0 || strcmp(I, "addi16")   == 0) return emit_add16(b, q);
    if (strcmp(I, "subu8")    == 0 || strcmp(I, "subi8")    == 0) return emit_sub8(b, q);
    if (strcmp(I, "subu16")   == 0 || strcmp(I, "subi16")   == 0) return emit_sub16(b, q);
    if (strcmp(I, "mulu8")    == 0 || strcmp(I, "muli8")    == 0) return emit_mul8(b, q);
    if (strcmp(I, "mulu16")   == 0 || strcmp(I, "muli16")   == 0) return emit_mul16(b, q);
    if (strcmp(I, "divu8")    == 0) return emit_divu8(b, q);
    if (strcmp(I, "divi8")    == 0) return emit_divi8(b, q);
    if (strcmp(I, "divu16")   == 0) return emit_divu16(b, q);
    if (strcmp(I, "divi16")   == 0) return emit_divi16(b, q);
    /* S7.x — 8/16-bit mod + bitwise/logical/shift _QUAD_TABLE rows
     * (main.py:189-208,276-299,587-600). modu/modi are DISTINCT funcs;
     * shl{u,i}->shl8/shl16; band{u,i}->band8/band16; etc. */
    if (strcmp(I, "modu8")    == 0) return emit_modu8(b, q);
    if (strcmp(I, "modi8")    == 0) return emit_modi8(b, q);
    if (strcmp(I, "modu16")   == 0) return emit_modu16(b, q);
    if (strcmp(I, "modi16")   == 0) return emit_modi16(b, q);
    if (strcmp(I, "shru8")    == 0) return emit_shru8(b, q);
    if (strcmp(I, "shri8")    == 0) return emit_shri8(b, q);
    if (strcmp(I, "shlu8")    == 0 || strcmp(I, "shli8")    == 0) return emit_shl8(b, q);
    if (strcmp(I, "shru16")   == 0) return emit_shru16(b, q);
    if (strcmp(I, "shri16")   == 0) return emit_shri16(b, q);
    if (strcmp(I, "shlu16")   == 0 || strcmp(I, "shli16")   == 0) return emit_shl16(b, q);
    if (strcmp(I, "oru8")     == 0 || strcmp(I, "ori8")     == 0) return emit_or8(b, q);
    if (strcmp(I, "oru16")    == 0 || strcmp(I, "ori16")    == 0) return emit_or16(b, q);
    if (strcmp(I, "andu8")    == 0 || strcmp(I, "andi8")    == 0) return emit_and8(b, q);
    if (strcmp(I, "andu16")   == 0 || strcmp(I, "andi16")   == 0) return emit_and16(b, q);
    if (strcmp(I, "xoru8")    == 0 || strcmp(I, "xori8")    == 0) return emit_xor8(b, q);
    if (strcmp(I, "xoru16")   == 0 || strcmp(I, "xori16")   == 0) return emit_xor16(b, q);
    if (strcmp(I, "boru8")    == 0 || strcmp(I, "bori8")    == 0) return emit_bor8(b, q);
    if (strcmp(I, "boru16")   == 0 || strcmp(I, "bori16")   == 0) return emit_bor16(b, q);
    if (strcmp(I, "bandu8")   == 0 || strcmp(I, "bandi8")   == 0) return emit_band8(b, q);
    if (strcmp(I, "bandu16")  == 0 || strcmp(I, "bandi16")  == 0) return emit_band16(b, q);
    if (strcmp(I, "bxoru8")   == 0 || strcmp(I, "bxori8")   == 0) return emit_bxor8(b, q);
    if (strcmp(I, "bxoru16")  == 0 || strcmp(I, "bxori16")  == 0) return emit_bxor16(b, q);
    if (strcmp(I, "cast")     == 0) return emit_cast(b, q);
    if (strcmp(I, "var")      == 0) return emit_var(b, q);
    if (strcmp(I, "vard")     == 0) return emit_vard(b, q);
    if (strcmp(I, "varx")     == 0) return emit_varx(b, q);
    /* S5.7c — local-init _QUAD_TABLE rows (main.py:575-583
     * LVARX/LVARD/LARRD -> _lvarx/_lvard/_larrd). Dispatch-reachable now;
     * exercised once the inside-function offset model (compute_offsets /
     * per-local offset+size+bounds / locals_size) lands and the
     * FunctionTranslator :58-116 walk gets its first real ic_larrd/
     * ic_lvard/ic_lvarx caller. Same "backend ABI complete; visitor
     * wrapper lands with its first real caller" discipline S5.7b used
     * for _paddr/_pload* (translator.c:420-430). */
    if (strcmp(I, "lvarx")    == 0) return emit_lvarx(b, q);
    if (strcmp(I, "lvard")    == 0) return emit_lvard(b, q);
    if (strcmp(I, "larrd")    == 0) return emit_larrd(b, q);
    if (strcmp(I, "data")     == 0) return emit_data(b, q);
    if (strcmp(I, "deflabel") == 0) return emit_deflabel(b, q);
    if (strcmp(I, "label")    == 0) return emit_label(b, q);

    /* ---- S5.5 — flow-control _QUAD_TABLE rows (main.py:308-398).
     * JUMP/CALL/RET scaffolding + the jzero/jnzero/jgezero width-split
     * (JZERO{I8,U8,I16,U16,I32,U32,F16,F} etc.). The IC opcode string is
     * the Quad's first token; ICInstruction membername->opcode mapping in
     * backend/icinstruction.py. No per-width JUMP/LABEL/CALL/RET split. */
    if (strcmp(I, "jump")     == 0) return emit_jump(b, q);
    if (strcmp(I, "call")     == 0) return emit_call(b, q);
    if (strcmp(I, "ret")      == 0) return emit_ret(b, q);
    if (strcmp(I, "enter")    == 0) return emit_enter(b, q);
    if (strcmp(I, "leave")    == 0) return emit_leave(b, q);
    if (strcmp(I, "jzeroi8")  == 0 || strcmp(I, "jzerou8")  == 0) return emit_jzero8(b, q);
    if (strcmp(I, "jzeroi16") == 0 || strcmp(I, "jzerou16") == 0) return emit_jzero16(b, q);
    if (strcmp(I, "jzeroi32") == 0 || strcmp(I, "jzerou32") == 0) return emit_jzero32(b, q);
    if (strcmp(I, "jzerof16") == 0) return emit_jzerof16(b, q);
    if (strcmp(I, "jzerof")   == 0) return emit_jzerof(b, q);
    if (strcmp(I, "jnzeroi8") == 0 || strcmp(I, "jnzerou8") == 0) return emit_jnzero8(b, q);
    if (strcmp(I, "jnzeroi16")== 0 || strcmp(I, "jnzerou16")== 0) return emit_jnzero16(b, q);
    if (strcmp(I, "jnzeroi32")== 0 || strcmp(I, "jnzerou32")== 0) return emit_jnzero32(b, q);
    if (strcmp(I, "jnzerof16")== 0) return emit_jnzerof16(b, q);
    if (strcmp(I, "jnzerof")  == 0) return emit_jnzerof(b, q);
    if (strcmp(I, "jgezeroi8")== 0) return emit_jgezeroi8(b, q);
    if (strcmp(I, "jgezerou8")== 0) return emit_jgezerou8(b, q);
    if (strcmp(I, "jgezeroi16")==0) return emit_jgezeroi16(b, q);
    if (strcmp(I, "jgezerou16")==0) return emit_jgezerou16(b, q);
    if (strcmp(I, "jgezeroi32")==0) return emit_jgezeroi32(b, q);
    if (strcmp(I, "jgezerou32")==0) return emit_jgezerou32(b, q);
    if (strcmp(I, "jgezerof16")==0) return emit_jgezerof16(b, q);
    if (strcmp(I, "jgezerof") == 0) return emit_jgezerof(b, q);

    /* ---- S5.4 — wide (i32/u32), fixed (f16), float (f) _QUAD_TABLE rows
     * (main.py:158-585). STOREI32/STOREU32 -> Bits32.store32 etc.;
     * VAR/VARD/STORE are the type-generic emitters above. */
    /* store / load */
    if (strcmp(I, "storei32") == 0 || strcmp(I, "storeu32") == 0) return emit_store32(b, q);
    if (strcmp(I, "storef16") == 0) return emit_storef16(b, q);
    if (strcmp(I, "storef")   == 0) return emit_storef(b, q);
    if (strcmp(I, "loadi32")  == 0 || strcmp(I, "loadu32")  == 0) return emit_load32(b, q);
    if (strcmp(I, "loadf16")  == 0) return emit_loadf16(b, q);
    if (strcmp(I, "loadf")    == 0) return emit_loadf(b, q);
    /* add / sub / mul / div / mod */
    if (strcmp(I, "addi32") == 0 || strcmp(I, "addu32") == 0) return emit_add32(b, q);
    if (strcmp(I, "addf16") == 0) return emit_addf16(b, q);
    if (strcmp(I, "addf")   == 0) return emit_addf(b, q);
    if (strcmp(I, "subi32") == 0 || strcmp(I, "subu32") == 0) return emit_sub32(b, q);
    if (strcmp(I, "subf16") == 0) return emit_subf16(b, q);
    if (strcmp(I, "subf")   == 0) return emit_subf(b, q);
    if (strcmp(I, "muli32") == 0 || strcmp(I, "mulu32") == 0) return emit_mul32(b, q);
    if (strcmp(I, "mulf16") == 0) return emit_mulf16(b, q);
    if (strcmp(I, "mulf")   == 0) return emit_mulf(b, q);
    if (strcmp(I, "divu32") == 0) return emit_divu32(b, q);
    if (strcmp(I, "divi32") == 0) return emit_divi32(b, q);
    if (strcmp(I, "divf16") == 0) return emit_divf16(b, q);
    if (strcmp(I, "divf")   == 0) return emit_divf(b, q);
    if (strcmp(I, "powf")   == 0) return emit_powf(b, q);
    if (strcmp(I, "modu32") == 0) return emit_modu32(b, q);
    if (strcmp(I, "modi32") == 0) return emit_modi32(b, q);
    if (strcmp(I, "modf16") == 0) return emit_modf16(b, q);
    if (strcmp(I, "modf")   == 0) return emit_modf(b, q);
    /* shifts (i32/u32) */
    if (strcmp(I, "shru32") == 0) return emit_shru32(b, q);
    if (strcmp(I, "shri32") == 0) return emit_shri32(b, q);
    if (strcmp(I, "shlu32") == 0 || strcmp(I, "shli32") == 0) return emit_shl32(b, q);
    /* comparisons */
    /* S5.5 — 8/16-bit integer comparisons (main.py:209-257). */
    if (strcmp(I, "ltu8")  == 0) return emit_ltu8(b, q);
    if (strcmp(I, "lti8")  == 0) return emit_lti8(b, q);
    if (strcmp(I, "ltu16") == 0) return emit_ltu16(b, q);
    if (strcmp(I, "lti16") == 0) return emit_lti16(b, q);
    if (strcmp(I, "gtu8")  == 0) return emit_gtu8(b, q);
    if (strcmp(I, "gti8")  == 0) return emit_gti8(b, q);
    if (strcmp(I, "gtu16") == 0) return emit_gtu16(b, q);
    if (strcmp(I, "gti16") == 0) return emit_gti16(b, q);
    if (strcmp(I, "leu8")  == 0) return emit_leu8(b, q);
    if (strcmp(I, "lei8")  == 0) return emit_lei8(b, q);
    if (strcmp(I, "leu16") == 0) return emit_leu16(b, q);
    if (strcmp(I, "lei16") == 0) return emit_lei16(b, q);
    if (strcmp(I, "geu8")  == 0) return emit_geu8(b, q);
    if (strcmp(I, "gei8")  == 0) return emit_gei8(b, q);
    if (strcmp(I, "geu16") == 0) return emit_geu16(b, q);
    if (strcmp(I, "gei16") == 0) return emit_gei16(b, q);
    if (strcmp(I, "equ8")  == 0 || strcmp(I, "eqi8")  == 0) return emit_eq8(b, q);
    if (strcmp(I, "equ16") == 0 || strcmp(I, "eqi16") == 0) return emit_eq16(b, q);
    if (strcmp(I, "neu8")  == 0 || strcmp(I, "nei8")  == 0) return emit_ne8(b, q);
    if (strcmp(I, "neu16") == 0 || strcmp(I, "nei16") == 0) return emit_ne16(b, q);

    if (strcmp(I, "ltu32") == 0) return emit_ltu32(b, q);
    if (strcmp(I, "lti32") == 0) return emit_lti32(b, q);
    if (strcmp(I, "ltf16") == 0) return emit_ltf16(b, q);
    if (strcmp(I, "ltf")   == 0) return emit_ltf(b, q);
    if (strcmp(I, "gtu32") == 0) return emit_gtu32(b, q);
    if (strcmp(I, "gti32") == 0) return emit_gti32(b, q);
    if (strcmp(I, "gtf16") == 0) return emit_gtf16(b, q);
    if (strcmp(I, "gtf")   == 0) return emit_gtf(b, q);
    if (strcmp(I, "leu32") == 0) return emit_leu32(b, q);
    if (strcmp(I, "lei32") == 0) return emit_lei32(b, q);
    if (strcmp(I, "lef16") == 0) return emit_lef16(b, q);
    if (strcmp(I, "lef")   == 0) return emit_lef(b, q);
    if (strcmp(I, "geu32") == 0) return emit_geu32(b, q);
    if (strcmp(I, "gei32") == 0) return emit_gei32(b, q);
    if (strcmp(I, "gef16") == 0) return emit_gef16(b, q);
    if (strcmp(I, "gef")   == 0) return emit_gef(b, q);
    if (strcmp(I, "equ32") == 0 || strcmp(I, "eqi32") == 0) return emit_eq32(b, q);
    if (strcmp(I, "eqf16") == 0) return emit_eqf16(b, q);
    if (strcmp(I, "eqf")   == 0) return emit_eqf(b, q);
    if (strcmp(I, "neu32") == 0 || strcmp(I, "nei32") == 0) return emit_ne32(b, q);
    if (strcmp(I, "nef16") == 0) return emit_nef16(b, q);
    if (strcmp(I, "nef")   == 0) return emit_nef(b, q);
    /* abs / neg */
    if (strcmp(I, "absi8")  == 0) return emit_abs8(b, q);
    if (strcmp(I, "absi16") == 0) return emit_abs16(b, q);
    if (strcmp(I, "absi32") == 0) return emit_abs32(b, q);
    if (strcmp(I, "absf16") == 0) return emit_absf16(b, q);
    if (strcmp(I, "absf")   == 0) return emit_absf(b, q);
    if (strcmp(I, "negu8")  == 0 || strcmp(I, "negi8")  == 0) return emit_neg8(b, q);
    if (strcmp(I, "negu16") == 0 || strcmp(I, "negi16") == 0) return emit_neg16(b, q);
    if (strcmp(I, "negu32") == 0 || strcmp(I, "negi32") == 0) return emit_neg32(b, q);
    if (strcmp(I, "negf16") == 0) return emit_negf16(b, q);
    if (strcmp(I, "negf")   == 0) return emit_negf(b, q);
    /* logical / bitwise */
    if (strcmp(I, "andu32") == 0 || strcmp(I, "andi32") == 0) return emit_and32(b, q);
    if (strcmp(I, "andf16") == 0) return emit_andf16(b, q);
    if (strcmp(I, "andf")   == 0) return emit_andf(b, q);
    if (strcmp(I, "oru32")  == 0 || strcmp(I, "ori32")  == 0) return emit_or32(b, q);
    if (strcmp(I, "orf16")  == 0) return emit_orf16(b, q);
    if (strcmp(I, "orf")    == 0) return emit_orf(b, q);
    if (strcmp(I, "xoru32") == 0 || strcmp(I, "xori32") == 0) return emit_xor32(b, q);
    if (strcmp(I, "xorf16") == 0) return emit_xorf16(b, q);
    if (strcmp(I, "xorf")   == 0) return emit_xorf(b, q);
    if (strcmp(I, "notu8")  == 0 || strcmp(I, "noti8")  == 0) return emit_not8(b, q);
    if (strcmp(I, "notu16") == 0 || strcmp(I, "noti16") == 0) return emit_not16(b, q);
    if (strcmp(I, "notu32") == 0 || strcmp(I, "noti32") == 0) return emit_not32(b, q);
    if (strcmp(I, "notf16") == 0) return emit_notf16(b, q);
    if (strcmp(I, "notf")   == 0) return emit_notf(b, q);
    if (strcmp(I, "bandu32")== 0 || strcmp(I, "bandi32")== 0) return emit_band32(b, q);
    if (strcmp(I, "boru32") == 0 || strcmp(I, "bori32") == 0) return emit_bor32(b, q);
    if (strcmp(I, "bxoru32")== 0 || strcmp(I, "bxori32")== 0) return emit_bxor32(b, q);
    if (strcmp(I, "bnotu8") == 0 || strcmp(I, "bnoti8") == 0) return emit_bnot8(b, q);
    if (strcmp(I, "bnotu16")== 0 || strcmp(I, "bnoti16")== 0) return emit_bnot16(b, q);
    if (strcmp(I, "bnotu32")== 0 || strcmp(I, "bnoti32")== 0) return emit_bnot32(b, q);
    /* param / ret */
    /* S5.7b — 8/16-bit param/fparam/ret + 32-bit fparam + paddr/pload
     * (main.py:336-355,362-373; _8bit/_16bit/_32bit/_pload). */
    if (strcmp(I, "parami8")  == 0 || strcmp(I, "paramu8")  == 0) return emit_param8(b, q);
    if (strcmp(I, "parami16") == 0 || strcmp(I, "paramu16") == 0) return emit_param16(b, q);
    if (strcmp(I, "parami32") == 0 || strcmp(I, "paramu32") == 0) return emit_param32(b, q);
    if (strcmp(I, "paramf16") == 0) return emit_paramf16(b, q);
    if (strcmp(I, "paramf")   == 0) return emit_paramf(b, q);
    if (strcmp(I, "fparami8")  == 0 || strcmp(I, "fparamu8")  == 0) return emit_fparam8(b, q);
    if (strcmp(I, "fparami16") == 0 || strcmp(I, "fparamu16") == 0) return emit_fparam16(b, q);
    if (strcmp(I, "fparami32") == 0 || strcmp(I, "fparamu32") == 0) return emit_fparam32(b, q);
    if (strcmp(I, "fparamf16") == 0) return emit_fparamf16(b, q);
    if (strcmp(I, "fparamf")   == 0) return emit_fparamf(b, q);
    if (strcmp(I, "in")        == 0) return emit_in(b, q);
    if (strcmp(I, "out")       == 0) return emit_out(b, q);
    if (strcmp(I, "reti8")    == 0 || strcmp(I, "retu8")    == 0) return emit_ret8(b, q);
    if (strcmp(I, "reti16")   == 0 || strcmp(I, "retu16")   == 0) return emit_ret16(b, q);
    if (strcmp(I, "reti32")   == 0 || strcmp(I, "retu32")   == 0) return emit_ret32(b, q);
    if (strcmp(I, "retf16")   == 0) return emit_retf16(b, q);
    if (strcmp(I, "retf")     == 0) return emit_retf(b, q);
    if (strcmp(I, "paddr")    == 0) return emit_paddr(b, q);
    if (strcmp(I, "ploadi8")  == 0 || strcmp(I, "ploadu8")  == 0) return emit_pload8(b, q);
    if (strcmp(I, "ploadi16") == 0 || strcmp(I, "ploadu16") == 0) return emit_pload16(b, q);
    if (strcmp(I, "ploadi32") == 0 || strcmp(I, "ploadu32") == 0) return emit_pload32(b, q);
    if (strcmp(I, "ploadf16")  == 0) return emit_pload32(b, q);   /* PLOADF16 -> _pload32 (main.py:532) */
    if (strcmp(I, "ploadf")    == 0) return emit_ploadf(b, q);
    if (strcmp(I, "ploadstr")  == 0) return emit_ploadstr(b, q);
    if (strcmp(I, "fploadstr") == 0) return emit_fploadstr(b, q);
    if (strcmp(I, "pstorei8")  == 0 || strcmp(I, "pstoreu8")  == 0) return emit_pstore8(b, q);
    if (strcmp(I, "pstorei16") == 0 || strcmp(I, "pstoreu16") == 0) return emit_pstore16(b, q);
    if (strcmp(I, "pstorei32") == 0 || strcmp(I, "pstoreu32") == 0) return emit_pstore32(b, q);
    if (strcmp(I, "pstorestr") == 0) return emit_pstorestr(b, q);
    if (strcmp(I, "pastorestr")== 0) return emit_pastorestr(b, q);

    /* Array element access (main.py:430-478,504-528,585 ICInfo). f16
     * array load maps to _aload32 (main.py:476); astore/pastore f16 use
     * the Fixed16 path — not registered (no fixture in scope). */
    if (strcmp(I, "aaddr")     == 0) return emit_aaddr(b, q);
    if (strcmp(I, "aloadi8")   == 0 || strcmp(I, "aloadu8")  == 0) return emit_aload8(b, q);
    if (strcmp(I, "aloadi16")  == 0 || strcmp(I, "aloadu16") == 0) return emit_aload16(b, q);
    if (strcmp(I, "aloadi32")  == 0 || strcmp(I, "aloadu32") == 0 ||
        strcmp(I, "aloadf16")  == 0) return emit_aload32(b, q);
    if (strcmp(I, "aloadf")    == 0) return emit_aloadf(b, q);
    if (strcmp(I, "aloadstr")  == 0) return emit_aloadstr(b, q);
    if (strcmp(I, "astorei8")  == 0 || strcmp(I, "astoreu8") == 0) return emit_astore8(b, q);
    if (strcmp(I, "astorei16") == 0 || strcmp(I, "astoreu16")== 0) return emit_astore16(b, q);
    if (strcmp(I, "astorei32") == 0 || strcmp(I, "astoreu32")== 0) return emit_astore32(b, q);
    if (strcmp(I, "astoref")   == 0) return emit_astoref(b, q);
    if (strcmp(I, "paaddr")    == 0) return emit_paaddr(b, q);
    if (strcmp(I, "paloadi8")  == 0 || strcmp(I, "paloadu8")  == 0) return emit_paload8(b, q);
    if (strcmp(I, "paloadi16") == 0 || strcmp(I, "paloadu16") == 0) return emit_paload16(b, q);
    if (strcmp(I, "paloadi32") == 0 || strcmp(I, "paloadu32") == 0 ||
        strcmp(I, "paloadf16") == 0) return emit_paload32(b, q);
    if (strcmp(I, "paloadf")   == 0) return emit_paloadf(b, q);
    if (strcmp(I, "paloadstr") == 0) return emit_paloadstr(b, q);
    if (strcmp(I, "pastorei8")  == 0 || strcmp(I, "pastoreu8") == 0) return emit_pastore8(b, q);
    if (strcmp(I, "pastorei16") == 0 || strcmp(I, "pastoreu16")== 0) return emit_pastore16(b, q);
    if (strcmp(I, "pastorei32") == 0 || strcmp(I, "pastoreu32")== 0) return emit_pastore32(b, q);
    if (strcmp(I, "memcopy")   == 0) return emit_memcopy(b, q);
    if (strcmp(I, "exchg")     == 0) return emit_exchg(b, q);

    /* Python KeyErrors here; an unported IC reaching this point is a real
     * gap, not silence (later S5.x add entries). */
    fprintf(stderr, "zxbc: no _QUAD_TABLE entry for IC '%s'\n", q->instr);
    return sv_new();
}

/* ---- _output_join (main.py:746-764) ---------------------------------- */
static void output_join(Backend *b, StrVec *output, StrVec chunk,
                        bool optimize) {
    int base = output->len;
    for (int i = 0; i < chunk.len; i++) vec_push(*output, chunk.data[i]);
    if (!optimize) return;

    int maxlen = peephole_maxlen();
    int level_cap = b->opt_level < 2 ? b->opt_level : 2; /* min(O,2) */
    int idx = base - maxlen;
    if (idx < 0) idx = 0;
    while (idx < output->len) {
        if (!peephole_apply_match(output, level_cap, idx)) {
            idx++;
        } else {
            idx -= maxlen;
            if (idx < 0) idx = 0;
        }
    }
}

/* token-boundary replace: op -> new_label, op not preceded by
 * [.a-zA-Z0-9_] and followed by end-or-space (main.py:741 re.sub). Only
 * reached when label aliasing exists (no temp/consecutive labels in S5.2;
 * first exercised when later sprints emit aliased labels). */
static char *label_resub(Backend *b, const char *ins, const char *op,
                         const char *new_label) {
    size_t ol = strlen(op);
    char outbuf[1024];
    size_t w = 0;
    const char *p = ins;
    while (*p) {
        if (strncmp(p, op, ol) == 0) {
            char prev = (p == ins) ? '\0' : p[-1];
            char next = p[ol];
            bool prev_ok = !((prev >= 'a' && prev <= 'z') ||
                             (prev >= 'A' && prev <= 'Z') ||
                             (prev >= '0' && prev <= '9') ||
                             prev == '.' || prev == '_');
            bool next_ok = (next == '\0' || next == ' ' || next == '\t');
            if (prev_ok && next_ok) {
                size_t nl = strlen(new_label);
                memcpy(outbuf + w, new_label, nl); w += nl;
                p += ol;
                continue;
            }
        }
        outbuf[w++] = *p++;
    }
    outbuf[w] = '\0';
    return arena_strdup(b->arena, outbuf);
}

/* TMP_LABELS membership (src.api.tmp_labels). Populated by s_tmp_label()
 * when wide-shift emitters (Bits32.shru32/shri32/shl32) emit djnz loop
 * labels; remove_unused_labels reads this set (main.py:715). */
static bool is_tmp_label(Backend *b, const char *s) {
    if (b == NULL || s == NULL) return false;
    return hashmap_has(&b->tmp_labels, s);
}

/* ---- remove_unused_labels (main.py:700-743) -------------------------- */
static void remove_unused_labels(Backend *b, StrVec *output) {
    HashMap labels;        hashmap_init(&labels);        /* set */
    HashMap label_alias;   hashmap_init(&label_alias);   /* str->str */
    HashMap labels_used;   hashmap_init(&labels_used);   /* set */
    HashMap labels_to_del; hashmap_init(&labels_to_del); /* str-> (i+1) */

    const char *prev = NULL;
    for (int i = 0; i < output->len; i++) {
        const char *ins = output->data[i];
        size_t L = strlen(ins);
        if (L > 0 && ins[L-1] == ':') {
            char *lab = arena_strndup(b->arena, ins, L - 1);
            hashmap_set(&labels, lab, (void *)1);
            if (prev != NULL) {
                bool cond = (!is_tmp_label(b, prev) && is_tmp_label(b, lab)) ||
                            hashmap_has(&label_alias, prev);
                if (cond) hashmap_set(&label_alias, lab, (void *)prev);
                else      hashmap_set(&label_alias, prev, (void *)lab);
            }
            prev = lab;
        } else {
            prev = NULL;
        }
    }

    for (int i = 0; i < output->len; i++) {
        const char *ins = output->data[i];
        size_t L = strlen(ins);
        char *try_label = arena_strndup(b->arena, ins, L > 0 ? L - 1 : 0);
        if (is_tmp_label(b, try_label)) {
            if (hashmap_has(&labels_used, try_label))
                hashmap_remove(&labels_to_del, try_label);
            else
                hashmap_set(&labels_to_del, try_label, (void *)(intptr_t)(i + 1));
            continue;
        }
        Z80StrList ops = z80asm_opers(b->arena, ins);
        for (int k = 0; k < ops.len; k++) {
            const char *op = ops.data[k];
            if (!hashmap_has(&labels, op)) continue;
            const char *new_label = op;
            while (hashmap_has(&label_alias, new_label))
                new_label = (const char *)hashmap_get(&label_alias, new_label);
            hashmap_set(&labels_used, new_label, (void *)1);
            hashmap_remove(&labels_to_del, new_label);
            if (strcmp(new_label, op) != 0)
                output->data[i] = label_resub(b, ins, op, new_label);
        }
        vec_free(ops);
    }

    /* pop the to-delete indices, descending (main.py:742-743) */
    int n = output->len;
    int *del = arena_alloc(b->arena, (size_t)(n + 1) * sizeof(int));
    int dn = 0;
    for (int i = 0; i < labels_to_del.capacity; i++) {
        if (labels_to_del.entries[i].occupied) {
            int idx = (int)(intptr_t)labels_to_del.entries[i].value - 1;
            del[dn++] = idx;
        }
    }
    for (int a = 0; a < dn; a++)            /* descending sort */
        for (int c = a + 1; c < dn; c++)
            if (del[c] > del[a]) { int t = del[a]; del[a] = del[c]; del[c] = t; }
    for (int a = 0; a < dn; a++) {
        int idx = del[a];
        for (int j = idx; j < output->len - 1; j++)
            output->data[j] = output->data[j + 1];
        output->len--;
    }

    hashmap_free(&labels);
    hashmap_free(&label_alias);
    hashmap_free(&labels_used);
    hashmap_free(&labels_to_del);
}

/* sorted(set) helper for REQUIRES/INITS (empty for the S5.2 calib). */
static StrVec sorted_keys(Backend *b, HashMap *set) {
    StrVec v = sv_new();
    for (int i = 0; i < set->capacity; i++)
        if (set->entries[i].occupied) sv_push(b, &v, set->entries[i].key);
    for (int a = 0; a < v.len; a++)
        for (int c = a + 1; c < v.len; c++)
            if (strcmp(v.data[c], v.data[a]) < 0) {
                char *t = v.data[a]; v.data[a] = v.data[c]; v.data[c] = t;
            }
    return v;
}

/* ---- Backend.emit (main.py:766-785) ---------------------------------- */
StrVec backend_emit(Backend *b, bool optimize) {
    StrVec output = sv_new();
    Quad *q;
    vec_foreach(b->memory, q) {
        StrVec chunk = quad_emit(b, q);
        output_join(b, &output, chunk, optimize);
        vec_free(chunk);
    }

    if (optimize && b->opt_level > 1) {
        remove_unused_labels(b, &output);
        StrVec tmp = output;
        output = sv_new();
        output_join(b, &output, tmp, optimize);
        vec_free(tmp);
    }

    StrVec reqs = sorted_keys(b, &b->requires_);
    for (int i = 0; i < reqs.len; i++)
        sv_pushf(b, &output, "#include once <%s>", reqs.data[i]);
    vec_free(reqs);
    return output;
}

/* ---- emit_prologue (main.py:638-681) --------------------------------- */
StrVec backend_emit_prologue(Backend *b) {
    StrVec heap_init = sv_new();
    sv_pushf(b, &heap_init, "%s:", LBL_DATA);

    /* REQUIRES ∩ MEMINITS or ".core.__MEM_INIT" in INITS -> heap branch.
     * Empty for the S5.2 calibration; faithfully evaluated. */
    bool mem_branch = false;
    for (int i = 0; i < (int)(sizeof(MEMINITS)/sizeof(MEMINITS[0])); i++)
        if (hashmap_has(&b->requires_, MEMINITS[i])) { mem_branch = true; break; }
    if (!mem_branch && hashmap_has(&b->inits, ZXBC_NAMESPACE ".__MEM_INIT"))
        mem_branch = true;
    if (mem_branch) {
        /* main.py:643-651. The label strings %s.ZXBASIC_HEAP_SIZE /
         * %s.ZXBASIC_MEM_HEAP ARE Python's OPTIONS.heap_size_label /
         * OPTIONS.heap_start_label defaults (f"{NAMESPACE}.ZXBASIC_HEAP_SIZE"
         * / f"{NAMESPACE}.ZXBASIC_MEM_HEAP", NAMESPACE=.core). The C does not
         * parse --heap-size-label/--heap-start-label (that is S7.2 CLI-parity,
         * out of scope), so the default-label form is faithful for every
         * C-supported invocation. str(int) / "%s" % int ⇒ decimal text. */
        sv_pushf(b, &heap_init, "; Defines HEAP SIZE\n%s.ZXBASIC_HEAP_SIZE EQU %d",
                 ZXBC_NAMESPACE, b->heap_size);
        if (b->heap_address == -1) {
            /* OPTIONS.heap_address is None */
            sv_pushf(b, &heap_init, "%s.ZXBASIC_MEM_HEAP:", ZXBC_NAMESPACE);
            sv_pushf(b, &heap_init, "DEFS %d", b->heap_size);
        } else {
            sv_pushf(b, &heap_init,
                     "; Defines HEAP ADDRESS\n%s.ZXBASIC_MEM_HEAP EQU %d",
                     ZXBC_NAMESPACE, b->heap_address);
        }
    }

    sv_pushf(b, &heap_init,
             "; Defines USER DATA Length in bytes\n%s EQU %s - %s",
             ZXBC_NAMESPACE ".ZXBASIC_USER_DATA_LEN", LBL_DATA_END, LBL_DATA);
    sv_pushf(b, &heap_init, "%s EQU %s",
             ZXBC_NAMESPACE ".__LABEL__.ZXBASIC_USER_DATA_LEN",
             ZXBC_NAMESPACE ".ZXBASIC_USER_DATA_LEN");
    sv_pushf(b, &heap_init, "%s EQU %s",
             ZXBC_NAMESPACE ".__LABEL__.ZXBASIC_USER_DATA", LBL_DATA);

    StrVec out = sv_new();
    sv_pushf(b, &out, "org %d", b->org);
    sv_pushf(b, &out, "%s:", LBL_START);
    if (b->headerless) {
        for (int i = 0; i < heap_init.len; i++) vec_push(out, heap_init.data[i]);
        vec_free(heap_init);
        return out;
    }

    sv_push(b, &out, "di");
    sv_push(b, &out, "push ix");
    sv_push(b, &out, "push iy");
    sv_push(b, &out, "exx");
    sv_push(b, &out, "push hl");
    sv_push(b, &out, "exx");
    sv_pushf(b, &out, "ld (%s), sp", LBL_CALLBACK);
    sv_push(b, &out, "ei");

    StrVec inits = sorted_keys(b, &b->inits);
    for (int i = 0; i < inits.len; i++)
        sv_pushf(b, &out, "call %s", inits.data[i]);
    vec_free(inits);

    sv_pushf(b, &out, "jp %s", LBL_MAIN);
    sv_pushf(b, &out, "%s:", LBL_CALLBACK);
    sv_push(b, &out, "DEFW 0");
    for (int i = 0; i < heap_init.len; i++) vec_push(out, heap_init.data[i]);
    vec_free(heap_init);
    return out;
}

/* ---- emit_epilogue (main.py:684-697) --------------------------------- */
StrVec backend_emit_epilogue(Backend *b) {
    StrVec out = sv_new();
    for (int i = 0; i < b->at_end.len; i++) vec_push(out, b->at_end.data[i]);
    if (b->autorun) sv_pushf(b, &out, "END %s", LBL_START);
    else            sv_push(b, &out, "END");
    return out;
}
