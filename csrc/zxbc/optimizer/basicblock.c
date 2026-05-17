/*
 * basicblock.c — Port of src/arch/z80/optimizer/basicblock.py
 * (BasicBlock + DummyBasicBlock), the surface main.py / flow_graph.py
 * drive at O>2. Each function cites its basicblock.py lines.
 *
 * Sets (comes_from / goes_to / called_by / used_by) are insertion-ordered
 * vecs with add-if-absent semantics. Python `set` iteration order is
 * unspecified, so this is a faithful representation; O3 byte-calibration
 * vs Python-O3 is S5.9c/S5.10 scope (this slice is the inert-at-O2
 * scaffold — the whole file is unreachable at O<=2 via main.py:199).
 */
#include "optimizer.h"
#include "opthelpers.h"
#include "peephole/engine.h"
#include "peephole/evaluator.h"
#include "peephole/pattern.h"
#include "peephole/template.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* common.ASMS membership — backend ##ASM table. The optimizer evaluates
 * ordinary emitted asm; no path inserts an evaluated instruction key
 * into ASMS (same documented invariant as peephole/memcell.c). */
static bool code_in_ASMS(const char *code) { (void)code; return false; }

static char *bb_strdup(Arena *a, const char *s) { return arena_strdup(a, s); }

/* patterns.RE_ID_OR_NUMBER = [0-9]+|[a-zA-Z_][a-zA-Z_0-9]* — re.findall
 * returns non-overlapping leftmost matches. */
static Z80StrList re_id_or_number(Arena *a, const char *s) {
    Z80StrList v; vec_init(v);
    const char *p = s;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c >= '0' && c <= '9') {
            const char *st = p;
            while (*p >= '0' && *p <= '9') p++;
            vec_push(v, arena_strndup(a, st, (size_t)(p - st)));
        } else if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_') {
            const char *st = p;
            p++;
            while ((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||
                   (*p>='0'&&*p<='9')||*p=='_') p++;
            vec_push(v, arena_strndup(a, st, (size_t)(p - st)));
        } else p++;
    }
    return v;
}

/* ---- set helpers (add-if-absent / remove) ------------------------- */
static bool bbvec_has(const BBVec *v, BasicBlock *x) {
    for (int i = 0; i < v->len; i++) if (v->data[i] == x) return true;
    return false;
}
static void bbvec_add(BBVec *v, BasicBlock *x) {
    if (!bbvec_has(v, x)) vec_push(*v, x);
}
static void bbvec_remove(BBVec *v, BasicBlock *x) {
    for (int i = 0; i < v->len; i++)
        if (v->data[i] == x) {
            for (int j = i; j < v->len - 1; j++) v->data[j] = v->data[j+1];
            v->len--; return;
        }
}

/* ---- construction (basicblock.py:49-71, 116-131) ------------------ */
static void bb_base_init(BasicBlock *b, Arena *a, Optimizer *opt) {
    b->a = a;
    b->optimizer = opt;
    vec_init(b->mem);
    b->next = NULL; b->prev = NULL;
    b->lock = false;
    vec_init(b->comes_from); vec_init(b->goes_to); vec_init(b->called_by);
    b->modified = false;
    b->ignored = false;
    opt->unique_id += 1;          /* __new__: cls.__UNIQUE_ID += 1 */
    b->id = opt->unique_id;       /* self.id = BasicBlock.__UNIQUE_ID */
    b->optimized = false;
    b->cpu = cpustate_new(a);     /* self.cpu = CPUState() */
    b->is_dummy = false;
    vec_init(b->dummy_destroys);
    vec_init(b->dummy_requires);
}

/* _set_code (basicblock.py:120-131): clean_asm_args -> simplify_asm_args */
void bb_set_code(BasicBlock *b, const char *const *mem, int n) {
    vec_free(b->mem); vec_init(b->mem);
    bool clean = b->optimizer->clean_asm_args;
    for (int i = 0; i < n; i++) {
        const char *asm_ = mem[i];
        if (clean) asm_ = opt_simplify_asm_args(b->a, asm_);
        vec_push(b->mem, omemcell_new(b->a, asm_, i));
    }
}

BasicBlock *bb_new(Arena *a, Optimizer *opt, const char *const *mem, int n) {
    BasicBlock *b = (BasicBlock *)arena_alloc(a, sizeof(BasicBlock));
    bb_base_init(b, a, opt);
    /* self.code = list(memory) -> the code setter -> _set_code */
    bb_set_code(b, mem, n);
    return b;
}

/* DummyBasicBlock.__init__ (basicblock.py:504-508) */
BasicBlock *bb_new_dummy(Arena *a, Optimizer *opt,
                         const Z80StrList *destroys,
                         const Z80StrList *requires_) {
    BasicBlock *b = (BasicBlock *)arena_alloc(a, sizeof(BasicBlock));
    bb_base_init(b, a, opt);
    b->is_dummy = true;
    for (int i = 0; i < destroys->len; i++)
        vec_push(b->dummy_destroys, bb_strdup(a, destroys->data[i]));
    for (int i = 0; i < requires_->len; i++)
        vec_push(b->dummy_requires, bb_strdup(a, requires_->data[i]));
    /* self.code = ["ret"] */
    const char *ret[] = { "ret" };
    bb_set_code(b, ret, 1);
    return b;
}

/* code property (basicblock.py:112-114): [x.code for x in self.mem] */
Z80StrList bb_code(Arena *a, BasicBlock *b) {
    Z80StrList v; vec_init(v);
    for (int i = 0; i < b->mem.len; i++)
        vec_push(v, bb_strdup(a, omemcell_code(b->mem.data[i])));
    return v;
}

/* jump_labels / opt_labels properties (basicblock.py:98-104) */
#define JUMP_LABELS (&b->optimizer->JUMP_LABELS)
#define OPT_LABELS  (&b->optimizer->LABELS)

static bool strlist_has(const Z80StrList *v, const char *s) {
    for (int i = 0; i < v->len; i++) if (strcmp(v->data[i], s) == 0) return true;
    return false;
}

/* get_first_partition_idx (basicblock.py:165-176) */
int bb_get_first_partition_idx(BasicBlock *b) {
    int n = b->mem.len;
    for (int i = 0; i < n; i++) {
        OMemCell *mem = b->mem.data[i];
        if (i > 0 && mem->is_label && strlist_has(JUMP_LABELS, mem->inst))
            return i;
        if ((mem->is_ender || code_in_ASMS(omemcell_code(mem))) && i < n - 1)
            return i + 1;
    }
    return -1;
}

/* add/delete comes_from/goes_to (basicblock.py:185-236) */
void bb_delete_comes_from(BasicBlock *b, BasicBlock *o) {
    if (o == NULL) return;
    if (!bbvec_has(&b->comes_from, o)) return;
    bbvec_remove(&b->comes_from, o);
    bbvec_remove(&o->goes_to, b);
}
void bb_delete_goes_to(BasicBlock *b, BasicBlock *o) {
    if (o == NULL) return;
    if (!bbvec_has(&b->goes_to, o)) return;
    bbvec_remove(&b->goes_to, o);
    bbvec_remove(&o->comes_from, b);
}
void bb_add_comes_from(BasicBlock *b, BasicBlock *o) {
    if (o == NULL) return;
    if (bbvec_has(&b->comes_from, o)) return;
    bbvec_add(&b->comes_from, o);
    bbvec_add(&o->goes_to, b);
}
void bb_add_goes_to(BasicBlock *b, BasicBlock *o) {
    if (o == NULL) return;
    if (bbvec_has(&b->goes_to, o)) return;
    bbvec_add(&b->goes_to, o);
    bbvec_add(&o->comes_from, b);
}

/* update_next_block (basicblock.py:238-267) */
void bb_update_next_block(BasicBlock *b) {
    OMemCell *last = b->mem.data[b->mem.len - 1];
    static const char *ENDS[] = {"djnz","jp","jr","call","ret","reti","retn","rst"};
    bool in_ends = false;
    for (size_t k = 0; k < sizeof(ENDS)/sizeof(ENDS[0]); k++)
        if (strcmp(last->inst, ENDS[k]) == 0) { in_ends = true; break; }
    if (!in_ends) return;

    if (strcmp(last->inst,"reti")==0 || strcmp(last->inst,"retn")==0) {
        if (b->next != NULL) bb_delete_comes_from(b->next, b);
        return;
    }
    if (b->next != NULL && last->cond == NULL) {
        bb_delete_comes_from(b->next, b);
        /* for blk in self.goes_to: self.delete_goes_to(blk) — Python
         * mutates the set during iteration over it (a known latent
         * quirk); replicate by snapshotting then deleting. */
        BBVec snap; vec_init(snap);
        for (int k = 0; k < b->goes_to.len; k++) vec_push(snap, b->goes_to.data[k]);
        for (int k = 0; k < snap.len; k++) bb_delete_goes_to(b, snap.data[k]);
        vec_free(snap);
    }
    if (strcmp(last->inst,"ret")==0) return;

    const char *op0 = last->opers.len > 0 ? last->opers.data[0] : "";
    if (!ld_has(OPT_LABELS, op0)) {
        Z80StrList all; vec_init(all);
        static const char *ALL[] = {"a","b","c","d","e","f","h","l",
            "ixh","ixl","iyh","iyl","r","i","sp"};
        for (size_t k=0;k<sizeof(ALL)/sizeof(ALL[0]);k++)
            vec_push(all, bb_strdup(b->a, ALL[k]));
        BasicBlock *dbb = bb_new_dummy(b->a, b->optimizer, &all, &all);
        ld_set(OPT_LABELS, op0, labelinfo_new(b->a, op0, 0, dbb, 0));
        vec_free(all);
    }
    BasicBlock *n_block = ld_get(OPT_LABELS, op0)->basic_block;
    bb_add_goes_to(b, n_block);
}

/* requires (basicblock.py:323-350) */
void bb_requires(BasicBlock *b, int i, int end_, Z80StrList *out) {
    vec_init(*out);
    if (b->is_dummy) {  /* DummyBasicBlock.requires (basicblock.py:513) */
        for (int k=0;k<b->dummy_requires.len;k++)
            vec_push(*out, bb_strdup(b->a, b->dummy_requires.data[k]));
        return;
    }
    if (i < 0) i = 0;
    int n = b->mem.len;
    if (end_ < 0 || end_ > n) end_ = n;
    static const char *REGS0[] = {"a","b","c","d","e","f","h","l","i",
        "ixh","ixl","iyh","iyl","sp"};
    Z80StrList regs; vec_init(regs);
    for (size_t k=0;k<sizeof(REGS0)/sizeof(REGS0[0]);k++)
        vec_push(regs, bb_strdup(b->a, REGS0[k]));
    for (int ii=i; ii<end_; ii++) {
        OMemCell *c = b->mem.data[ii];
        for (int q=0;q<c->requires_.len;q++) {
            const char *r = c->requires_.data[q]; /* already lower in our port */
            if (strlist_has(&regs, r)) {
                if (!strlist_has(out, r)) vec_push(*out, bb_strdup(b->a, r));
                /* regs.remove(r) */
                for (int z=0;z<regs.len;z++) if(!strcmp(regs.data[z],r)){
                    for(int w=z;w<regs.len-1;w++) regs.data[w]=regs.data[w+1];
                    regs.len--; break; }
            }
        }
        for (int q=0;q<c->destroys_.len;q++) {
            const char *r = c->destroys_.data[q];
            for (int z=0;z<regs.len;z++) if(!strcmp(regs.data[z],r)){
                for(int w=z;w<regs.len-1;w++) regs.data[w]=regs.data[w+1];
                regs.len--; break; }
        }
        if (regs.len==0) break;
    }
    vec_free(regs);
}

/* destroys (basicblock.py:352-369) */
void bb_destroys(BasicBlock *b, int i, Z80StrList *out) {
    vec_init(*out);
    if (b->is_dummy) {  /* DummyBasicBlock.destroys (basicblock.py:510) */
        for (int k=0;k<b->dummy_destroys.len;k++)
            vec_push(*out, bb_strdup(b->a, b->dummy_destroys.data[k]));
        return;
    }
    static const char *REGS0[] = {"a","b","c","d","e","f","h","l","i",
        "ixh","ixl","iyh","iyl","sp"};
    Z80StrList regs; vec_init(regs);
    for (size_t k=0;k<sizeof(REGS0)/sizeof(REGS0[0]);k++)
        vec_push(regs, bb_strdup(b->a, REGS0[k]));
    int top = b->mem.len;
    for (int ii=i; ii<top; ii++) {
        OMemCell *c = b->mem.data[ii];
        for (int q=0;q<c->destroys_.len;q++) {
            const char *r = c->destroys_.data[q];
            if (strlist_has(&regs,r)) {
                vec_push(*out, bb_strdup(b->a, r));
                for (int z=0;z<regs.len;z++) if(!strcmp(regs.data[z],r)){
                    for(int w=z;w<regs.len-1;w++) regs.data[w]=regs.data[w+1];
                    regs.len--; break; }
            }
        }
        if (regs.len==0) break;
    }
    vec_free(regs);
}

static bool bb_goes_requires(BasicBlock *b, const Z80StrList *regs);

/* is_used (basicblock.py:269-321) */
bool bb_is_used(BasicBlock *b, const Z80StrList *regs0, int i, int top) {
    if (b->is_dummy) { /* DummyBasicBlock.is_used (basicblock.py:516-517) */
        for (int k=0;k<regs0->len;k++)
            if (strlist_has(&b->dummy_requires, regs0->data[k])) return true;
        return false;
    }
    if (b->lock) return true;
    if (i < 0) i = 0;
    int n = b->mem.len;
    top = (top < 0) ? n : top + 1;

    if (regs0->len && regs0->data[0][0]=='(' &&
        regs0->data[0][strlen(regs0->data[0])-1]==')') {
        const char *R0 = regs0->data[0];
        char *inner = arena_strndup(b->a, R0+1, strlen(R0)-2);
        Z80StrList r16; vec_init(r16);
        if (z80h_is_16bit_oper_register(inner)) {
            Z80StrList sr = z80h_single_registers1(b->a, inner);
            for (int k=0;k<sr.len;k++) vec_push(r16, sr.data[k]);
            vec_free(sr);
        }
        Z80StrList ix; vec_init(ix);
        const char *rg,*sg,*ag;
        if (opt_idx_args(b->a, inner, &rg,&sg,&ag)) {
            Z80StrList sr = z80h_single_registers1(b->a, rg);
            for (int k=0;k<sr.len;k++) vec_push(ix, sr.data[k]);
            vec_free(sr);
        }
        Z80StrList rr; vec_init(rr);
        for (int k=0;k<r16.len;k++) if(!strlist_has(&rr,r16.data[k])) vec_push(rr,r16.data[k]);
        for (int k=0;k<ix.len;k++) if(!strlist_has(&rr,ix.data[k])) vec_push(rr,ix.data[k]);
        /* mem_vars = RE_ID_OR_NUMBER.findall(regs[0]) when rr empty */
        for (int ii=i; ii<top && ii<n; ii++) {
            OMemCell *m = b->mem.data[ii];
            const char *o0 = m->opers.len>0?m->opers.data[0]:NULL;
            const char *olast = m->opers.len>0?m->opers.data[m->opers.len-1]:NULL;
            if (strcmp(m->inst,"ld")==0 && o0 && strcmp(o0,R0)==0) return false;
            if ((!strcmp(m->inst,"and")||!strcmp(m->inst,"or")||
                 !strcmp(m->inst,"xor")) && o0 && strcmp(o0,R0)==0) return true;
            if (m->opers.len && olast && strcmp(olast,R0)==0) return true;
            if (rr.len) {
                for (int q=0;q<m->destroys_.len;q++)
                    if (strlist_has(&r16,m->destroys_.data[q])) return true;
            }
            /* mem_vars = set([] if rr else RE_ID_OR_NUMBER.findall(regs[0]))
             * if mem.opers and mem_vars.intersection(
             *        RE_ID_OR_NUMBER.findall(mem.opers[-1])): return True
             * RE_ID_OR_NUMBER = [0-9]+|[a-zA-Z_][a-zA-Z_0-9]* */
            if (!rr.len && m->opers.len && olast) {
                Z80StrList mv = re_id_or_number(b->a, R0);
                Z80StrList ov = re_id_or_number(b->a, olast);
                bool hit = false;
                for (int x=0; x<mv.len && !hit; x++)
                    for (int y=0; y<ov.len; y++)
                        if (strcmp(mv.data[x], ov.data[y])==0) { hit=true; break; }
                vec_free(mv); vec_free(ov);
                if (hit) return true;
            }
        }
        return true;
    }

    /* regs = flatten_list([single_registers(x) for x in regs]) */
    Z80StrList regs; vec_init(regs);
    for (int k=0;k<regs0->len;k++) {
        Z80StrList sr = z80h_single_registers1(b->a, regs0->data[k]);
        for (int q=0;q<sr.len;q++) vec_push(regs, sr.data[q]);
        vec_free(sr);
    }
    for (int ii=i; ii<top; ii++) {
        OMemCell *c = b->mem.data[ii];
        for (int q=0;q<c->requires_.len;q++)
            if (strlist_has(&regs, c->requires_.data[q])) { vec_free(regs); return true; }
        for (int q=0;q<c->destroys_.len;q++) {
            const char *r=c->destroys_.data[q];
            for (int z=0;z<regs.len;z++) if(!strcmp(regs.data[z],r)){
                for(int w=z;w<regs.len-1;w++) regs.data[w]=regs.data[w+1];
                regs.len--; break; }
        }
        if (regs.len==0) { vec_free(regs); return false; }
    }
    b->lock = true;
    bool result = bb_goes_requires(b, &regs);
    b->lock = false;
    vec_free(regs);
    return result;
}

static bool bb_goes_requires(BasicBlock *b, const Z80StrList *regs) {
    for (int k=0;k<b->goes_to.len;k++)
        if (bb_is_used(b->goes_to.data[k], regs, 0, -1)) return true;
    return false;
}

/* get_first_non_label_instruction (basicblock.py:381-389) */
static OMemCell *bb_first_non_label(BasicBlock *b) {
    for (int i=0;i<b->mem.len;i++)
        if (!b->mem.data[i]->is_label) return b->mem.data[i];
    return NULL;
}

/* get_next_exec_instruction (basicblock.py:391-406) */
OMemCell *bb_get_next_exec_instruction(BasicBlock *b) {
    OMemCell *result = bb_first_non_label(b);
    BasicBlock *blk = b;
    while (result == NULL) {
        if (blk->goes_to.len != 1) return NULL;
        blk = blk->goes_to.data[0];
        result = bb_first_non_label(blk);
    }
    return result;
}

/* guesses_initial_state_from_origin_blocks (basicblock.py:408-422) */
static void bb_guess_initial(BasicBlock *b, OMap *regs, OMap *mems) {
    omap_init(regs); omap_init(mems);
    if (b->comes_from.len == 0) return;
    BasicBlock *first = b->comes_from.data[0]; /* sfirst(comes_from) */
    omap_copy(regs, &first->cpu->regs);  /* regs = first.cpu.regs (ref) */
    OMap r2; omap_init(&r2); omap_update(b->a,&r2,regs); *regs=r2;
    omap_init(mems); omap_update(b->a, mems, &first->cpu->mem);
    for (int k=1;k<b->comes_from.len;k++) {
        BasicBlock *blk = b->comes_from.data[k];
        OMap nr, nm;
        opt_dict_intersection(b->a,&nr,regs,&blk->cpu->regs);
        opt_dict_intersection(b->a,&nm,mems,&blk->cpu->mem);
        omap_free(regs); *regs=nr;
        omap_free(mems); *mems=nm;
    }
}

/* compute_cpu_state (basicblock.py:424-432) */
void bb_compute_cpu_state(BasicBlock *b) {
    cpustate_reset(b->cpu, NULL, NULL);
    Z80StrList code = bb_code(b->a, b);
    for (int i=0;i<code.len;i++) cpustate_execute(b->cpu, code.data[i]);
    vec_free(code);
}

/* ---- optimize (basicblock.py:434-496) ----------------------------- */
/* CPU-state hook context for the monkey-patched UNARY (GVAL/FLAGVAL/
 * IS_REQUIRED). i + len(p.patt) for IS_REQUIRED is updated per match. */
typedef struct OptHookCtx {
    BasicBlock *b;
    int is_required_top;   /* i + len(p.patt) */
} OptHookCtx;

static const char *hook_gval(void *vc, const char *x) {
    OptHookCtx *c = (OptHookCtx *)vc;
    return cpustate_get(c->b->cpu, x);   /* self.cpu.get(x) */
}
static char *hook_flagval(void *vc, const char *x) {
    OptHookCtx *c = (OptHookCtx *)vc;
    char lx[8]; size_t n=strlen(x); if(n>=sizeof(lx)) n=sizeof(lx)-1;
    for (size_t k=0;k<n;k++) lx[k]=(char)tolower((unsigned char)x[k]);
    lx[n]='\0';
    int C = cpustate_C(c->b->cpu), Z = cpustate_Z(c->b->cpu);
    if (strcmp(lx,"c")==0)
        return C!=FLAG_NONE ? (C? arena_strdup(c->b->a,"1")
                                 : arena_strdup(c->b->a,"0"))
                            : z80h_new_tmp_val(c->b->a);
    if (strcmp(lx,"z")==0)
        return Z!=FLAG_NONE ? (Z? arena_strdup(c->b->a,"1")
                                 : arena_strdup(c->b->a,"0"))
                            : z80h_new_tmp_val(c->b->a);
    return z80h_new_tmp_val(c->b->a);
}
static bool hook_is_required(void *vc, const char *x) {
    OptHookCtx *c = (OptHookCtx *)vc;
    Z80StrList one; vec_init(one);
    vec_push(one, arena_strdup(c->b->a, x));
    bool r = bb_is_used(c->b, &one, c->is_required_top, -1);
    vec_free(one);
    return r;
}

void bb_optimize(BasicBlock *b) {
    if (b->optimized) return;
    Arena *a = b->a;

    /* filtered_patterns_list (main.py:251):
     *   [p for p in engine.PATTERNS if OPTIONS.optimization_level >= p.level >= 3] */
    int olvl = b->optimizer->opt_level;
    VEC(int) plist; vec_init(plist);
    for (int pi=0; pi<peephole_pattern_count(); pi++) {
        int lvl = peephole_pattern_level(pi);
        if (olvl >= lvl && lvl >= 3) vec_push(plist, pi);
    }

    bool changed = true;
    Z80StrList code = bb_code(a, b);

    /* monkey-patch UNARY GVAL/FLAGVAL/IS_REQUIRED (basicblock.py:448-454) */
    OptHookCtx hc; hc.b = b; hc.is_required_top = 0;
    EvCpuHook hook;
    hook.ctx = &hc;
    hook.gval = hook_gval;
    hook.flagval = hook_flagval;
    hook.is_required = hook_is_required;
    ev_set_cpu_hook(&hook);

    /* if OPTIONS.optimization_level > 3: regs,mems = guesses... else {},{} */
    OMap regs, mems;
    if (olvl > 3) bb_guess_initial(b, &regs, &mems);
    else { omap_init(&regs); omap_init(&mems); }

    while (changed) {
        changed = false;
        cpustate_reset(b->cpu, &regs, &mems);

        for (int i=0; i<code.len; i++) {
            for (int q=0; q<plist.len; q++) {
                int pi = plist.data[q];
                const BlockPattern *patt = peephole_pattern_patt(pi);
                /* match = p.patt.match(code[i:]) */
                HashMap *match = block_pattern_match(
                    a, patt, (const char *const *)code.data + i,
                    code.len - i, 0);
                if (match == NULL) continue;

                /* for var,defline in p.defines: match[var]=defline.expr.eval(match) */
                bool unbound = false;
                int nd = peephole_pattern_ndefines(pi);
                for (int di=0; di<nd; di++) {
                    Ev *de = peephole_pattern_define_expr(pi, di);
                    EvVal *dv = ev_eval(a, de, match, &unbound);
                    if (unbound) break;
                    hashmap_set(match, peephole_pattern_define_var(pi,di),
                                evval_str(a, dv));
                }
                if (unbound) { hashmap_free(match); continue; }

                /* if not p.cond.eval(match): continue
                 * IS_REQUIRED closure uses i + len(p.patt). */
                hc.is_required_top = i + block_pattern_len(patt);
                unbound = false;
                EvVal *cv = ev_eval(a, peephole_pattern_cond(pi), match, &unbound);
                if (unbound || !evval_truthy(cv)) { hashmap_free(match); continue; }

                /* new_code = list(code)
                 * new_code[i:i+len(p.patt)] = p.template.filter(match) */
                int plen = block_pattern_len(patt);
                TplStrVec applied; vec_init(applied);
                bool tub=false;
                block_template_filter(a, peephole_pattern_templ(pi),
                                      match, &applied, &tub);
                if (tub) { vec_free(applied); hashmap_free(match); continue; }
                Z80StrList new_code; vec_init(new_code);
                for (int k=0;k<i;k++) vec_push(new_code, code.data[k]);
                for (int k=0;k<applied.len;k++) vec_push(new_code, applied.data[k]);
                for (int k=i+plen;k<code.len;k++) vec_push(new_code, code.data[k]);
                vec_free(applied);

                /* errmsg.info(...) / __DEBUG__ — debug_level default 0
                 * (no stderr, no asm effect); faithfully gated off. */

                /* changed = new_code != code */
                bool diff = (new_code.len != code.len);
                if (!diff) for (int k=0;k<code.len;k++)
                    if (strcmp(new_code.data[k], code.data[k])!=0){diff=true;break;}
                changed = diff;
                if (changed) {
                    vec_free(code);
                    code = new_code;        /* code = new_code */
                    /* self.code = new_code -> _set_code (rebuild mem) */
                    bb_set_code(b, (const char *const *)code.data, code.len);
                    hashmap_free(match);
                    break;
                }
                vec_free(new_code);
                hashmap_free(match);
            }
            if (changed) { b->modified = true; break; }
            cpustate_execute(b->cpu, code.data[i]);
        }
    }

    ev_set_cpu_hook(NULL);     /* restore old copy (basicblock.py:495) */
    omap_free(&regs); omap_free(&mems);
    vec_free(plist);
    vec_free(code);
    b->optimized = true;
}
