/*
 * optmemcell.c — see optmemcell.h.
 *
 * `requires` is the byte-for-byte twin of csrc/zxbc/peephole/memcell.c's
 * proven port (memcell.py:184-321); `destroys` ports memcell.py:115-182
 * branch-for-branch. `code in ASMS` is faithfully constant-false: ASMS
 * holds verbatim user ##ASM blocks; the optimizer evaluates ordinary
 * emitted asm only — no path inserts an evaluated instruction into ASMS
 * (same documented invariant as peephole/memcell.c).
 *
 * used_labels: Python runs the full zxbasm asmlex over the line and
 * collects token.type == "ID". asmlex's ID rule is the regex
 * [._a-zA-Z][._a-zA-Z0-9]* whose value is "ID" unless the lowercase
 * lexeme is a reserved instruction / pseudo-op / reg8 / reg16 / flag
 * (asmlex.py:300-330; zx_next_mnemonics only when OPTIONS.zxnext, which
 * the codegen corpus never sets). Ported exactly with those vocab
 * tables. Numbers / strings / punctuation are other token types, never
 * ID, so a lexeme-by-lexeme scan that emits an ID only for the identifier
 * shape AND-NOT-in-vocab reproduces Python's collected list.
 */
#include "optmemcell.h"

#include <ctype.h>
#include <string.h>

static char *m_strdup(Arena *a, const char *s) { return arena_strdup(a, s); }
static char *m_strndup(Arena *a, const char *s, size_t n) {
    return arena_strndup(a, s, n);
}

static bool in_set(const char *x, const char *const *s, size_t n) {
    for (size_t i = 0; i < n; i++) if (strcmp(x, s[i]) == 0) return true;
    return false;
}

/* sorted unique add (helpers.single_registers' sorted() ordering) */
static void radd(Arena *a, Z80StrList *r, const char *s) {
    int i = 0;
    while (i < r->len) {
        int c = strcmp(r->data[i], s);
        if (c == 0) return;
        if (c > 0) break;
        i++;
    }
    char *cp = m_strdup(a, s);
    vec_push(*r, NULL);
    for (int j = r->len - 1; j > i; j--) r->data[j] = r->data[j - 1];
    r->data[i] = cp;
}
static void radd_list(Arena *a, Z80StrList *r, const Z80StrList *src) {
    for (int i = 0; i < src->len; i++) radd(a, r, src->data[i]);
}
/* unordered set add (Python set(...).update — order irrelevant for the
 * consumers: requires/destroys are only ever membership-tested). */
static void uadd(Arena *a, Z80StrList *r, const char *s) {
    for (int i = 0; i < r->len; i++) if (strcmp(r->data[i], s) == 0) return;
    vec_push(*r, m_strdup(a, s));
}
static void uadd_list(Arena *a, Z80StrList *r, const Z80StrList *src) {
    for (int i = 0; i < src->len; i++) uadd(a, r, src->data[i]);
}

static const char *oget(const Z80StrList *o, int k) {
    return (k >= 0 && k < o->len) ? o->data[k] : "";
}
static bool contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

/* BLOCK_ENDERS (helpers.py:104) */
static bool inst_is_ender(const char *i) {
    static const char *S[] = {"jr","jp","call","ret","reti","retn","djnz","rst"};
    return in_set(i, S, 8);
}

/* helpers.ALL_REGS (helpers.py ALL_REGS frozenset). `code in ASMS` is
 * true exactly for backend-emitted inline-asm cells, whose code is a
 * `##ASMn` id (backend/common.py new_ASMID + generic.py:580 ASMS[id]=...);
 * the peephole evaluator's IS_ASM uses the same `startswith("##ASM")`
 * test (evaluator.py:64). For such cells both requires() (memcell.py:187)
 * and destroys() (memcell.py:131) return set(ALL_REGS) — the inline asm
 * may read/clobber anything. The optimizer MUST honour this so a register
 * loaded for a FASTCALL whose body is `asm ... end asm` (e.g. `ld a, 1`
 * before `call _sub` whose block is `##ASM0`) is NOT dropped as dead
 * (fastcall_str, opt3_einar04). */
static bool code_is_asm(const char *code) {
    return code != NULL && strncmp(code, "##ASM", 5) == 0;
}
static void add_all_regs(Arena *a, Z80StrList *r,
                         void (*addfn)(Arena *, Z80StrList *, const char *)) {
    static const char *ALL[] = {"a","b","c","d","e","f","h","l",
        "ixh","ixl","iyh","iyl","r","i","sp"};
    for (size_t k = 0; k < sizeof(ALL)/sizeof(ALL[0]); k++)
        addfn(a, r, ALL[k]);
}

/* ---- requires (memcell.py:184-321) — identical to peephole port ---- */
static Z80StrList compute_requires(Arena *a, const char *i,
                                   const char *code, const char *cond,
                                   const Z80StrList *opers) {
    Z80StrList result; vec_init(result);
    if (code_is_asm(code)) {            /* code in ASMS -> ALL_REGS */
        add_all_regs(a, &result, radd);
        return result;
    }

    Z80StrList o; vec_init(o);
    for (int k = 0; k < opers->len; k++) {
        const char *src = opers->data[k];
        size_t sl = strlen(src);
        char *low = (char *)arena_alloc(a, sl + 1);
        for (size_t j = 0; j < sl; j++)
            low[j] = (char)tolower((unsigned char)src[j]);
        low[sl] = '\0';
        vec_push(o, low);
    }

    if (strcmp(i, "#pragma") == 0) {
        Z80StrList tmp; vec_init(tmp);
        const char *p = code;
        bool first = true;
        for (;;) {
            const char *sp = strchr(p, ' ');
            const char *end = sp ? sp : p + strlen(p);
            if (!first) vec_push(tmp, m_strndup(a, p, (size_t)(end - p)));
            first = false;
            if (!sp) break;
            p = sp + 1;
        }
        if (tmp.len == 0 || strcmp(tmp.data[0], "opt") != 0) {
            vec_free(o); vec_free(tmp); return result;
        }
        if (tmp.len > 1 && strcmp(tmp.data[1], "require") == 0) {
            for (int k = 2; k < tmp.len; k++) {
                const char *x = tmp.data[k];
                const char *b = x;
                while (*b==','||*b==' '||*b=='\t'||*b=='\r') b++;
                const char *e = x + strlen(x);
                while (e>b && (e[-1]==','||e[-1]==' '||e[-1]=='\t'||e[-1]=='\r')) e--;
                char *tok = m_strndup(a, b, (size_t)(e - b));
                Z80StrList sr = z80h_single_registers1(a, tok);
                radd_list(a, &result, &sr); vec_free(sr);
            }
        }
        vec_free(o); vec_free(tmp); return result;
    }

    { static const char *S[] = {"ret","pop","push"};
      if (in_set(i, S, 3)) radd(a, &result, "sp"); }
    { static const char *S[] = {"sbc","adc"};
      if (cond != NULL || in_set(i, S, 2)) radd(a, &result, "f"); }

    for (int k = 0; k < o.len; k++) {
        const char *O = o.data[k];
        if (contains(O,"(hl)")) { radd(a,&result,"h"); radd(a,&result,"l"); }
        if (contains(O,"(de)")) { radd(a,&result,"d"); radd(a,&result,"e"); }
        if (contains(O,"(bc)")) { radd(a,&result,"b"); radd(a,&result,"c"); }
        if (contains(O,"(sp)")) { radd(a,&result,"sp"); }
        if (contains(O,"(ix"))  { radd(a,&result,"ixh"); radd(a,&result,"ixl"); }
        if (contains(O,"(iy"))  { radd(a,&result,"iyh"); radd(a,&result,"iyl"); }
    }

    #define O0 oget(&o, 0)
    #define O1 oget(&o, 1)
    if (strcmp(i,"ccf")==0) {
        radd(a,&result,"f");
    } else if (in_set(i,(const char*[]){"rra","rla","rrca","rlca"},4)) {
        radd(a,&result,"a"); radd(a,&result,"f");
    } else if (in_set(i,(const char*[]){"xor","cp"},2)) {
        if (strcmp(O0,"a")!=0) {
            radd(a,&result,"a");
            if (O0[0]!='(' && !z80h_is_number(O0)) {
                Z80StrList sr=z80h_single_registers(a,&o);
                radd_list(a,&result,&sr); vec_free(sr); }
        }
    } else if (in_set(i,(const char*[]){"or","and"},2)) {
        radd(a,&result,"a");
        if (O0[0]!='(' && !z80h_is_number(O0)) {
            Z80StrList sr=z80h_single_registers(a,&o);
            radd_list(a,&result,&sr); vec_free(sr); }
    } else if (in_set(i,(const char*[]){"adc","sbc","add","sub"},4)) {
        if (o.len==1) {
            bool subsbc=(strcmp(i,"sub")==0||strcmp(i,"sbc")==0);
            if (!subsbc||strcmp(O0,"a")!=0) radd(a,&result,"a");
            if (O0[0]!='(' && !z80h_is_number(O0)) {
                Z80StrList sr=z80h_single_registers(a,&o);
                radd_list(a,&result,&sr); vec_free(sr); }
        } else {
            bool addadc=(strcmp(i,"add")==0||strcmp(i,"adc")==0);
            if (strcmp(O0,O1)!=0||addadc) {
                Z80StrList sr=z80h_single_registers(a,&o);
                radd_list(a,&result,&sr); vec_free(sr); }
        }
        if (strcmp(i,"adc")==0||strcmp(i,"sbc")==0) radd(a,&result,"f");
    } else if (in_set(i,(const char*[]){"daa","rld","rrd","neg","cpl"},5)) {
        radd(a,&result,"a");
    } else if (in_set(i,(const char*[]){"rl","rr","rlc","rrc"},4)) {
        Z80StrList sr=z80h_single_registers(a,&o);
        radd_list(a,&result,&sr); vec_free(sr); radd(a,&result,"f");
    } else if (in_set(i,(const char*[]){"sla","sll","sra","srl","inc","dec"},6)) {
        Z80StrList sr=z80h_single_registers(a,&o);
        radd_list(a,&result,&sr); vec_free(sr);
    } else if (strcmp(i,"djnz")==0) {
        radd(a,&result,"b");
    } else if (in_set(i,(const char*[]){"ldir","lddr","ldi","ldd"},4)) {
        const char *R[]={"b","c","d","e","h","l"};
        for (size_t k=0;k<6;k++) radd(a,&result,R[k]);
    } else if (in_set(i,(const char*[]){"cpi","cpd","cpir","cpdr"},4)) {
        const char *R[]={"a","b","c","h","l"};
        for (size_t k=0;k<5;k++) radd(a,&result,R[k]);
    } else if (strcmp(i,"ld")==0 && !z80h_is_number(O1)) {
        Z80StrList sr=z80h_single_registers1(a,O1);
        radd_list(a,&result,&sr); vec_free(sr);
    } else if (strcmp(i,"ex")==0) {
        if (strcmp(O0,"de")==0) {
            const char *R[]={"d","e","h","l"};
            for (size_t k=0;k<4;k++) radd(a,&result,R[k]);
        } else if (strcmp(O1,"(sp)")==0) {
            radd(a,&result,"h"); radd(a,&result,"l");
        } else {
            const char *R[]={"a","f","a'","f'"};
            for (size_t k=0;k<4;k++) radd(a,&result,R[k]);
        }
    } else if (strcmp(i,"exx")==0) {
        const char *R[]={"b","c","d","e","h","l"};
        for (size_t k=0;k<6;k++) radd(a,&result,R[k]);
    } else if (strcmp(i,"push")==0) {
        Z80StrList sr=z80h_single_registers(a,&o);
        radd_list(a,&result,&sr); vec_free(sr);
    } else if (in_set(i,(const char*[]){"bit","set","res"},3)) {
        Z80StrList sr=z80h_single_registers1(a,O1);
        radd_list(a,&result,&sr); vec_free(sr);
    } else if (strcmp(i,"out")==0) {
        radd(a,&result,O1);
        if (strcmp(O0,"c")==0) { radd(a,&result,"b"); radd(a,&result,"c"); }
    } else if (strcmp(i,"in")==0) {
        if (strcmp(O1,"c")==0) { radd(a,&result,"b"); radd(a,&result,"c"); }
    } else if (strcmp(i,"im")==0) {
        radd(a,&result,"i");
    }
    #undef O0
    #undef O1
    vec_free(o);
    return result;
}

/* ---- destroys (memcell.py:115-182) -------------------------------- */
static Z80StrList compute_destroys(Arena *a, const char *code,
                                   const char *i, const Z80StrList *o) {
    Z80StrList res; vec_init(res);
    if (code_is_asm(code)) {            /* code in ASMS -> ALL_REGS */
        add_all_regs(a, &res, uadd);
        return res;
    }

    if (in_set(i,(const char*[]){"push","ret","call","rst","reti","retn"},6)) {
        uadd(a,&res,"sp"); return res;
    }
    if (strcmp(i,"pop")==0) {
        uadd(a,&res,"sp");
        Z80StrList o1; vec_init(o1);
        if (o->len>0) vec_push(o1, o->data[0]);
        Z80StrList sr=z80h_single_registers(a,&o1);
        uadd_list(a,&res,&sr); vec_free(sr); vec_free(o1);
    } else if (in_set(i,(const char*[]){"ldir","lddr"},2) ||
               in_set(i,(const char*[]){"ldd","ldi"},2)) {
        const char *R[]={"b","c","d","e","h","l","f"};
        for (size_t k=0;k<7;k++) uadd(a,&res,R[k]);
    } else if (in_set(i,(const char*[]){"otir","otdr","oti","otd",
                                        "inir","indr","ini","ind"},8)) {
        uadd(a,&res,"h"); uadd(a,&res,"l"); uadd(a,&res,"b");
    } else if (in_set(i,(const char*[]){"cpir","cpi","cpdr","cpd"},4)) {
        const char *R[]={"h","l","b","c","f"};
        for (size_t k=0;k<5;k++) uadd(a,&res,R[k]);
    } else if (in_set(i,(const char*[]){"ld","in"},2)) {
        Z80StrList o1; vec_init(o1);
        if (o->len>0) vec_push(o1,o->data[0]);
        Z80StrList sr=z80h_single_registers(a,&o1);
        uadd_list(a,&res,&sr); vec_free(sr); vec_free(o1);
    } else if (in_set(i,(const char*[]){"inc","dec"},2)) {
        uadd(a,&res,"f");
        Z80StrList o1; vec_init(o1);
        if (o->len>0) vec_push(o1,o->data[0]);
        Z80StrList sr=z80h_single_registers(a,&o1);
        uadd_list(a,&res,&sr); vec_free(sr); vec_free(o1);
    } else if (strcmp(i,"exx")==0) {
        const char *R[]={"b","c","d","e","h","l"};
        for (size_t k=0;k<6;k++) uadd(a,&res,R[k]);
    } else if (strcmp(i,"ex")==0) {
        Z80StrList s0=z80h_single_registers1(a,oget(o,0));
        uadd_list(a,&res,&s0); vec_free(s0);
        Z80StrList s1=z80h_single_registers1(a,oget(o,1));
        uadd_list(a,&res,&s1); vec_free(s1);
    } else if (in_set(i,(const char*[]){"ccf","scf","bit","cp"},4)) {
        uadd(a,&res,"f");
    } else if (in_set(i,(const char*[]){"or","and"},2)) {
        uadd(a,&res,"f");
        if (strcmp(oget(o,0),"a")!=0) uadd(a,&res,"a");
    } else if (in_set(i,(const char*[]){"xor","add","adc","sub","sbc"},5)) {
        if (o->len>1) {
            Z80StrList s0=z80h_single_registers1(a,oget(o,0));
            uadd_list(a,&res,&s0); vec_free(s0);
        } else uadd(a,&res,"a");
        uadd(a,&res,"f");
    } else if (in_set(i,(const char*[]){"neg","cpl","daa","rra","rla",
                                        "rrca","rlca","rrd","rld"},9)) {
        uadd(a,&res,"a"); uadd(a,&res,"f");
    } else if (strcmp(i,"djnz")==0) {
        uadd(a,&res,"b"); uadd(a,&res,"f");
    } else if (in_set(i,(const char*[]){"rr","rl","rrc","rlc",
                                        "srl","sra","sll","sla"},8)) {
        Z80StrList s0=z80h_single_registers1(a,oget(o,0));
        uadd_list(a,&res,&s0); vec_free(s0);
        uadd(a,&res,"f");
    } else if (in_set(i,(const char*[]){"set","res"},2)) {
        Z80StrList s1=z80h_single_registers1(a,oget(o,1));
        uadd_list(a,&res,&s1); vec_free(s1);
    }
    return res;
}

static void omemcell_derive(OMemCell *c) {
    Arena *a = c->a;
    c->is_label = c->asm_->is_label;
    if (c->is_label) {
        const char *s = c->asm_->asm_;
        size_t n = strlen(s);
        c->inst = m_strndup(a, s, n > 0 ? n - 1 : 0);
    } else {
        c->inst = c->asm_->inst;
    }
    c->cond = c->asm_->cond;
    c->is_ender = inst_is_ender(c->inst);

    /* opers = self.__instr.oper */
    vec_init(c->opers);
    for (int k = 0; k < c->asm_->oper.len; k++)
        vec_push(c->opers, c->asm_->oper.data[k]);

    /* branch_arg: inst in {jr,jp,call,rst,djnz} -> asm.split()[-1] else None
     * NOTE Python uses self.__instr.inst (the raw Asm.inst), not the
     * label-stripped MemCell.inst — identical for non-labels (the only
     * cells where inst could differ are labels, never in that set). */
    c->branch_arg = NULL;
    {
        const char *ai = c->asm_->inst;
        if (in_set(ai,(const char*[]){"jr","jp","call","rst","djnz"},5)) {
            const char *s = c->asm_->asm_;
            /* split() last token */
            const char *e = s + strlen(s);
            while (e > s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\n'||
                             e[-1]=='\r'||e[-1]=='\f'||e[-1]=='\v')) e--;
            const char *b = e;
            while (b > s && !(b[-1]==' '||b[-1]=='\t'||b[-1]=='\n'||
                              b[-1]=='\r'||b[-1]=='\f'||b[-1]=='\v')) b--;
            c->branch_arg = m_strndup(a, b, (size_t)(e - b));
        }
    }

    c->requires_ = compute_requires(a, c->inst, c->asm_->asm_,
                                    c->cond, &c->opers);
    c->destroys_ = compute_destroys(a, c->asm_->asm_, c->inst, &c->opers);
}

OMemCell *omemcell_new(Arena *a, const char *instr, int addr) {
    OMemCell *c = (OMemCell *)arena_alloc(a, sizeof(OMemCell));
    c->a = a;
    c->addr = addr;
    c->asm_ = z80asm_new(a, instr);
    omemcell_derive(c);
    return c;
}

void omemcell_set_asm(OMemCell *c, const char *s) {
    /* MemCell.asm setter (memcell.py:35-41) reassigns self.__instr ONLY.
     * It does NOT invalidate the @cached_property fields inst / opers /
     * branch_arg / requires / destroys (memcell.py:83,103,108,115,184) —
     * once computed (on first access, which in the optimizer happens during
     * block-building / _compute_calls / cpu-state, all BEFORE the
     * jump-over-jump rewrites at main.py:228-246), they keep their stale
     * value. Only the PLAIN properties recompute from the new __instr:
     * code (asm_->asm_, read live), condition_flag (cond), is_label.
     *
     * This staleness is load-bearing: the jump-over-jump pass does
     * `blk[-1].asm = blk[-1].code.replace(label, new_label)` and then a
     * LATER label's `new_label = first.opers[0]` reads the STALE opers of a
     * cell whose code was already rewritten — yielding the pre-rewrite
     * target (the `while` fixture: a cell rewritten `jp _20`->`jp _10`
     * still reports opers `[_20]`, so a chained trampoline resolves to
     * `_20`, not `_10`). Recomputing opers here (the previous C behaviour)
     * gave `_10` and diverged from the oracle. Keep inst/is_ender/opers/
     * branch_arg/requires/destroys frozen; refresh only the plain fields. */
    c->asm_ = z80asm_new(c->a, s);
    c->is_label = c->asm_->is_label;        /* plain property */
    c->cond     = c->asm_->cond;            /* condition_flag: plain property */
}

/* ---- used_labels (memcell.py:343-366) ----------------------------- */
static bool asmlex_is_id(const char *low) {
    /* reserved_instructions */
    static const char *RI[] = {"adc","add","and","bit","call","ccf","cp",
      "cpd","cpdr","cpi","cpir","cpl","daa","dec","di","djnz","ei","ex",
      "exx","halt","im","in","inc","ind","indr","ini","inir","jp","jr",
      "ld","ldd","lddr","ldi","ldir","neg","nop","or","otdr","otir","out",
      "outd","outi","pop","push","res","ret","reti","retn","rl","rla",
      "rlc","rlca","rld","rr","rra","rrc","rrca","rrd","rst","sbc","scf",
      "set","sla","sll","sra","srl","sub","xor"};
    static const char *PS[] = {"align","db","defb","defm","defs","defw",
      "ds","dw","end","endp","equ","incbin","local","namespace","org","proc"};
    static const char *R8[] = {"a","b","c","d","e","h","i","ixh","ixl",
      "iyh","iyl","l","r"};
    static const char *R16[] = {"af","bc","de","hl","ix","iy","sp"};
    static const char *FL[] = {"m","nc","nz","p","pe","po","z"};
    if (in_set(low, RI, sizeof(RI)/sizeof(RI[0]))) return false;
    if (in_set(low, PS, sizeof(PS)/sizeof(PS[0]))) return false;
    if (in_set(low, R8, sizeof(R8)/sizeof(R8[0]))) return false;
    if (in_set(low, FL, sizeof(FL)/sizeof(FL[0]))) return false;
    if (in_set(low, R16, sizeof(R16)/sizeof(R16[0]))) return false;
    return true;
}

Z80StrList omemcell_used_labels(Arena *a, const OMemCell *c) {
    Z80StrList result; vec_init(result);
    const char *tmp = c->asm_->asm_;
    /* if not len(tmp) or tmp[0] in ("#",";"): return [] */
    if (tmp[0] == '\0' || tmp[0] == '#' || tmp[0] == ';') return result;

    /* scan identifier-shaped lexemes [._a-zA-Z][._a-zA-Z0-9]* and keep
     * those whose lowercase form is not in the asmlex vocab. Strings
     * ("...") and numeric tokens are distinct asmlex token types and are
     * skipped so an ID inside them is never (mis)collected. */
    const char *p = tmp;
    while (*p) {
        if (*p == '"') {              /* STRING token: skip to matching " */
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
            continue;
        }
        unsigned char ch = (unsigned char)*p;
        if (ch == '.' || ch == '_' || isalpha(ch)) {
            const char *s = p;
            p++;
            while (*p && (*p=='.' || *p=='_' || isalnum((unsigned char)*p)))
                p++;
            size_t n = (size_t)(p - s);
            char *low = (char *)arena_alloc(a, n + 1);
            char *up  = (char *)arena_alloc(a, n + 1);
            for (size_t k = 0; k < n; k++) {
                low[k] = (char)tolower((unsigned char)s[k]);
                up[k]  = (char)toupper((unsigned char)s[k]);
            }
            low[n] = up[n] = '\0';
            if (asmlex_is_id(low)) {
                /* asmlex sets t.value = original (regs16.get default ID
                 * branch restores the original lexeme, asmlex.py:328-330);
                 * collected ID values are the original spelling. */
                vec_push(result, m_strndup(a, s, n));
            }
            continue;
        }
        p++;
    }
    return result;
}

/* ---- replace_label (memcell.py:368-373) --------------------------- */
void omemcell_replace_label(OMemCell *c, const char *old_label,
                            const char *new_label) {
    if (strcmp(old_label, new_label) == 0) return;
    /* re.sub(r"\bold\b", new, self.code) — \b is a word boundary where a
     * "word" char is [A-Za-z0-9_]. Replace every non-overlapping
     * occurrence. */
    const char *code = c->asm_->asm_;
    size_t ol = strlen(old_label), nl = strlen(new_label);
    Arena *a = c->a;
    /* worst case growth */
    size_t cl = strlen(code);
    size_t cap = cl + 1;
    /* count occurrences for sizing */
    {
        const char *q = code;
        while ((q = strstr(q, old_label)) != NULL) {
            bool lb = (q == code) ||
                      !(isalnum((unsigned char)q[-1]) || q[-1] == '_');
            const char *after = q + ol;
            bool rb = (*after == '\0') ||
                      !(isalnum((unsigned char)*after) || *after == '_');
            if (lb && rb && nl > ol) cap += (nl - ol);
            q += ol ? ol : 1;
        }
    }
    char *out = (char *)arena_alloc(a, cap + 1);
    size_t w = 0;
    const char *r = code;
    while (*r) {
        if (strncmp(r, old_label, ol) == 0 && ol > 0) {
            bool lb = (r == code) ||
                      !(isalnum((unsigned char)r[-1]) || r[-1] == '_');
            const char *after = r + ol;
            bool rb = (*after == '\0') ||
                      !(isalnum((unsigned char)*after) || *after == '_');
            if (lb && rb) {
                memcpy(out + w, new_label, nl); w += nl;
                r += ol;
                continue;
            }
        }
        out[w++] = *r++;
    }
    out[w] = '\0';
    omemcell_set_asm(c, out);
}
