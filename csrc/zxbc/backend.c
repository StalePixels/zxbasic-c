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
#define RL_NEGHL      ZXBC_NAMESPACE ".__NEGHL"
#define RL_LOAD_DE_DE ZXBC_NAMESPACE ".__LOAD_DE_DE"

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
    if (strcmp(label, RL_NEGHL)      == 0) return "neg16.asm";
    if (strcmp(label, RL_LOAD_DE_DE) == 0) return "lddede.asm";
    return NULL;
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

/* ---- _vard (generic.py:180-208) -------------------------------------- */
static StrVec emit_vard(Backend *b, Quad *q) {
    StrVec out = sv_new();
    sv_pushf(b, &out, "%s:", q->args[0]);
    char *items[64];
    int n = eval_str_list(b, q->args[1], items, 64);
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
    char *items[64];
    int n = eval_str_list(b, q->args[2], items, 64);

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

/* to_byte (common.py:318-339) — S5.3 integer slice. is_int_type(stype)
 * == stype[0] in ('u','i') (common.py:238-240): i16/u16/i32/u32 -> "ld a,l"
 * (i8/u8 short-circuit to []). bool/f16/f are out of the integer-scalar
 * slice (no bool/float DIM in the S5.3 corpus) — loud, not silent. */
static void cast_to_byte(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "i8") || !strcmp(tA, "u8")) return;     /* [] */
    if (tA[0] == 'u' || tA[0] == 'i') {                     /* is_int_type */
        sv_push(b, out, "ld a, l");
        return;
    }
    fprintf(stderr, "zxbc: cast to_byte from %s not in S5.3 scope\n", tA);
}
/* to_word (common.py:342-367) — S5.3 integer slice. u8 -> ld l,a / ld h,0;
 * i8 -> ld l,a / add a,a / sbc a,a / ld h,a; i16/u16/i32/u32 -> [].
 * bool prepends normalize_boolean (out of the integer-scalar slice). */
static void cast_to_word(Backend *b, StrVec *out, const char *tA) {
    if (!strcmp(tA, "u8")) {
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
    if (tA[0] == 'u' || tA[0] == 'i') return;   /* i16/u16/i32/u32 -> [] */
    fprintf(stderr, "zxbc: cast to_word from %s not in S5.3 scope\n", tA);
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
    } else {
        fprintf(stderr, "zxbc: cast from %s not in S5.3 scope\n", tA);
        return sv_new();
    }

    if (!strcmp(tB, "u8") || !strcmp(tB, "i8")) {
        cast_to_byte(b, &out, tA);
    } else if (!strcmp(tB, "u16") || !strcmp(tB, "i16")) {
        cast_to_word(b, &out, tA);
    } else {
        fprintf(stderr, "zxbc: cast to %s not in S5.3 scope\n", tB);
        return out;
    }

    xsB += sB % 2;                       /* round up to even */
    if (xsB > 4) {
        fprintf(stderr, "zxbc: cast fpush not in S5.3 scope\n");
    } else {
        if (xsB > 2) sv_push(b, &out, "push de");
        if (sB > 1)  sv_push(b, &out, "push hl");
        else         sv_push(b, &out, "push af");
    }
    return out;
}

/* ---- _QUAD_TABLE dispatch (main.py:151-611) -------------------------- */
static StrVec quad_emit(Backend *b, Quad *q) {
    const char *I = q->instr;
    if (strcmp(I, IC_END) == 0)    return emit_end(b, q);
    if (strcmp(I, IC_INLINE) == 0) return emit_inline(b, q);

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
    if (strcmp(I, "cast")     == 0) return emit_cast(b, q);
    if (strcmp(I, "var")      == 0) return emit_var(b, q);
    if (strcmp(I, "vard")     == 0) return emit_vard(b, q);
    if (strcmp(I, "varx")     == 0) return emit_varx(b, q);
    if (strcmp(I, "deflabel") == 0) return emit_deflabel(b, q);
    if (strcmp(I, "label")    == 0) return emit_label(b, q);

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

/* TMP_LABELS membership (src.api.tmp_labels). S5.2 emits no temp labels
 * (the degenerate calibration has no temporaries); the set is genuinely
 * empty. Later sprints that emit temporaries populate this. */
static bool is_tmp_label(const char *s) { (void)s; return false; }

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
                bool cond = (!is_tmp_label(prev) && is_tmp_label(lab)) ||
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
        if (is_tmp_label(try_label)) {
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
        /* heap-size lines — unreached by calib; later S5.x (heap programs). */
        sv_pushf(b, &heap_init, "; Defines HEAP SIZE\n%s.ZXBASIC_HEAP_SIZE EQU 4768",
                 ZXBC_NAMESPACE);
        sv_pushf(b, &heap_init, "%s.ZXBASIC_MEM_HEAP:", ZXBC_NAMESPACE);
        sv_push(b, &heap_init, "DEFS 4768");
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
