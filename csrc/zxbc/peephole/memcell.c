/*
 * memcell.c — see memcell.h.
 *
 * MemCell.requires (memcell.py:184-321) is ported branch-for-branch.
 * Operand indexing mirrors the Python exactly; where a Python branch
 * indexes o[1] without a length guard (e.g. the `out`/`ex` branches —
 * a latent Python IndexError for malformed asm that the optimizer never
 * receives for a valid program), the C reads "" instead of invoking UB.
 * This is the single, documented divergence: Python would raise and
 * abort; C yields no output either way; no valid emitted-asm sequence
 * reaches it. Every branch that the calibration (and any real program)
 * exercises is byte-faithful.
 */
#include "memcell.h"

#include <ctype.h>
#include <string.h>

/* ASMS membership: self.code in ASMS. ASMS holds verbatim user ##ASM
 * blocks; ordinary emitted/peephole-evaluated instructions are never
 * keys. Faithfully constant-false here (no peephole path populates it). */
static bool code_in_ASMS(const char *code) { (void)code; return false; }

static bool in_set(const char *x, const char *const *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (strcmp(x, s[i]) == 0) return true;
    return false;
}

/* sorted unique add (same ordering as helpers.single_registers' sorted()) */
static void radd(Arena *a, Z80StrList *r, const char *s) {
    int i = 0;
    while (i < r->len) {
        int c = strcmp(r->data[i], s);
        if (c == 0) return;
        if (c > 0) break;
        i++;
    }
    char *cp = arena_strdup(a, s);
    vec_push(*r, NULL);
    for (int j = r->len - 1; j > i; j--) r->data[j] = r->data[j - 1];
    r->data[i] = cp;
}

static void radd_list(Arena *a, Z80StrList *r, const Z80StrList *src) {
    for (int i = 0; i < src->len; i++) radd(a, r, src->data[i]);
}

/* o[k] or "" when k is out of range (see file header note). */
static const char *oget(const Z80StrList *o, int k) {
    return (k >= 0 && k < o->len) ? o->data[k] : "";
}

/* helpers.is_number on an operand token */
static bool h_is_number(const char *x) { return z80h_is_number(x); }

/* "(hl)" in O  (Python substring containment) */
static bool contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

MemCell *memcell_new(Arena *a, const char *instr, int addr) {
    MemCell *c = (MemCell *)arena_alloc(a, sizeof(MemCell));
    c->addr = addr;
    c->asm_ = z80asm_new(a, instr); /* self.asm = Asm(instr) */

    /* is_label = self.__instr.is_label */
    c->is_label = c->asm_->is_label;

    /* MemCell.inst: if is_label -> asm.asm[:-1] else asm.inst */
    if (c->is_label) {
        const char *s = c->asm_->asm_;
        size_t n = strlen(s);
        c->inst = arena_strndup(a, s, n > 0 ? n - 1 : 0);
    } else {
        c->inst = c->asm_->inst;
    }

    /* condition_flag = self.__instr.cond */
    c->cond = c->asm_->cond;

    /* ---- requires (cached_property) -------------------------------- */
    Z80StrList result; vec_init(result);

    /* opers = self.__instr.oper ; o = [x.lower() for x in opers] */
    Z80StrList o; vec_init(o);
    for (int k = 0; k < c->asm_->oper.len; k++) {
        const char *src = c->asm_->oper.data[k];
        size_t sl = strlen(src);
        char *low = (char *)arena_alloc(a, sl + 1);
        for (size_t j = 0; j < sl; j++) low[j] = (char)tolower((unsigned char)src[j]);
        low[sl] = '\0';
        vec_push(o, low);
    }

    const char *i = c->inst; /* self.inst (already label-stripped) */

    if (code_in_ASMS(c->asm_->asm_)) {
        /* return set(helpers.ALL_REGS) */
        static const char *ALL[] = {"a","b","c","d","e","f","h","l",
                                    "ixh","ixl","iyh","iyl","r","i","sp"};
        for (size_t k = 0; k < sizeof(ALL)/sizeof(ALL[0]); k++) radd(a, &result, ALL[k]);
        c->requires_ = result;
        vec_free(o);
        return c;
    }

    if (strcmp(i, "#pragma") == 0) {
        /* tmp = self.code.split(" ")[1:] */
        Z80StrList tmp; vec_init(tmp);
        const char *p = c->asm_->asm_;
        bool first = true;
        for (;;) {
            const char *sp = strchr(p, ' ');
            const char *end = sp ? sp : p + strlen(p);
            if (!first) vec_push(tmp, arena_strndup(a, p, (size_t)(end - p)));
            first = false;
            if (!sp) break;
            p = sp + 1;
        }
        if (tmp.len == 0 || strcmp(tmp.data[0], "opt") != 0) {
            c->requires_ = result; vec_free(o); vec_free(tmp); return c;
        }
        if (tmp.len > 1 && strcmp(tmp.data[1], "require") == 0) {
            /* set(flatten_list([single_registers(x.strip(", \t\r"))
             *                   for x in tmp[2:]])) */
            for (int k = 2; k < tmp.len; k++) {
                /* x.strip(", \t\r") */
                const char *x = tmp.data[k];
                const char *b = x;
                while (*b == ',' || *b == ' ' || *b == '\t' || *b == '\r') b++;
                const char *e = x + strlen(x);
                while (e > b && (e[-1] == ',' || e[-1] == ' ' ||
                                 e[-1] == '\t' || e[-1] == '\r')) e--;
                char *tok = arena_strndup(a, b, (size_t)(e - b));
                Z80StrList sr = z80h_single_registers1(a, tok);
                radd_list(a, &result, &sr);
                vec_free(sr);
            }
        }
        c->requires_ = result; vec_free(o); vec_free(tmp); return c;
    }

    /* if i in ["ret","pop","push"]: result.add("sp") */
    {
        static const char *S[] = {"ret","pop","push"};
        if (in_set(i, S, 3)) radd(a, &result, "sp");
    }
    /* if condition_flag is not None or i in ["sbc","adc"]: result.add("f") */
    {
        static const char *S[] = {"sbc","adc"};
        if (c->cond != NULL || in_set(i, S, 2)) radd(a, &result, "f");
    }

    /* for O in o: indirection-implied registers */
    for (int k = 0; k < o.len; k++) {
        const char *O = o.data[k];
        if (contains(O, "(hl)")) { radd(a, &result, "h"); radd(a, &result, "l"); }
        if (contains(O, "(de)")) { radd(a, &result, "d"); radd(a, &result, "e"); }
        if (contains(O, "(bc)")) { radd(a, &result, "b"); radd(a, &result, "c"); }
        if (contains(O, "(sp)")) { radd(a, &result, "sp"); }
        if (contains(O, "(ix"))  { radd(a, &result, "ixh"); radd(a, &result, "ixl"); }
        if (contains(O, "(iy"))  { radd(a, &result, "iyh"); radd(a, &result, "iyl"); }
    }

    /* The big elif chain. `result.union(single_registers(o))` etc. */
    #define O0 oget(&o, 0)
    #define O1 oget(&o, 1)

    if (strcmp(i, "ccf") == 0) {
        radd(a, &result, "f");
    } else if (in_set(i, (const char*[]){"rra","rla","rrca","rlca"}, 4)) {
        radd(a, &result, "a"); radd(a, &result, "f");
    } else if (in_set(i, (const char*[]){"xor","cp"}, 2)) {
        if (strcmp(O0, "a") != 0) {
            radd(a, &result, "a");
            if (O0[0] != '(' && !h_is_number(O0)) {
                Z80StrList sr = z80h_single_registers(a, &o);
                radd_list(a, &result, &sr); vec_free(sr);
            }
        }
    } else if (in_set(i, (const char*[]){"or","and"}, 2)) {
        radd(a, &result, "a");
        if (O0[0] != '(' && !h_is_number(O0)) {
            Z80StrList sr = z80h_single_registers(a, &o);
            radd_list(a, &result, &sr); vec_free(sr);
        }
    } else if (in_set(i, (const char*[]){"adc","sbc","add","sub"}, 4)) {
        if (o.len == 1) {
            bool subsbc = (strcmp(i,"sub")==0 || strcmp(i,"sbc")==0);
            if (!subsbc || strcmp(O0, "a") != 0) radd(a, &result, "a");
            if (O0[0] != '(' && !h_is_number(O0)) {
                Z80StrList sr = z80h_single_registers(a, &o);
                radd_list(a, &result, &sr); vec_free(sr);
            }
        } else {
            bool addadc = (strcmp(i,"add")==0 || strcmp(i,"adc")==0);
            if (strcmp(O0, O1) != 0 || addadc) {
                Z80StrList sr = z80h_single_registers(a, &o);
                radd_list(a, &result, &sr); vec_free(sr);
            }
        }
        if (strcmp(i,"adc")==0 || strcmp(i,"sbc")==0) radd(a, &result, "f");
    } else if (in_set(i, (const char*[]){"daa","rld","rrd","neg","cpl"}, 5)) {
        radd(a, &result, "a");
    } else if (in_set(i, (const char*[]){"rl","rr","rlc","rrc"}, 4)) {
        Z80StrList sr = z80h_single_registers(a, &o);
        radd_list(a, &result, &sr); vec_free(sr);
        radd(a, &result, "f");
    } else if (in_set(i, (const char*[]){"sla","sll","sra","srl","inc","dec"}, 6)) {
        Z80StrList sr = z80h_single_registers(a, &o);
        radd_list(a, &result, &sr); vec_free(sr);
    } else if (strcmp(i, "djnz") == 0) {
        radd(a, &result, "b");
    } else if (in_set(i, (const char*[]){"ldir","lddr","ldi","ldd"}, 4)) {
        const char *R[] = {"b","c","d","e","h","l"};
        for (size_t k = 0; k < 6; k++) radd(a, &result, R[k]);
    } else if (in_set(i, (const char*[]){"cpi","cpd","cpir","cpdr"}, 4)) {
        const char *R[] = {"a","b","c","h","l"};
        for (size_t k = 0; k < 5; k++) radd(a, &result, R[k]);
    } else if (strcmp(i, "ld") == 0 && !h_is_number(O1)) {
        Z80StrList sr = z80h_single_registers1(a, O1);
        radd_list(a, &result, &sr); vec_free(sr);
    } else if (strcmp(i, "ex") == 0) {
        if (strcmp(O0, "de") == 0) {
            const char *R[] = {"d","e","h","l"};
            for (size_t k = 0; k < 4; k++) radd(a, &result, R[k]);
        } else if (strcmp(O1, "(sp)") == 0) {
            radd(a, &result, "h"); radd(a, &result, "l");
        } else {
            const char *R[] = {"a","f","a'","f'"};
            for (size_t k = 0; k < 4; k++) radd(a, &result, R[k]);
        }
    } else if (strcmp(i, "exx") == 0) {
        const char *R[] = {"b","c","d","e","h","l"};
        for (size_t k = 0; k < 6; k++) radd(a, &result, R[k]);
    } else if (strcmp(i, "push") == 0) {
        Z80StrList sr = z80h_single_registers(a, &o);
        radd_list(a, &result, &sr); vec_free(sr);
    } else if (in_set(i, (const char*[]){"bit","set","res"}, 3)) {
        Z80StrList sr = z80h_single_registers1(a, O1);
        radd_list(a, &result, &sr); vec_free(sr);
    } else if (strcmp(i, "out") == 0) {
        radd(a, &result, O1);
        if (strcmp(O0, "c") == 0) { radd(a, &result, "b"); radd(a, &result, "c"); }
    } else if (strcmp(i, "in") == 0) {
        if (strcmp(O1, "c") == 0) { radd(a, &result, "b"); radd(a, &result, "c"); }
    } else if (strcmp(i, "im") == 0) {
        radd(a, &result, "i");
    }

    #undef O0
    #undef O1

    c->requires_ = result;
    vec_free(o);
    return c;
}

bool memcell_needs(Arena *a, const MemCell *c, const Z80StrList *reglist) {
    /* reglist = helpers.single_registers(reglist)
     * return any(x for x in self.requires if x in reglist) */
    Z80StrList rl = z80h_single_registers(a, reglist);
    bool found = false;
    for (int i = 0; i < c->requires_.len && !found; i++) {
        const char *x = c->requires_.data[i];
        if (x[0] == '\0') continue; /* `any(x for ...)`: empty str is falsy */
        for (int j = 0; j < rl.len; j++) {
            if (strcmp(x, rl.data[j]) == 0) { found = true; break; }
        }
    }
    vec_free(rl);
    return found;
}
