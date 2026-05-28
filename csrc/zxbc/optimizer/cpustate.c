/*
 * cpustate.c — see cpustate.h. Faithful port of cpustate.py
 * (Flags / Memory / CPUState), instruction-for-instruction.
 *
 * String/None model: Python register/mem values are str or (for flags)
 * None|0|1. NULL C-string == Python None. Numbers are produced as
 * decimal strings exactly as Python's str(int). is_register is the
 * helpers.py:335-340 predicate (lowercase ∈ REGS_OPER_SET); ported
 * locally since z80asm.c didn't need it.
 *
 * Provenance: each function header cites cpustate.py line ranges.
 */
#include "cpustate.h"
#include "opthelpers.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HL_SEP "|"

static char *cs_dup(Arena *a, const char *s) { return arena_strdup(a, s); }
static char *cs_fmt_int(Arena *a, long v) {
    char b[32]; snprintf(b, sizeof(b), "%ld", v);
    return cs_dup(a, b);
}

/* helpers.is_register (helpers.py:335-340) — x.lower() in REGS_OPER_SET */
static bool is_register(const char *x) {
    if (x == NULL) return false;
    char buf[8];
    size_t n = strlen(x);
    if (n >= sizeof(buf)) return false;
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)x[i]);
    buf[n] = '\0';
    static const char *S[] = {"a","b","c","d","e","h","l","bc","de","hl",
        "sp","ix","iy","ixh","ixl","iyh","iyl","af","af'","i","r"};
    for (size_t i = 0; i < sizeof(S)/sizeof(S[0]); i++)
        if (strcmp(buf, S[i]) == 0) return true;
    return false;
}

static bool is_number(const char *x) { return z80h_is_number(x); }
static bool is_unknown(const char *x) { return z80h_is_unknown(x); }
static bool valnum(const char *x, long *o) { return z80h_valnum(x, o); }
static bool is_8bit_oper_register(const char *x) {
    return z80h_is_8bit_oper_register(x);
}
static bool is_16bit_composed_register(const char *x) {
    return z80h_is_16bit_composed_register(x);
}

/* _16bit / _8bit maps (cpustate.py:137-172) */
static const char *map16(const char *r) {
    struct { const char *k, *v; } M[] = {
        {"b","bc"},{"c","bc"},{"d","de"},{"e","de"},{"h","hl"},{"l","hl"},
        {"b'","bc'"},{"c'","bc'"},{"d'","de'"},{"e'","de'"},{"h'","hl'"},
        {"l'","hl'"},{"ixh","ix"},{"ixl","ix"},{"iyh","iy"},{"iyl","iy"},
        {"a","af"},{"a'","af'"},{"f","af"},{"f'","af'"}};
    for (size_t i = 0; i < sizeof(M)/sizeof(M[0]); i++)
        if (strcmp(r, M[i].k) == 0) return M[i].v;
    return NULL;
}
static bool map8(const char *r, const char **hi, const char **lo) {
    struct { const char *k, *h, *l; } M[] = {
        {"af","a","f"},{"bc","b","c"},{"de","d","e"},{"hl","h","l"},
        {"af'","a'","f'"},{"bc'","b'","c'"},{"de'","d'","e'"},
        {"hl'","h'","l'"},{"ix","ixh","ixl"},{"iy","iyh","iyl"}};
    for (size_t i = 0; i < sizeof(M)/sizeof(M[0]); i++)
        if (strcmp(r, M[i].k) == 0) { *hi=M[i].h; *lo=M[i].l; return true; }
    return false;
}

/* ---- Memory (cpustate.py:50-125) ---------------------------------- */

/* Memory.__getitem__ with __missing__: dict.get; if absent, assign a
 * fresh new_tmp_val() and return it (cpustate.py:123-125). */
static const char *mem_get(CPUState *s, const char *key) {
    const char *v = omap_get(&s->mem, key);
    if (v != NULL || omap_has(&s->mem, key)) return v;
    char *nv = z80h_new_tmp_val(s->a);
    omap_set(s->a, &s->mem, key, nv);
    return omap_get(&s->mem, key);
}

/* Memory._get_hl_addr (cpustate.py:56-72) -> (lo_addr, hi_addr) */
static void mem_hl_addr(CPUState *s, const char *addr,
                        const char **lo, const char **hi) {
    if (is_number(addr)) {
        long v; valnum(addr, &v);
        *lo = cs_dup(s->a, addr);
        *hi = cs_fmt_int(s->a, v + 1);
        return;
    }
    const char *base; long off;
    if (!opt_re_offset(s->a, addr, &base, &off)) {
        *lo = cs_dup(s->a, addr);
        size_t n = strlen(addr) + 3;
        char *h = (char *)arena_alloc(s->a, n);
        snprintf(h, n, "%s+1", addr);
        *hi = h;
        return;
    }
    if (off == 0) { mem_hl_addr(s, base, lo, hi); return; }
    if (off == -1) { *lo = cs_dup(s->a, addr); *hi = cs_dup(s->a, base); return; }
    *lo = cs_dup(s->a, addr);
    /* "%s%+i" % (base, off + 1) */
    char b[64]; snprintf(b, sizeof(b), "%s%+ld", base, off + 1);
    *hi = cs_dup(s->a, b);
}

/* Memory.read_16_bit_value (cpustate.py:74-85) */
static const char *mem_read16(CPUState *s, const char *addr) {
    const char *lo, *hi;
    mem_hl_addr(s, addr, &lo, &hi);
    const char *vhi = mem_get(s, hi);
    const char *vlo = mem_get(s, lo);
    if (is_number(vhi) && is_number(vlo)) {
        long h, l; valnum(vlo, &l); valnum(vhi, &h);
        return cs_fmt_int(s->a, l + 256 * h);
    }
    size_t n = strlen(vhi ? vhi : "") + 1 + strlen(vlo ? vlo : "") + 1;
    char *res = (char *)arena_alloc(s->a, n);
    snprintf(res, n, "%s|%s", vhi ? vhi : "", vlo ? vlo : "");
    char *lab = opt_get_orig_label_from_unknown16(s->a, "");
    if (lab != NULL) return lab;
    return res;
}

/* Memory.write_16_bit_value (cpustate.py:87-102) */
static void mem_write16(CPUState *s, const char *addr, const char *value) {
    char *vhi, *vlo;
    if (is_number(value)) {
        long v; valnum(value, &v);
        vhi = cs_fmt_int(s->a, (v >> 8) & 0xFF);
        vlo = cs_fmt_int(s->a, v & 0xFF);
    } else {
        const char *v_;
        if (opt_is_unknown16(value)) v_ = value;
        else v_ = opt_new_tmp_val16_from_label(s->a, value);
        vhi = opt_get_H_from_unknown_value(s->a, v_);
        vlo = opt_get_L_from_unknown_value(s->a, v_);
    }
    const char *lo, *hi;
    mem_hl_addr(s, addr, &lo, &hi);
    omap_set(s->a, &s->mem, lo, vlo);
    omap_set(s->a, &s->mem, hi, vhi);
}

/* Memory.read_8_bit_value (cpustate.py:104-110) */
static const char *mem_read8(CPUState *s, const char *addr) {
    const char *lo, *hi;
    mem_hl_addr(s, addr, &lo, &hi);
    const char *v = mem_get(s, lo);
    if (is_number(v)) { long n; valnum(v, &n); return cs_fmt_int(s->a, n & 0xFF); }
    return v;
}

/* Memory.write_8_bit_value (cpustate.py:112-121) */
static void mem_write8(CPUState *s, const char *addr, const char *value) {
    char *val;
    if (is_number(value)) { long v; valnum(value,&v); val = cs_fmt_int(s->a, v & 0xFF); }
    else if (opt_is_unknown16(value)) val = opt_get_L_from_unknown_value(s->a, value);
    else if (opt_is_label(value))
        val = opt_get_L_from_unknown_value(s->a,
                  opt_new_tmp_val16_from_label(s->a, value));
    else val = cs_dup(s->a, value);
    const char *lo, *hi;
    mem_hl_addr(s, addr, &lo, &hi);
    omap_set(s->a, &s->mem, lo, val);
}

/* ---- CPUState ----------------------------------------------------- */

CPUState *cpustate_new(Arena *a) {
    CPUState *s = (CPUState *)arena_alloc(a, sizeof(CPUState));
    s->a = a;
    omap_init(&s->regs);
    vec_init(s->stack);
    omap_init(&s->mem);
    vec_init(s->ix_ptr);
    cpustate_reset(s, NULL, NULL);
    return s;
}

int cpustate_C(const CPUState *s) { return s->flags0.C; }
int cpustate_Z(const CPUState *s) { return s->flags0.Z; }

static const char *regs_get(CPUState *s, const char *r) {
    return omap_get(&s->regs, r);
}
static void regs_set(CPUState *s, const char *r, const char *v) {
    omap_set(s->a, &s->regs, r, v);
}

/* getv("f") helper used by the flag setters */
static bool getv_internal(CPUState *s, const char *r, long *out);

/* Flag property setters (cpustate.py:182-244). Each: set _flags[0].X,
 * then if not is_unknown(val) and is_number(regs["f"]): patch f bits,
 * else regs["f"] = new_tmp_val(). val here is always 0/1 (callers pass
 * int(...)) or FLAG_NONE (Python None -> is_unknown True). */
static void set_flag_bit(CPUState *s, int *slot, int val,
                         int mask, int shift) {
    *slot = val;
    const char *f = regs_get(s, "f");
    if (val != FLAG_NONE && is_number(f)) {
        long fv = 0; getv_internal(s, "f", &fv);
        regs_set(s, "f", cs_fmt_int(s->a, (fv & mask) | (val << shift)));
    } else {
        regs_set(s, "f", z80h_new_tmp_val(s->a));
    }
}
static void set_C(CPUState *s, int v) { set_flag_bit(s,&s->flags0.C,v,0xFE,0); }
static void set_Z(CPUState *s, int v) { set_flag_bit(s,&s->flags0.Z,v,0xBF,6); }
static void set_P(CPUState *s, int v) { set_flag_bit(s,&s->flags0.P,v,0xFB,2); }
static void set_S(CPUState *s, int v) { set_flag_bit(s,&s->flags0.S,v,0x7F,7); }

void cpustate_reset_flags(CPUState *s) {
    set_C(s, FLAG_NONE); set_Z(s, FLAG_NONE);
    set_P(s, FLAG_NONE); set_S(s, FLAG_NONE);
}

/* reset (cpustate.py:246-289) */
void cpustate_reset(CPUState *s, const OMap *regs, const OMap *mems) {
    omap_free(&s->regs); omap_init(&s->regs);
    vec_free(s->stack);  vec_init(s->stack);
    omap_free(&s->mem);  omap_init(&s->mem);
    s->flags0.C=s->flags0.Z=s->flags0.P=s->flags0.S=FLAG_NONE;
    s->flags1.C=s->flags1.Z=s->flags1.P=s->flags1.S=FLAG_NONE;
    vec_free(s->ix_ptr); vec_init(s->ix_ptr);

    const char *order8 = "abcdefhl";
    for (const char *p = order8; *p; p++) {
        char k[3] = { *p, 0, 0 };
        regs_set(s, k, z80h_new_tmp_val(s->a));
        char kp[3] = { *p, '\'', 0 };
        regs_set(s, kp, z80h_new_tmp_val(s->a));
    }
    regs_set(s, "ixh", z80h_new_tmp_val(s->a));
    regs_set(s, "ixl", z80h_new_tmp_val(s->a));
    regs_set(s, "iyh", z80h_new_tmp_val(s->a));
    regs_set(s, "iyl", z80h_new_tmp_val(s->a));
    regs_set(s, "sp", z80h_new_tmp_val(s->a));
    regs_set(s, "r", z80h_new_tmp_val(s->a));
    regs_set(s, "i", z80h_new_tmp_val(s->a));

    const char *pairs[] = {"af","bc","de","hl"};
    for (int i = 0; i < 4; i++) {
        const char *pr = pairs[i];
        char h[2] = { pr[0], 0 }, l[2] = { pr[1], 0 };
        char hp[3] = { pr[0], '\'', 0 }, lp[3] = { pr[1], '\'', 0 };
        char buf[80];
        snprintf(buf, sizeof(buf), "%s%s%s", regs_get(s,h), HL_SEP, regs_get(s,l));
        regs_set(s, pr, buf);
        char pp[4] = { pr[0], pr[1], '\'', 0 };
        snprintf(buf, sizeof(buf), "%s%s%s", regs_get(s,hp), HL_SEP, regs_get(s,lp));
        regs_set(s, pp, buf);
    }
    { char buf[80];
      snprintf(buf,sizeof(buf),"%s%s%s",regs_get(s,"ixh"),HL_SEP,regs_get(s,"ixl"));
      regs_set(s,"ix",buf);
      snprintf(buf,sizeof(buf),"%s%s%s",regs_get(s,"iyh"),HL_SEP,regs_get(s,"iyl"));
      regs_set(s,"iy",buf); }

    if (regs) omap_update(s->a, &s->regs, regs);
    if (mems) {
        omap_update(s->a, &s->mem, mems);
        for (int i = 0; i < mems->len; i++) {
            const char *rg,*sg,*ag;
            if (opt_idx_args(s->a, mems->data[i].key, &rg,&sg,&ag)) {
                IxPtr ip = { (char*)rg,(char*)sg,(char*)ag };
                /* set add: only if not present */
                bool found=false;
                for (int j=0;j<s->ix_ptr.len;j++)
                    if (!strcmp(s->ix_ptr.data[j].reg,rg)&&
                        !strcmp(s->ix_ptr.data[j].sign,sg)&&
                        !strcmp(s->ix_ptr.data[j].off,ag)){found=true;break;}
                if(!found) vec_push(s->ix_ptr, ip);
            }
        }
    }
    cpustate_reset_flags(s);
}

/* get (cpustate.py:437-471). Returns NULL == Python None. */
const char *cpustate_get(CPUState *s, const char *r0) {
    if (r0 == NULL) return NULL;
    const char *r = r0;
    /* r = str(r) — already str */
    char low[16]; size_t rn = strlen(r);
    bool small = rn < sizeof(low);
    if (small) { for (size_t i=0;i<rn;i++) low[i]=(char)tolower((unsigned char)r[i]); low[rn]=0; }

    if (small && strcmp(low,"(sp)")==0 && s->stack.len)
        return s->stack.data[s->stack.len-1];

    if (small && (!strcmp(low,"(hl)")||!strcmp(low,"(bc)")||!strcmp(low,"(de)"))) {
        char rr[3] = { low[1], low[2], 0 };
        const char *i = regs_get(s, rr);
        return mem_read8(s, i);
    }

    if (r[0]=='(') {
        /* v_ = r[1:-1].strip() */
        const char *b=r+1; const char *e=r+strlen(r);
        if (e>b && e[-1]==')') e--;
        while (b<e && (*b==' '||*b=='\t')) b++;
        while (e>b && (e[-1]==' '||e[-1]=='\t')) e--;
        char *v_ = arena_strndup(s->a,b,(size_t)(e-b));
        const char *rg,*sg,*ag;
        if (opt_idx_args(s->a, v_, &rg,&sg,&ag)) {
            char buf[96]; snprintf(buf,sizeof(buf),"%s%s%s",rg,sg,ag);
            char *key=cs_dup(s->a,buf);
            bool found=false;
            for(int j=0;j<s->ix_ptr.len;j++)
                if(!strcmp(s->ix_ptr.data[j].reg,rg)&&
                   !strcmp(s->ix_ptr.data[j].sign,sg)&&
                   !strcmp(s->ix_ptr.data[j].off,ag)){found=true;break;}
            if(!found){IxPtr ip={(char*)rg,(char*)sg,(char*)ag};vec_push(s->ix_ptr,ip);}
            return mem_read8(s, key);
        }
        return mem_read16(s, v_);
    }

    if (is_number(r)) { long v; valnum(r,&v); return cs_fmt_int(s->a, v); }
    if (is_unknown(r)) return cs_dup(s->a, r);
    if (!is_register(small ? low : r)) return cs_dup(s->a, r);
    /* return self.regs[r_] : missing key -> Python KeyError; the
     * optimizer only get()s registers that reset() populated, so the
     * key is always present (faithful: same as Python's guarantee). */
    return regs_get(s, small ? low : r);
}

static bool getv_internal(CPUState *s, const char *r, long *out) {
    const char *v = cpustate_get(s, r);
    if (is_unknown(v)) return false;
    if (v == NULL) return false;
    /* int(v) — Python int() of decimal string; ValueError -> None */
    char *end=NULL;
    long val = strtol(v, &end, 10);
    if (end == v || (end && *end != '\0')) return false;
    *out = val; return true;
}
bool cpustate_getv(CPUState *s, const char *r, long *out) {
    return getv_internal(s, r, out);
}

/* set (cpustate.py:344-435) */
static void cpu_set(CPUState *s, const char *r, const char *orig_or_int);

/* set called with an int value (Python passes int) — render to str the
 * same way Python's f-string / str() would, then go through cpu_set. */
static void cpu_set_int(CPUState *s, const char *r, long v) {
    cpu_set(s, r, cs_fmt_int(s->a, v));
}
/* set(r, None) */
static void cpu_set_none(CPUState *s, const char *r) {
    cpu_set(s, r, NULL);
}

static void cpu_set(CPUState *s, const char *r0, const char *orig_val) {
    /* val = self.get(val) ; is_num = is_number(val) */
    const char *gv = cpustate_get(s, orig_val);
    bool is_num = is_number(gv);
    char *val;
    if (gv == NULL) val = opt_new_tmp_val16(s->a);
    else val = cs_dup(s->a, gv);
    if (is_num) { long v; valnum(val,&v); val = cs_fmt_int(s->a, v & 0xFFFF); }

    /* if self.getv(r) == val: return */
    {
        long rv;
        bool have = getv_internal(s, r0, &rv);
        if (have) { char *rs=cs_fmt_int(s->a,rv); if(strcmp(rs,val)==0) return; }
    }

    const char *r = r0;
    if (strcmp(r,"(sp)")==0) {
        if (s->stack.len==0) vec_push(s->stack, opt_new_tmp_val16(s->a));
        s->stack.data[s->stack.len-1] = val;
        return;
    }
    if (!strcmp(r,"(hl)")||!strcmp(r,"(bc)")||!strcmp(r,"(de)")) {
        char rr[3]={r[1],r[2],0};
        const char *rg = regs_get(s, rr);
        mem_write8(s, rg, val);
        return;
    }
    if (r[0]=='(') {
        /* r = r[1:-1].strip() */
        const char *b=r+1; const char *e=r+strlen(r);
        if (e>b && e[-1]==')') e--;
        while (b<e && (*b==' '||*b=='\t')) b++;
        while (e>b && (e[-1]==' '||e[-1]=='\t')) e--;
        char *rr = arena_strndup(s->a,b,(size_t)(e-b));
        const char *rg,*sg,*ag;
        if (opt_idx_args(s->a, rr, &rg,&sg,&ag)) {
            char buf[96]; snprintf(buf,sizeof(buf),"%s%s%s",rg,sg,ag);
            char *key=cs_dup(s->a,buf);
            bool found=false;
            for(int j=0;j<s->ix_ptr.len;j++)
                if(!strcmp(s->ix_ptr.data[j].reg,rg)&&
                   !strcmp(s->ix_ptr.data[j].sign,sg)&&
                   !strcmp(s->ix_ptr.data[j].off,ag)){found=true;break;}
            if(!found){IxPtr ip={(char*)rg,(char*)sg,(char*)ag};vec_push(s->ix_ptr,ip);}
            char *lo = z80h_LO16_val(s->a, val);
            mem_write8(s, key, lo);
            return;
        }
        if (is_8bit_oper_register(orig_val ? orig_val : "")) {
            mem_write8(s, rr, val);
            return;
        }
        if (opt_is_unknown8(val)) {
            char buf[80]; snprintf(buf,sizeof(buf),"%s%s%s",
                z80h_new_tmp_val(s->a),HL_SEP,val);
            val = cs_dup(s->a, buf);
        }
        mem_write16(s, rr, val);
        return;
    }

    if (is_8bit_oper_register(r)) {
        /* oldval = self.getv(r) — int|None. */
        char *nv;
        if (is_num) { long v; valnum(val,&v); nv = cs_fmt_int(s->a, v & 0xFF); }
        else nv = opt_get_L_from_unknown_value(s->a, val); /* val is unknown */
        /* `if val == oldval: return` — Python compares the str `val`
         * (post-transform) to oldval = self.getv(r), which is int|None.
         * A str is never == an int|None in Python, so this never
         * early-returns. Faithfully a no-op (verbatim Python behaviour). */
        regs_set(s, r, nv);
        const char *hl = map16(r);
        if (hl == NULL) return;
        const char *h8 = NULL, *l8 = NULL; map8(hl,&h8,&l8);
        const char *h_=regs_get(s,h8), *l_=regs_get(s,l8);
        if (is_number(h_) && is_number(l_)) {
            long hh,ll; valnum(h_,&hh); valnum(l_,&ll);
            regs_set(s, hl, cs_fmt_int(s->a,(hh<<8)|ll));
            return;
        }
        char buf[80]; snprintf(buf,sizeof(buf),"%s%s%s",h_,HL_SEP,l_);
        regs_set(s, hl, buf);
        return;
    }

    /* a 16 bit reg. assert r in self.regs */
    if (opt_is_unknown8(val)) {
        char buf[80]; snprintf(buf,sizeof(buf),"%s%s%s",
            z80h_new_tmp_val(s->a),HL_SEP,val);
        val = cs_dup(s->a, buf);
    }
    regs_set(s, r, val);
    if (is_16bit_composed_register(r)) {
        if (!is_num) {
            const char *vv = val;
            if (opt_is_label(val)) vv = opt_new_tmp_val16_from_label(s->a,val);
            /* val.split(HL_SEP) -> HI16(r), LO16(r) */
            const char *bar = strchr(vv,'|');
            const char *hi,*lo;
            if (bar) { hi=arena_strndup(s->a,vv,(size_t)(bar-vv)); lo=bar+1; }
            else { hi=vv; lo=""; }
            regs_set(s, z80h_HI16(s->a,r), hi);
            regs_set(s, z80h_LO16(s->a,r), lo);
        } else {
            long v; valnum(val,&v);
            regs_set(s, z80h_HI16(s->a,r), cs_fmt_int(s->a, v>>8));
            regs_set(s, z80h_LO16(s->a,r), cs_fmt_int(s->a, v&0xFF));
        }
        if (strchr(r,'f')) cpustate_reset_flags(s);
    }
}

/* set_flag (cpustate.py:494-505) */
static void cpu_set_flag(CPUState *s, const char *val) {
    if (!is_number(val)) {
        regs_set(s, "f", z80h_new_tmp_val(s->a));
        cpustate_reset_flags(s);
        return;
    }
    cpu_set(s, "f", val);
    long v; valnum(val,&v);
    set_C(s, (int)(v & 1));
    set_P(s, (int)((v >> 2) & 1));
    set_Z(s, (int)((v >> 6) & 1));
    set_S(s, (int)((v >> 7) & 1));
}

/* shift_idx_regs_refs / clear_idx_reg_refs (cpustate.py:298-342). Only
 * exercised by inc/dec ix/iy; ported faithfully. */
static void clear_idx_reg_refs(CPUState *s, const char *r0) {
    char r[3] = { (char)tolower((unsigned char)r0[0]),
                  (char)tolower((unsigned char)r0[1]), 0 };
    for (int k = s->ix_ptr.len - 1; k >= 0; k--) {
        IxPtr *ip = &s->ix_ptr.data[k];
        if (strcmp(ip->reg, r) != 0) continue;
        if (!is_number(ip->off)) {
            char buf[96]; snprintf(buf,sizeof(buf),"%s%s%s",ip->reg,ip->sign,ip->off);
            omap_del(&s->mem, buf);
            for (int j=k;j<s->ix_ptr.len-1;j++) s->ix_ptr.data[j]=s->ix_ptr.data[j+1];
            s->ix_ptr.len--;
        }
    }
}
static void shift_idx_regs_refs(CPUState *s, const char *r0, int offset) {
    if (offset == 0) return;
    clear_idx_reg_refs(s, r0);
    char r[3] = { (char)tolower((unsigned char)r0[0]),
                  (char)tolower((unsigned char)r0[1]), 0 };
    if (offset > 0) {
        for (int i = -128; i < 128; i++) {
            char idx[16], old[16];
            snprintf(idx,sizeof(idx),"%s%+d",r,i);
            snprintf(old,sizeof(old),"%s%+d",r,offset+i);
            const char *v = (offset+i>127) ? z80h_new_tmp_val(s->a)
                                           : mem_read8(s, old);
            mem_write8(s, idx, v);
        }
    } else {
        for (int i = 127; i > -129; i--) {
            char idx[16], old[16];
            snprintf(idx,sizeof(idx),"%s%+d",r,i);
            snprintf(old,sizeof(old),"%s%+d",r,offset+i);
            const char *v = (offset+i<-128) ? z80h_new_tmp_val(s->a)
                                            : mem_read8(s, old);
            mem_write8(s, idx, v);
        }
    }
}

/* inc / dec (cpustate.py:507-576) */
static void cpu_inc(CPUState *s, const char *r) {
    if (!is_register(r)) {
        if (r[0]=='(') {
            long v_; bool have = getv_internal(s, r, &v_);
            long nv;
            if (have) { nv=(v_+1)&0xFF; set_Z(s,(int)(nv==0)); set_C(s,(int)(nv==0)); }
            else { char *t=z80h_new_tmp_val(s->a); cpu_set_flag(s,NULL);
                   /* v_ = new_tmp_val() then write str(v_) */
                   const char *r_inner;
                   { const char *b=r+1,*e=r+strlen(r); if(e>b&&e[-1]==')')e--;
                     r_inner=arena_strndup(s->a,b,(size_t)(e-b)); }
                   mem_write8(s, cpustate_get(s,r_inner), t); return; }
            const char *b=r+1,*e=r+strlen(r); if(e>b&&e[-1]==')')e--;
            char *r_=arena_strndup(s->a,b,(size_t)(e-b));
            mem_write8(s, cpustate_get(s,r_), cs_fmt_int(s->a,nv));
        }
        return;
    }
    { long v = 0; if (getv_internal(s,r,&v)) cpu_set_int(s,r,v+1); else cpu_set_none(s,r); }
    if (!strcmp(r,"ix")||!strcmp(r,"iy")) shift_idx_regs_refs(s,r,1);
    if (!is_8bit_oper_register(r)) return;
    if (is_unknown(regs_get(s,r))) { cpu_set_flag(s,NULL); return; }
    { long v = 0; getv_internal(s,r,&v); set_Z(s,(int)(v==0)); set_C(s,(int)(v==0)); }
}
static void cpu_dec(CPUState *s, const char *r) {
    if (!is_register(r)) {
        long v_; bool have = getv_internal(s, r, &v_);
        long nv;
        if (have) { nv=(v_-1)&0xFF; set_Z(s,(int)(nv==0)); set_C(s,(int)(nv==0xFF)); }
        else { char *t=z80h_new_tmp_val(s->a); cpu_set_flag(s,NULL);
               const char *b=r+1,*e=r+strlen(r); if(e>b&&e[-1]==')')e--;
               char *r_=arena_strndup(s->a,b,(size_t)(e-b));
               mem_write8(s, cpustate_get(s,r_), t); return; }
        const char *b=r+1,*e=r+strlen(r); if(e>b&&e[-1]==')')e--;
        char *r_=arena_strndup(s->a,b,(size_t)(e-b));
        mem_write8(s, cpustate_get(s,r_), cs_fmt_int(s->a,nv));
        return;
    }
    { long v = 0; if (getv_internal(s,r,&v)) cpu_set_int(s,r,v-1); else cpu_set_none(s,r); }
    if (!strcmp(r,"ix")||!strcmp(r,"iy")) shift_idx_regs_refs(s,r,-1);
    if (!is_8bit_oper_register(r)) return;
    if (is_unknown(regs_get(s,r))) { cpu_set_flag(s,NULL); return; }
    { long v = 0; getv_internal(s,r,&v); set_Z(s,(int)(v==0)); set_C(s,(int)(v==0xFF)); }
}

/* rrc/rr/rlc/rl (cpustate.py:578-622) */
static void cpu_rrc(CPUState *s, const char *r) {
    if (!is_number(regs_get(s,r))) { cpu_set_none(s,r); cpu_set_flag(s,NULL); return; }
    long v = 0; getv_internal(s, regs_get(s,r), &v); v &= 0xFF;
    regs_set(s, r, cs_fmt_int(s->a, (v>>1)|((v&1)<<7)));
}
static void cpu_rr(CPUState *s, const char *r) {
    if (s->flags0.C==FLAG_NONE || !is_number(regs_get(s,r))) {
        cpu_set_none(s,r); cpu_set_flag(s,NULL); return; }
    cpu_rrc(s,r);
    int tmp = s->flags0.C;
    long v = 0; getv_internal(s, regs_get(s,r), &v);
    set_C(s, (int)(v>>7));
    regs_set(s, r, cs_fmt_int(s->a, (v&0x7F)|(tmp<<7)));
}
static void cpu_rlc(CPUState *s, const char *r) {
    if (!is_number(regs_get(s,r))) { cpu_set_none(s,r); cpu_set_flag(s,NULL); return; }
    long v = 0; getv_internal(s, regs_get(s,r), &v); v &= 0xFF;
    cpu_set_int(s, r, ((v<<1)&0xFF)|(v>>7));
}
static void cpu_rl(CPUState *s, const char *r) {
    if (s->flags0.C==FLAG_NONE || !is_number(regs_get(s,r))) {
        cpu_set_none(s,r); cpu_set_flag(s,NULL); return; }
    cpu_rlc(s,r);
    int tmp = s->flags0.C;
    long v = 0; getv_internal(s, regs_get(s,r), &v);
    set_C(s, (int)(v&1));
    regs_set(s, r, cs_fmt_int(s->a, (v&0xFE)|tmp));
}

/* execute (cpustate.py:643-893) */
void cpustate_execute(CPUState *s, const char *asm_code) {
    Z80Asm *A = z80asm_new(s->a, asm_code);
    if (A->is_label) return;
    const char *i = A->inst;

    /* o = list(asm.oper); lower the register operands */
    Z80StrList o; vec_init(o);
    for (int k=0;k<A->oper.len;k++) {
        const char *x=A->oper.data[k];
        if (is_register(x)) {
            size_t n=strlen(x);
            char *lw=(char*)arena_alloc(s->a,n+1);
            for(size_t j=0;j<n;j++) lw[j]=(char)tolower((unsigned char)x[j]);
            lw[n]=0; vec_push(o,lw);
        } else vec_push(o, (char*)x);
    }
    #define O(k) ((k)<o.len?o.data[k]:"")
    #define OLEN o.len

    if (!strcmp(i,"ld")) { cpu_set(s,O(0),O(1)); goto done; }

    if (!strcmp(i,"push")) {
        long sp;
        if (valnum(regs_get(s,"sp"),&sp)) {
            long v = 0; getv_internal(s,"sp",&v); cpu_set_int(s,"sp",(v-2)%0xFFFF);
        } else cpu_set_none(s,"sp");
        vec_push(s->stack, (char*)regs_get(s,O(0)));
        goto done;
    }
    if (!strcmp(i,"pop")) {
        if (s->stack.len) { char *t=s->stack.data[s->stack.len-1];
            s->stack.len--; cpu_set(s,O(0),t); }
        else cpu_set_none(s,O(0));
        long sp;
        if (valnum(regs_get(s,"sp"),&sp) && sp) {
            long v = 0; getv_internal(s,"sp",&v); cpu_set_int(s,"sp",(v+2)%0xFFFF);
        } else cpu_set_none(s,"sp");
        goto done;
    }
    if (!strcmp(i,"inc")) { cpu_inc(s,O(0)); goto done; }
    if (!strcmp(i,"dec")) { cpu_dec(s,O(0)); goto done; }
    if (!strcmp(i,"rra")) { cpu_rr(s,"a"); goto done; }
    if (!strcmp(i,"rla")) { cpu_rl(s,"a"); goto done; }
    if (!strcmp(i,"rlca")) { cpu_rlc(s,"a"); goto done; }
    if (!strcmp(i,"rrca")) { cpu_rrc(s,"a"); goto done; }
    if (!strcmp(i,"rr")) { cpu_rr(s,O(0)); goto done; }
    if (!strcmp(i,"rl")) { cpu_rl(s,O(0)); goto done; }

    if (!strcmp(i,"exx")) {
        const char *J[]={"bc","de","hl","b","c","d","e","h","l"};
        for (size_t k=0;k<9;k++) {
            char p[4]; snprintf(p,sizeof(p),"%s'",J[k]);
            const char *a1=regs_get(s,J[k]); const char *a2=regs_get(s,p);
            char *t1=a1?cs_dup(s->a,a1):NULL, *t2=a2?cs_dup(s->a,a2):NULL;
            regs_set(s,J[k],t2); regs_set(s,p,t1);
        }
        goto done;
    }
    if (!strcmp(i,"ex")) {
        if (OLEN==2 && !strcmp(O(0),"de") && !strcmp(O(1),"hl")) {
            const char *P[][2]={{"de","hl"},{"d","h"},{"e","l"}};
            for (int k=0;k<3;k++) {
                const char *x=regs_get(s,P[k][0]),*y=regs_get(s,P[k][1]);
                char *tx=x?cs_dup(s->a,x):NULL,*ty=y?cs_dup(s->a,y):NULL;
                regs_set(s,P[k][0],ty); regs_set(s,P[k][1],tx);
            }
        } else {
            const char *J[]={"af","a","f"};
            for (int k=0;k<3;k++) {
                char p[4]; snprintf(p,sizeof(p),"%s'",J[k]);
                const char *x=regs_get(s,J[k]),*y=regs_get(s,p);
                char *tx=x?cs_dup(s->a,x):NULL,*ty=y?cs_dup(s->a,y):NULL;
                regs_set(s,J[k],ty); regs_set(s,p,tx);
            }
        }
        goto done;
    }

    if (!strcmp(i,"xor")) {
        set_C(s,0);
        if (!strcmp(O(0),"a")) { cpu_set_int(s,"a",0); set_Z(s,1); goto done; }
        long av,ov;
        if (!getv_internal(s,"a",&av) || !getv_internal(s,O(0),&ov)) {
            set_Z(s,FLAG_NONE); cpu_set_none(s,"a"); goto done; }
        cpu_set_int(s,"a",av^ov);
        { const char *ga=cpustate_get(s,"a"); set_Z(s,(int)(ga&&strcmp(ga,"0")==0)); }
        goto done;
    }
    if (!strcmp(i,"or")||!strcmp(i,"and")) {
        set_C(s,0);
        long av,ov;
        if (!getv_internal(s,"a",&av)||!getv_internal(s,O(0),&ov)) {
            set_Z(s,FLAG_NONE); cpu_set_none(s,"a"); goto done; }
        if (!strcmp(i,"or")) cpu_set_int(s,"a",av|ov);
        else cpu_set_int(s,"a",av&ov);
        { const char *ga=cpustate_get(s,"a"); set_Z(s,(int)(ga&&strcmp(ga,"0")==0)); }
        goto done;
    }

    if (!strcmp(i,"adc")||!strcmp(i,"sbc")) {
        const char *o0,*o1;
        if (OLEN==1) { o0="a"; o1=O(0); } else { o0=O(0); o1=O(1); }
        if (s->flags0.C==FLAG_NONE) { set_Z(s,FLAG_NONE); cpu_set_none(s,o0); goto done; }
        if (!strcmp(i,"sbc") && !strcmp(o0,o1)) {
            set_Z(s,(int)(!s->flags0.C)); cpu_set_int(s,o0,-s->flags0.C); goto done; }
        long v0,v1;
        if (!getv_internal(s,o0,&v0)||!getv_internal(s,o1,&v1)) {
            cpu_set_flag(s,NULL); cpu_set_none(s,o0); goto done; }
        if (!strcmp(i,"adc")) {
            long val=v0+v1+s->flags0.C;
            if (is_8bit_oper_register(o0)) set_C(s,(int)(val>0xFF));
            else set_C(s,(int)(val>0xFFFF));
            cpu_set_int(s,o0,val); goto done;
        }
        long val=v0-v1-s->flags0.C;
        set_C(s,(int)(val<0)); set_Z(s,(int)(val==0)); cpu_set_int(s,o0,val);
        goto done;
    }

    if (!strcmp(i,"add")||!strcmp(i,"sub")) {
        const char *o0,*o1;
        if (OLEN==1) { o0="a"; o1=O(0); } else { o0=O(0); o1=O(1); }
        if (!strcmp(i,"sub") && !strcmp(o0,o1)) {
            set_Z(s,1); set_C(s,0); cpu_set_int(s,o0,0); goto done; }
        /* if not is_number(self.get(o0)) or is_number(self.get(o1)) is not None:
         *   set_flag(None); set(o0,None)  — faithful to the Python (a known
         *   latent quirk: the 2nd clause is always True so this branch
         *   nearly always taken). Replicated verbatim. */
        bool c0 = is_number(cpustate_get(s,o0));
        bool c1_isnum = is_number(cpustate_get(s,o1)); /* "is not None" on a
            bool is always True in Python -> the OR is True unless c0 True
            AND (False is not None == True) ... i.e. `(not c0) or True` */
        (void)c1_isnum;
        if (!c0 || true) { cpu_set_flag(s,NULL); cpu_set_none(s,o0); goto done; }
        /* unreachable (matches Python: the second operand of `or` is a
         * bool, `bool is not None` is always True). Kept for provenance. */
    }

    if (!strcmp(i,"neg")) {
        long av;
        if (!getv_internal(s,"a",&av)) { cpu_set_none(s,"a"); cpu_set_flag(s,NULL); goto done; }
        long val=-av;
        cpu_set_int(s,"a",val);
        set_Z(s,(int)(!val));
        set_C(s,(int)(!(s->flags0.Z)));
        val &= 0xFF; set_S(s,(int)(val>>7));
        goto done;
    }
    if (!strcmp(i,"scf")) { set_C(s,1); goto done; }
    if (!strcmp(i,"ccf")) { if (s->flags0.C!=FLAG_NONE) set_C(s,(int)(!s->flags0.C)); goto done; }
    if (!strcmp(i,"cpl")) {
        long av;
        if (!getv_internal(s,"a",&av)) { cpu_set_none(s,"a"); goto done; }
        cpu_set_int(s,"a",0xFF^av); goto done;
    }
    if (!strcmp(i,"cp")) {
        long v0; bool have=getv_internal(s,O(0),&v0);
        const char *fa=regs_get(s,"a");
        if (!is_number(fa) || !have) { cpu_set_flag(s,NULL); goto done; }
        long av; valnum(fa,&av);
        long val=av-v0;
        set_Z(s,(int)(val==0)); set_C(s,(int)(val<0)); set_S(s,(int)(val<0));
        goto done;
    }
    if (!strcmp(i,"jp")||!strcmp(i,"jr")||!strcmp(i,"ret")||
        !strcmp(i,"rst")||!strcmp(i,"call")) goto done;
    if (!strcmp(i,"djnz")) {
        long bv;
        if (!getv_internal(s,"b",&bv)) { cpu_set_none(s,"b"); set_Z(s,FLAG_NONE); goto done; }
        long val=(bv-1)&0xFF; cpu_set_int(s,"b",val); set_Z(s,(int)(val==0));
        goto done;
    }
    if (!strcmp(i,"out")) goto done;
    if (!strcmp(i,"in")) { cpu_set_none(s,O(0)); goto done; }

    /* Unknown. Resets ALL */
    cpustate_reset(s, NULL, NULL);

done:
    #undef O
    #undef OLEN
    vec_free(o);
}
