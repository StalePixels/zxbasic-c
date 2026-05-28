/*
 * z80asm.c — see z80asm.h. Faithful port of optimizer/asm.py + the
 * helpers.py register vocabulary used by the peephole subsystem.
 *
 * Every branch below corresponds line-for-line to the Python; the
 * Z80INSTR mnemonic set is the live src.zxbasm.z80.Z80INSTR (dumped and
 * frozen here verbatim). Operand-cache (Asm._operands_cache) is omitted:
 * it is a pure memoisation with no observable behaviour difference, and
 * the peephole rebuilds Asm objects per evaluation just as Python does
 * inside MemCell — semantics are identical, only the cache is dropped.
 */
#include "z80asm.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

/* --------------------------------------------------------------------
 * Small string helpers (arena-owned results, no malloc/free churn).
 * ------------------------------------------------------------------ */

static char *as_strdup(Arena *a, const char *s) {
    return arena_strdup(a, s ? s : "");
}

static char *as_strndup(Arena *a, const char *s, size_t n) {
    return arena_strndup(a, s, n);
}

static char *as_lower(Arena *a, const char *s) {
    size_t n = strlen(s);
    char *r = (char *)arena_alloc(a, n + 1);
    for (size_t i = 0; i < n; i++) r[i] = (char)tolower((unsigned char)s[i]);
    r[n] = '\0';
    return r;
}

static char *as_upper(Arena *a, const char *s) {
    size_t n = strlen(s);
    char *r = (char *)arena_alloc(a, n + 1);
    for (size_t i = 0; i < n; i++) r[i] = (char)toupper((unsigned char)s[i]);
    r[n] = '\0';
    return r;
}

/* Python str.strip(" \t\n"): strip exactly space/tab/newline. */
static char *strip_sptn(Arena *a, const char *s) {
    const char *b = s;
    while (*b == ' ' || *b == '\t' || *b == '\n') b++;
    const char *e = s + strlen(s);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n')) e--;
    return as_strndup(a, b, (size_t)(e - b));
}

/* Python str.strip() — strip ASCII whitespace (the set str.strip() uses:
 * space, \t, \n, \r, \f, \v). */
static int py_ws(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static char *strip_ws(Arena *a, const char *s) {
    const char *b = s;
    while (*b && py_ws((unsigned char)*b)) b++;
    const char *e = s + strlen(s);
    while (e > b && py_ws((unsigned char)e[-1])) e--;
    return as_strndup(a, b, (size_t)(e - b));
}

static bool ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* membership in a static lowercased set; x compared case-insensitively */
static bool in_set_ci(const char *x, const char *const *set, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (ci_eq(x, set[i])) return true;
    return false;
}

/* --------------------------------------------------------------------
 * z80.Z80INSTR — verbatim from live src.zxbasm.z80.Z80INSTR (uppercase).
 * ------------------------------------------------------------------ */
static const char *const Z80INSTR[] = {
    "ADC", "ADD", "AND", "BIT", "BRLC", "BSLA", "BSRA", "BSRF", "BSRL",
    "CALL", "CCF", "CP", "CPD", "CPDR", "CPI", "CPIR", "CPL", "DAA",
    "DEC", "DI", "DJNZ", "EI", "EX", "EXX", "HALT", "IM", "IN", "INC",
    "IND", "INDR", "INI", "INIR", "JP", "JR", "LD", "LDD", "LDDR",
    "LDDRX", "LDDX", "LDI", "LDIR", "LDIRX", "LDIX", "LDPIRX", "LDWS",
    "MIRROR", "MUL", "NEG", "NEXTREG", "NOP", "OR", "OTDR", "OTIR",
    "OUT", "OUTD", "OUTI", "OUTINB", "PIXELAD", "PIXELDN", "POP",
    "PUSH", "RES", "RET", "RETI", "RETN", "RL", "RLA", "RLC", "RLCA",
    "RLD", "RR", "RRA", "RRC", "RRCA", "RRD", "RST", "SBC", "SCF",
    "SET", "SETAE", "SLA", "SLL", "SRA", "SRL", "SUB", "SWAPNIB",
    "TEST", "XOR"
};
static const size_t Z80INSTR_N = sizeof(Z80INSTR) / sizeof(Z80INSTR[0]);

static bool z80instr_has_upper(const char *upper) {
    for (size_t i = 0; i < Z80INSTR_N; i++)
        if (strcmp(upper, Z80INSTR[i]) == 0) return true;
    return false;
}

/* --------------------------------------------------------------------
 * RE_INDIR16 = r"[ \t]*\([ \t]*([dD][eE]|[hH][lL])[ \t]*\)[ \t]*"
 * Asm.opers uses .match() (start-anchored, unanchored end).
 * RE_OUTC   = r"[ \t]*\([ \t]*[cC]\)"  (start-anchored, .match)
 * ------------------------------------------------------------------ */
static const char *skip_sp_tab(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Matches RE_INDIR16 at the start of o. Returns true if it matches. */
static bool re_indir16_match(const char *o) {
    const char *p = skip_sp_tab(o);
    if (*p != '(') return false;
    p++;
    p = skip_sp_tab(p);
    if ((tolower((unsigned char)p[0]) == 'd' && tolower((unsigned char)p[1]) == 'e') ||
        (tolower((unsigned char)p[0]) == 'h' && tolower((unsigned char)p[1]) == 'l')) {
        p += 2;
    } else {
        return false;
    }
    p = skip_sp_tab(p);
    if (*p != ')') return false;
    return true;
}

/* Matches RE_OUTC at the start of s. */
static bool re_outc_match(const char *s) {
    const char *p = skip_sp_tab(s);
    if (*p != '(') return false;
    p++;
    p = skip_sp_tab(p);
    if (tolower((unsigned char)*p) != 'c') return false;
    p++;
    if (*p != ')') return false;
    return true;
}

/* --------------------------------------------------------------------
 * Asm.instruction(asm)
 * ------------------------------------------------------------------ */
char *z80asm_instruction(Arena *a, const char *asm_str) {
    char *s = strip_sptn(a, asm_str);
    /* split(" ", 1)[0]: first space-delimited token (split on single space) */
    const char *sp = strchr(s, ' ');
    char *tmp = sp ? as_strndup(a, s, (size_t)(sp - s)) : s;
    char *up = as_upper(a, tmp);
    if (z80instr_has_upper(up)) return as_lower(a, tmp);
    return tmp;
}

/* --------------------------------------------------------------------
 * Asm.opers(inst)
 * ------------------------------------------------------------------ */
Z80StrList z80asm_opers(Arena *a, const char *asm_str) {
    Z80StrList op;
    vec_init(op);

    /* car, cdr = (inst.strip(" \t\n") + " ").split(" ", 1) */
    char *st = strip_sptn(a, asm_str);
    size_t sl = strlen(st);
    char *withsp = (char *)arena_alloc(a, sl + 2);
    memcpy(withsp, st, sl);
    withsp[sl] = ' ';
    withsp[sl + 1] = '\0';
    const char *sp = strchr(withsp, ' ');
    /* sp is never NULL because we appended a space */
    char *car = as_strndup(a, withsp, (size_t)(sp - withsp));
    const char *cdr = sp + 1;

    char *I = as_lower(a, car);

    /* op = [x.strip() for x in cdr.split(",")] */
    {
        const char *p = cdr;
        for (;;) {
            const char *comma = strchr(p, ',');
            const char *end = comma ? comma : p + strlen(p);
            /* x.strip() — ASCII whitespace */
            const char *b = p;
            while (b < end && py_ws((unsigned char)*b)) b++;
            const char *e = end;
            while (e > b && py_ws((unsigned char)e[-1])) e--;
            char *tok = as_strndup(a, b, (size_t)(e - b));
            vec_push(op, tok);
            if (!comma) break;
            p = comma + 1;
        }
    }

    /* Per-instruction synthetic-operand rules (asm.py:92-141) */
    static const char *S_call_jp_jr[] = {"call", "jp", "jr"};
    static const char *S_pp[]         = {"push", "pop", "call"};
    static const char *S_oan[]        = {"or", "and", "xor", "neg", "cpl", "rrca", "rlca"};
    static const char *S_rrla[]       = {"rra", "rla"};
    static const char *S_rrl[]        = {"rr", "rl"};
    static const char *S_adcsbc[]     = {"adc", "sbc"};
    static const char *S_addsub[]     = {"add", "sub"};
    static const char *S_lddi[]       = {"ldd", "ldi", "lddr", "ldir"};
    static const char *S_cpdi[]       = {"cpd", "cpi", "cpdr", "cpir"};
    static const char *S_ret[]        = {"ret", "reti", "retn"};

    if (in_set_ci(I, S_call_jp_jr, 3) && op.len > 1) {
        /* op = op[1:] + ["f"] */
        Z80StrList n; vec_init(n);
        for (int i = 1; i < op.len; i++) vec_push(n, op.data[i]);
        vec_push(n, as_strdup(a, "f"));
        vec_free(op);
        op = n;
    } else if (ci_eq(I, "djnz")) {
        vec_push(op, as_strdup(a, "b"));
    } else if (in_set_ci(I, S_pp, 3)) {
        vec_push(op, as_strdup(a, "sp"));
    } else if (in_set_ci(I, S_oan, 7)) {
        vec_push(op, as_strdup(a, "a"));
    } else if (in_set_ci(I, S_rrla, 2)) {
        vec_push(op, as_strdup(a, "a"));
        vec_push(op, as_strdup(a, "f"));
    } else if (in_set_ci(I, S_rrl, 2)) {
        vec_push(op, as_strdup(a, "f"));
    } else if (in_set_ci(I, S_adcsbc, 2)) {
        if (op.len == 1) {
            /* op = ["a","f"] + op */
            Z80StrList n; vec_init(n);
            vec_push(n, as_strdup(a, "a"));
            vec_push(n, as_strdup(a, "f"));
            vec_push(n, op.data[0]);
            vec_free(op);
            op = n;
        }
    } else if (in_set_ci(I, S_addsub, 2)) {
        if (op.len == 1) {
            Z80StrList n; vec_init(n);
            vec_push(n, as_strdup(a, "a"));
            vec_push(n, op.data[0]);
            vec_free(op);
            op = n;
        }
    } else if (in_set_ci(I, S_lddi, 4)) {
        vec_free(op); vec_init(op);
        vec_push(op, as_strdup(a, "hl"));
        vec_push(op, as_strdup(a, "de"));
        vec_push(op, as_strdup(a, "bc"));
    } else if (in_set_ci(I, S_cpdi, 4)) {
        vec_free(op); vec_init(op);
        vec_push(op, as_strdup(a, "a"));
        vec_push(op, as_strdup(a, "hl"));
        vec_push(op, as_strdup(a, "bc"));
    } else if (ci_eq(I, "exx")) {
        vec_free(op); vec_init(op);
        const char *e[] = {"*", "bc", "de", "hl", "b", "c", "d", "e", "h", "l"};
        for (size_t i = 0; i < 10; i++) vec_push(op, as_strdup(a, e[i]));
    } else if (in_set_ci(I, S_ret, 3)) {
        vec_push(op, as_strdup(a, "sp"));
    } else if (ci_eq(I, "out")) {
        if (op.len > 0 && re_outc_match(op.data[0])) {
            op.data[0] = as_strdup(a, "c");
        } else {
            /* op.pop(0) */
            for (int i = 1; i < op.len; i++) op.data[i - 1] = op.data[i];
            if (op.len > 0) op.len--;
        }
    } else if (ci_eq(I, "in")) {
        if (op.len > 1 && re_outc_match(op.data[1])) {
            op.data[1] = as_strdup(a, "c");
        } else {
            /* op.pop(1) */
            if (op.len > 1) {
                for (int i = 2; i < op.len; i++) op.data[i - 1] = op.data[i];
                op.len--;
            }
        }
    }

    /* RE_INDIR16 normalization:
     *   op[i] = "(" + op[i].strip()[1:-1].strip().lower() + ")" */
    for (int i = 0; i < op.len; i++) {
        if (re_indir16_match(op.data[i])) {
            char *t = strip_ws(a, op.data[i]); /* op[i].strip() */
            size_t tl = strlen(t);
            /* [1:-1] then .strip().lower() */
            char *inner = (tl >= 2) ? as_strndup(a, t + 1, tl - 2) : as_strdup(a, "");
            inner = strip_ws(a, inner);
            inner = as_lower(a, inner);
            size_t il = strlen(inner);
            char *res = (char *)arena_alloc(a, il + 3);
            res[0] = '(';
            memcpy(res + 1, inner, il);
            res[il + 1] = ')';
            res[il + 2] = '\0';
            op.data[i] = res;
        }
    }

    return op;
}

/* --------------------------------------------------------------------
 * Asm.condition(asm)
 * ------------------------------------------------------------------ */
char *z80asm_condition(Arena *a, const char *asm_str) {
    char *i = z80asm_instruction(a, asm_str);

    static const char *S_cjjrd[] = {"call", "jp", "jr", "ret", "djnz"};
    if (!in_set_ci(i, S_cjjrd, 5)) return NULL;

    if (ci_eq(i, "ret")) {
        /* asm = [x.lower() for x in asm.split(" ") if x]; return asm[1] if len>1 */
        Z80StrList parts; vec_init(parts);
        const char *p = asm_str;
        while (*p) {
            const char *q = strchr(p, ' ');
            const char *end = q ? q : p + strlen(p);
            if (end > p) vec_push(parts, as_lower(a, as_strndup(a, p, (size_t)(end - p))));
            if (!q) break;
            p = q + 1;
        }
        char *r = (parts.len > 1) ? parts.data[1] : NULL;
        vec_free(parts);
        return r;
    }

    if (ci_eq(i, "djnz")) return as_strdup(a, "nz");

    /* asm = [x.strip() for x in asm.split(",")]
     * asm = [x.lower() for x in asm[0].split(" ") if x] */
    const char *comma = strchr(asm_str, ',');
    const char *first_end = comma ? comma : asm_str + strlen(asm_str);
    /* asm[0].strip() then split(" ") dropping empties, lowercased */
    {
        const char *b = asm_str;
        while (b < first_end && py_ws((unsigned char)*b)) b++;
        const char *e = first_end;
        while (e > b && py_ws((unsigned char)e[-1])) e--;
        Z80StrList parts; vec_init(parts);
        const char *p = b;
        while (p < e) {
            const char *q = memchr(p, ' ', (size_t)(e - p));
            const char *end = q ? q : e;
            if (end > p) vec_push(parts, as_lower(a, as_strndup(a, p, (size_t)(end - p))));
            if (!q) break;
            p = q + 1;
        }
        static const char *FLAGS[] = {"c", "nc", "z", "nz", "po", "pe", "p", "m"};
        char *r = NULL;
        if (parts.len > 1 && in_set_ci(parts.data[1], FLAGS, 8)) r = parts.data[1];
        vec_free(parts);
        return r;
    }
}

/* --------------------------------------------------------------------
 * Asm.__init__
 * ------------------------------------------------------------------ */
Z80Asm *z80asm_new(Arena *a, const char *asm_str) {
    char *s = strip_ws(a, asm_str); /* asm = asm.strip() */
    assert(s[0] != '\0' && "Empty instruction");

    Z80Asm *r = (Z80Asm *)arena_alloc(a, sizeof(Z80Asm));
    r->inst = z80asm_instruction(a, s);
    r->oper = z80asm_opers(a, s);

    /* asm = "{} {}".format(inst, " ".join(asm.split(" ",1)[1:])).strip() */
    {
        const char *sp = strchr(s, ' ');
        const char *rest = sp ? sp + 1 : "";
        size_t il = strlen(r->inst), rl = strlen(rest);
        char *buf = (char *)arena_alloc(a, il + 1 + rl + 1);
        memcpy(buf, r->inst, il);
        buf[il] = ' ';
        memcpy(buf + il + 1, rest, rl);
        buf[il + 1 + rl] = '\0';
        r->asm_ = strip_ws(a, buf);
    }

    r->cond = z80asm_condition(a, s);
    /* is_label = self.inst[-1] == ":" */
    size_t il = strlen(r->inst);
    r->is_label = il > 0 && r->inst[il - 1] == ':';
    return r;
}

/* --------------------------------------------------------------------
 * helpers.py register predicates
 * ------------------------------------------------------------------ */
bool z80h_is_8bit_normal_register(const char *x) {
    static const char *S[] = {"a", "b", "c", "d", "e", "i", "h", "l"};
    return in_set_ci(x, S, 8);
}
bool z80h_is_8bit_idx_register(const char *x) {
    static const char *S[] = {"ixh", "ixl", "iyh", "iyl"};
    return in_set_ci(x, S, 4);
}
bool z80h_is_8bit_oper_register(const char *x) {
    static const char *S[] = {"a", "b", "c", "d", "e", "i", "h", "l",
                              "ixh", "ixl", "iyh", "iyl"};
    return in_set_ci(x, S, 12);
}
bool z80h_is_16bit_normal_register(const char *x) {
    static const char *S[] = {"bc", "de", "hl"};
    return in_set_ci(x, S, 3);
}
bool z80h_is_16bit_idx_register(const char *x) {
    static const char *S[] = {"ix", "iy"};
    return in_set_ci(x, S, 2);
}
bool z80h_is_16bit_composed_register(const char *x) {
    static const char *S[] = {"af", "af'", "bc", "de", "hl", "ix", "iy"};
    return in_set_ci(x, S, 7);
}
bool z80h_is_16bit_oper_register(const char *x) {
    static const char *S[] = {"af", "af'", "bc", "de", "hl", "ix", "iy", "sp"};
    return in_set_ci(x, S, 8);
}

/* helpers.LO16 / HI16 */
const char *z80h_LO16(Arena *a, const char *x0) {
    char *x = as_lower(a, x0);
    assert(z80h_is_16bit_oper_register(x) && "not a 16bit register");
    assert(strcmp(x, "sp") != 0 && "'sp' cannot be split");
    if (z80h_is_16bit_idx_register(x)) {
        size_t n = strlen(x);
        char *r = (char *)arena_alloc(a, n + 2);
        memcpy(r, x, n); r[n] = 'l'; r[n + 1] = '\0';
        return r;
    }
    /* x[1] + ("'" if "'" in x else "") */
    bool prime = strchr(x, '\'') != NULL;
    char *r = (char *)arena_alloc(a, 3);
    r[0] = x[1];
    if (prime) { r[1] = '\''; r[2] = '\0'; } else { r[1] = '\0'; }
    return r;
}
const char *z80h_HI16(Arena *a, const char *x0) {
    char *x = as_lower(a, x0);
    assert(z80h_is_16bit_oper_register(x) && "not a 16bit register");
    assert(strcmp(x, "sp") != 0 && "'sp' cannot be split");
    if (z80h_is_16bit_idx_register(x)) {
        size_t n = strlen(x);
        char *r = (char *)arena_alloc(a, n + 2);
        memcpy(r, x, n); r[n] = 'h'; r[n + 1] = '\0';
        return r;
    }
    bool prime = strchr(x, '\'') != NULL;
    char *r = (char *)arena_alloc(a, 3);
    r[0] = x[0];
    if (prime) { r[1] = '\''; r[2] = '\0'; } else { r[1] = '\0'; }
    return r;
}

/* sorted() insertion: keep a unique, sorted (by strcmp, == Python str sort
 * for ASCII) list. */
static void sorted_add(Arena *a, Z80StrList *r, const char *s0) {
    char *s = as_strdup(a, s0);
    int i = 0;
    while (i < r->len) {
        int c = strcmp(r->data[i], s);
        if (c == 0) return;       /* set: dedupe */
        if (c > 0) break;
        i++;
    }
    vec_push(*r, NULL);
    for (int j = r->len - 1; j > i; j--) r->data[j] = r->data[j - 1];
    r->data[i] = s;
}

Z80StrList z80h_single_registers(Arena *a, const Z80StrList *op) {
    Z80StrList result; vec_init(result);
    for (int k = 0; k < op->len; k++) {
        const char *x = op->data[k];
        if (z80h_is_8bit_oper_register(x) || ci_eq(x, "f") || ci_eq(x, "sp")) {
            sorted_add(a, &result, x);
        } else if (!z80h_is_16bit_oper_register(x)) {
            continue;
        } else {
            sorted_add(a, &result, z80h_LO16(a, x));
            sorted_add(a, &result, z80h_HI16(a, x));
        }
    }
    return result;
}

Z80StrList z80h_single_registers1(Arena *a, const char *tok) {
    Z80StrList one; vec_init(one);
    vec_push(one, as_strdup(a, tok));
    Z80StrList r = z80h_single_registers(a, &one);
    vec_free(one);
    return r;
}

/* --------------------------------------------------------------------
 * helpers.new_tmp_val — stateful counter (_RAND_COUNT)
 * ------------------------------------------------------------------ */
#define UNKNOWN_PREFIX "*UNKNOWN_"
#define HL_SEP "|"
static int g_rand_count = 0;

void z80h_helpers_init(void) { g_rand_count = 0; }

char *z80h_new_tmp_val(Arena *a) {
    g_rand_count += 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%d", UNKNOWN_PREFIX, g_rand_count);
    return as_strdup(a, buf);
}

/* helpers.is_unknown(x) */
bool z80h_is_unknown(const char *x) {
    if (x == NULL) return true;
    /* split by HL_SEP */
    const char *p = x;
    int parts = 1;
    const char *seg_start = x;
    bool any_unknown = false;
    /* iterate segments */
    for (;;) {
        const char *bar = strchr(p, '|');
        const char *end = bar ? bar : p + strlen(p);
        size_t pl = strlen(UNKNOWN_PREFIX);
        if ((size_t)(end - seg_start) >= pl &&
            strncmp(seg_start, UNKNOWN_PREFIX, pl) == 0) {
            any_unknown = true;
        }
        if (!bar) break;
        parts++;
        if (parts > 2) return false; /* len(xx) > 2 -> False */
        p = bar + 1;
        seg_start = p;
    }
    return any_unknown;
}

/* helpers.LO16_val / HI16_val */
char *z80h_LO16_val(Arena *a, const char *x) {
    if (x == NULL) return z80h_new_tmp_val(a);
    long v;
    if (z80h_valnum(x, &v)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ld", (long)((int)v & 0xFF));
        return as_strdup(a, buf);
    }
    if (!z80h_is_unknown(x)) return z80h_new_tmp_val(a);
    /* x.split(HL_SEP)[-1] */
    const char *bar = strrchr(x, '|');
    return as_strdup(a, bar ? bar + 1 : x);
}

char *z80h_HI16_val(Arena *a, const char *x) {
    if (x == NULL) return z80h_new_tmp_val(a);
    long v;
    if (z80h_valnum(x, &v)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ld", (long)(((int)v >> 8) & 0xFF));
        return as_strdup(a, buf);
    }
    if (!z80h_is_unknown(x)) return z80h_new_tmp_val(a);
    /* f"0{HL_SEP}{x}".split(HL_SEP)[-2] : take the part before the LAST
     * '|' of "0|x". If x has no '|', that part is "0"; else it is the
     * substring up to x's last '|'. */
    const char *last = strrchr(x, '|');
    if (last == NULL) return as_strdup(a, "0");
    return as_strndup(a, x, (size_t)(last - x));
}

/* --------------------------------------------------------------------
 * helpers.is_mem_access / is_number / valnum
 *
 * Faithfulness note: Python is_number/valnum/simplify_arg use eval() to
 * recognise arbitrary arithmetic. In the peephole + MemCell.requires
 * paths these are only ever called with single operand TOKENS produced
 * by Asm.opers (a register name, an indirection "(...)", a label, or a
 * numeric literal) — never a multi-term Python expression. For those
 * inputs Python's eval() either (a) raises NameError (identifier) and we
 * fall to RE_NUMBER, or (b) returns the int/float of a bare literal. The
 * port reproduces exactly that observable result: a bare integer/float
 * literal is a number; otherwise RE_NUMBER decides. valnum mirrors the
 * literal-base parsing (%, b, $, h, decimal). No Python construct that
 * reaches here needs general expression evaluation.
 * ------------------------------------------------------------------ */
bool z80h_is_mem_access(const char *arg0) {
    /* arg.strip(); (arg[0],arg[-1]) == ('(',')') */
    size_t n = strlen(arg0);
    const char *b = arg0;
    while (*b && py_ws((unsigned char)*b)) b++;
    const char *e = arg0 + n;
    while (e > b && py_ws((unsigned char)e[-1])) e--;
    if (e == b) return false;
    return b[0] == '(' && e[-1] == ')';
}

/* RE_NUMBER = r"^([-+]?[0-9]+|$[A-Fa-f0-9]+|[0-9][A-Fa-f0-9]*[Hh]|%[01]+|[01]+[bB])$"
 * Note: the unescaped '$' in the 2nd alternative is a Python end-anchor,
 * so that alternative can only match the empty string between ^...$ —
 * i.e. it never contributes. Faithfully reproduced (omitted). */
static bool re_number_match(const char *s) {
    size_t n = strlen(s);
    if (n == 0) return false;
    /* [-+]?[0-9]+ */
    {
        size_t i = 0;
        if (s[0] == '-' || s[0] == '+') i = 1;
        size_t d = i;
        while (d < n && isdigit((unsigned char)s[d])) d++;
        if (d == n && d > i) return true;
    }
    /* [0-9][A-Fa-f0-9]*[Hh] */
    if (n >= 2 && isdigit((unsigned char)s[0])) {
        size_t i = 1;
        while (i < n - 1 && isxdigit((unsigned char)s[i])) i++;
        if (i == n - 1 && (s[n - 1] == 'h' || s[n - 1] == 'H')) return true;
    }
    /* %[01]+ */
    if (s[0] == '%' && n >= 2) {
        size_t i = 1;
        while (i < n && (s[i] == '0' || s[i] == '1')) i++;
        if (i == n) return true;
    }
    /* [01]+[bB] */
    if (n >= 2 && (s[n - 1] == 'b' || s[n - 1] == 'B')) {
        size_t i = 0;
        while (i < n - 1 && (s[i] == '0' || s[i] == '1')) i++;
        if (i == n - 1) return true;
    }
    return false;
}

/* Python `eval(x,{},{})` returning int/float for a *bare literal* only.
 * A bare decimal integer or float literal (optionally signed) qualifies. */
static bool py_eval_is_number_literal(const char *x) {
    size_t n = strlen(x);
    if (n == 0) return false;
    size_t i = 0;
    if (x[0] == '+' || x[0] == '-') i = 1;
    bool digits = false, dot = false, exp = false;
    for (; i < n; i++) {
        char c = x[i];
        if (isdigit((unsigned char)c)) { digits = true; continue; }
        if (c == '.' && !dot && !exp) { dot = true; continue; }
        if ((c == 'e' || c == 'E') && digits && !exp) {
            exp = true;
            if (i + 1 < n && (x[i + 1] == '+' || x[i + 1] == '-')) i++;
            continue;
        }
        return false;
    }
    return digits;
}

bool z80h_is_number(const char *x) {
    if (x == NULL || x[0] == '\0') return false;
    /* x.strip() if str */
    char tmpbuf[256];
    const char *b = x;
    while (*b && py_ws((unsigned char)*b)) b++;
    const char *e = x + strlen(x);
    while (e > b && py_ws((unsigned char)e[-1])) e--;
    size_t tn = (size_t)(e - b);
    if (tn >= sizeof(tmpbuf)) tn = sizeof(tmpbuf) - 1;
    memcpy(tmpbuf, b, tn);
    tmpbuf[tn] = '\0';

    if (z80h_is_mem_access(tmpbuf)) return false;
    if (py_eval_is_number_literal(tmpbuf)) return true;
    /* RE_NUMBER.match(str(x)) — original (unstripped) string */
    return re_number_match(x);
}

bool z80h_valnum(const char *x, long *out) {
    if (!z80h_is_number(x)) return false;
    /* x = str(x) — use as-is */
    size_t n = strlen(x);
    if (n == 0) return false;
    if (x[0] == '%') { *out = strtol(x + 1, NULL, 2); return true; }
    if (n >= 1 && (x[n - 1] == 'b' || x[n - 1] == 'B')) {
        char buf[256]; size_t m = n - 1 < sizeof(buf) ? n - 1 : sizeof(buf) - 1;
        memcpy(buf, x, m); buf[m] = '\0';
        *out = strtol(buf, NULL, 2); return true;
    }
    if (x[0] == '$') { *out = strtol(x + 1, NULL, 16); return true; }
    if (n >= 1 && (x[n - 1] == 'h' || x[n - 1] == 'H')) {
        char buf[256]; size_t m = n - 1 < sizeof(buf) ? n - 1 : sizeof(buf) - 1;
        memcpy(buf, x, m); buf[m] = '\0';
        *out = strtol(buf, NULL, 16); return true;
    }
    /* int(eval(x,{},{})) — bare decimal/float literal -> truncates */
    *out = (long)strtod(x, NULL);
    return true;
}

/* --------------------------------------------------------------------
 * backend.common.is_int / Bits16.int16
 * ------------------------------------------------------------------ */
bool z80h_is_int(const char *op) {
    /* Python int(op): optional sign, decimal digits, surrounding ws ok. */
    if (op == NULL) return false;
    const char *p = op;
    while (*p && py_ws((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') p++;
    if (!isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) p++;
    while (*p && py_ws((unsigned char)*p)) p++;
    return *p == '\0';
}

bool z80h_int16(const char *op, int *out) {
    if (!z80h_is_int(op)) return false;
    long v = strtol(op, NULL, 10);
    *out = (int)(v & 0xFFFF);
    return true;
}

/* --------------------------------------------------------------------
 * backend.common.is_float / float(op) — Python float() string parse.
 * Python float() accepts: optional surrounding whitespace, optional sign,
 * decimal/exponent forms, "inf"/"infinity"/"nan" (case-insensitive),
 * underscores between digits (3.6+). It rejects trailing junk, empty,
 * and hex/binary suffix forms (those are NOT valid float() input — e.g.
 * float("1Fh") raises). The f16/float corpus operands are plain decimal
 * /float literals, temp names ("t0"), or labels ("_a"); only the literal
 * forms must parse. We mirror Python: strtod over a copy with the
 * underscores stripped only when they sit strictly between digits, then
 * require the whole (whitespace-trimmed) string be consumed.
 * ------------------------------------------------------------------ */
static bool py_float_parse(const char *op, double *out) {
    if (op == NULL) return false;

    /* Trim Python whitespace both ends into a stack copy. */
    const char *b = op;
    while (*b && py_ws((unsigned char)*b)) b++;
    const char *e = b + strlen(b);
    while (e > b && py_ws((unsigned char)e[-1])) e--;
    size_t n = (size_t)(e - b);
    if (n == 0) return false;

    char buf[256];
    if (n >= sizeof(buf)) return false;

    /* Copy, dropping underscores that are strictly between two digits
     * (Python 3.6 numeric-literal grouping; any other underscore makes
     * float() raise — reproduce by leaving it in so strtod stops short
     * and the full-consume check below fails). */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (c == '_' && i > 0 && i + 1 < n &&
            isdigit((unsigned char)b[i - 1]) &&
            isdigit((unsigned char)b[i + 1]))
            continue;
        buf[w++] = c;
    }
    buf[w] = '\0';

    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (endp == buf) return false;          /* nothing parsed */
    if (*endp != '\0') return false;        /* trailing junk -> float() raises */
    if (out) *out = v;
    return true;
}

bool z80h_is_float(const char *op) {
    return py_float_parse(op, NULL);
}

bool z80h_float(const char *op, double *out) {
    return py_float_parse(op, out);
}

/* --------------------------------------------------------------------
 * src/api/fp.py — the ZX Spectrum 40-bit FP encoder (BYTE-CRITICAL).
 * Faithful port of fp() + immediate_float(). Reproduces Python's
 * int()-truncation, 32-bit binary string, and the mantissa bit-walk
 * exactly (all ops are exact in IEEE-754 double — /2, *2, and integer
 * truncation of a value in [0,1)).
 * ------------------------------------------------------------------ */

/* bin(int(f) & 0xFFFFFFFF)[2:].zfill(32) — 32-char MSB-first binary of
 * the low 32 bits of int(f) (truncate toward zero). */
static void fp_bin32(double f, char out[33]) {
    long long i = (long long)f;                 /* Python int(f): trunc */
    unsigned long u = (unsigned long)((unsigned long long)i & 0xFFFFFFFFULL);
    for (int b = 0; b < 32; b++)
        out[b] = (char)('0' + (int)((u >> (31 - b)) & 1u));
    out[32] = '\0';
}

/* bindec32(f): "0" (or bin32(f) if f>=1) + "." + 32 fraction bits.
 * Writes the full string; returns its length. */
static int fp_bindec32(double f, char *result) {
    int w = 0;
    char b32[33];
    if (f >= 1.0) {
        fp_bin32(f, b32);
        for (int k = 0; b32[k]; k++) result[w++] = b32[k];
    } else {
        result[w++] = '0';
    }
    result[w++] = '.';

    double a = f;
    long long c = (long long)a;                 /* int(a) */
    for (int i = 0; i < 32; i++) {
        a -= (double)c;
        a *= 2.0;
        c = (long long)a;                        /* int(a) (trunc) */
        result[w++] = (char)('0' + (int)c);
    }
    result[w] = '\0';
    return w;
}

/* fp(x) -> (M, E): M is the 32-char sign+mantissa string, E the 8-char
 * exponent string. immediate_float(x) -> the (C, ED, LH) "0XXh" triple. */
void z80h_immediate_float(double x, char *C, char *ED, char *LH) {
    int e = 0;
    int s = (x < 0.0) ? 1 : 0;
    double m = (x < 0.0) ? -x : x;              /* abs(x) */

    while (m >= 1.0)       { m /= 2.0; e += 1; }
    while (m > 0.0 && m < 0.5) { m *= 2.0; e -= 1; }

    char dec[80];
    fp_bindec32(m, dec);                        /* len 34 (here m<1) */

    /* M = str(s) + bindec32(m)[3:]  -> 32 chars (1 + 31) */
    char M[40];
    int mw = 0;
    M[mw++] = (char)('0' + s);
    for (int k = 3; dec[k]; k++) M[mw++] = dec[k];
    M[mw] = '\0';

    /* E = bin32(e + 128)[-8:]  if x != 0 else bin32(0)[-8:] */
    char eb[33];
    fp_bin32(x != 0.0 ? (double)(e + 128) : 0.0, eb);
    const char *E = eb + 24;                    /* last 8 chars */

    /* bin2hex(y) = "%02X" % int(y, 2) over a binary substring. */
    #define FP_B2H(dst, ptr, len) do {                                  \
        unsigned _v = 0;                                                \
        for (int _i = 0; _i < (len); _i++)                              \
            _v = (_v << 1) | (unsigned)((ptr)[_i] - '0');               \
        snprintf((dst), sizeof(dst), "%02X", _v);                       \
    } while (0)

    char he[4], hm0[4], hm1[4], hm2[4], hm3[4];
    FP_B2H(he,  E,        8);
    FP_B2H(hm0, M + 8,    8);   /* M[8:16] */
    FP_B2H(hm1, M,        8);   /* M[:8]   */
    FP_B2H(hm2, M + 24,   8);   /* M[24:]  */
    FP_B2H(hm3, M + 16,   8);   /* M[16:24]*/
    #undef FP_B2H

    /* C = "0"+bin2hex(E)+"h"; ED = "0"+b2h(M[8:16])+b2h(M[:8])+"h";
     * LH = "0"+b2h(M[24:])+b2h(M[16:24])+"h" */
    snprintf(C,  16, "0%sh", he);
    snprintf(ED, 16, "0%s%sh", hm0, hm1);
    snprintf(LH, 16, "0%s%sh", hm2, hm3);
}

/* Python repr/str(float) — moved to pyfloat.c so the small test targets
 * (test_ast/test_symboltable/test_check) need not link the rest of
 * z80asm.c. */

/* Bits32.int32(op): (int(op) & 0xFFFFFFFF) -> (DE=hi16, HL=lo16).
 * Python int(op) here is fed an integer-valued operand (is_int true) or,
 * for f16/f, a stringified packed int from f16_to_32bit — both decimal. */
bool z80h_int32(const char *op, unsigned *de, unsigned *hl) {
    if (op == NULL) return false;
    /* int(op): accept the Python int() decimal form (sign + digits). */
    long long v = strtoll(op, NULL, 10);
    unsigned long long u = (unsigned long long)v & 0xFFFFFFFFULL;
    if (de) *de = (unsigned)((u >> 16) & 0xFFFF);
    if (hl) *hl = (unsigned)(u & 0xFFFF);
    return true;
}
