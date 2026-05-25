/*
 * optmain.c — Port of src/arch/z80/optimizer/main.py (Optimizer):
 *   __init__ / init           (main.py:43-83)
 *   _cleanup_mem              (main.py:85-99)  [the O3 entry runs this;
 *                              the O<=2 path uses codegen.c's byte-proven
 *                              cleanup_mem inside `le2` — see below]
 *   cleanup_local_labels      (main.py:101-176)
 *   get_labels                (main.py:178-185)
 *   initialize_memory         (main.py:187-191)
 *   optimize                  (main.py:193-261)  *** the inertness gate ***
 *
 * INERTNESS GUARANTEE: optimize() reproduces main.py:193-201 EXACTLY —
 *   self.MEMORY.clear(); self.PROC_COUNTER = 0
 *   self._cleanup_mem(initial_memory)
 *   if OPTIONS.optimization_level <= 2:        # main.py:199
 *       return "\n".join(x for x in initial_memory if not RE_PRAGMA.match(x))
 * For O<=2 it delegates to `le2` — the pre-existing, byte-proven
 * optimizer_optimize_le2 (codegen.c) which itself does _cleanup_mem +
 * the RE_PRAGMA-filtered join. So the -O2 codegen meter is byte-identical
 * to HEAD: the O3 machinery below main.py:202 is unreachable at O<=2,
 * exactly as in Python (the verbatim early-return).
 */
#include "optimizer.h"
#include "opthelpers.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* helpers.ALL_REGS (helpers.py:56-74) */
static const char *ALL_REGS[] = {"a","b","c","d","e","f","h","l",
    "ixh","ixl","iyh","iyl","r","i","sp"};
#define END_PROGRAM_LABEL "__END_PROGRAM"

static Z80StrList sl_of(Arena *a, const char *const *items, int n) {
    Z80StrList v; vec_init(v);
    for (int i=0;i<n;i++) vec_push(v, arena_strdup(a, items[i]));
    return v;
}

/* Optimizer.init (main.py:46-83): helpers.init() + reset members +
 * LABELS seeded with the special/global blocks. */
static void optimizer_reinit(Optimizer *o) {
    z80h_helpers_init();                 /* helpers.init() */
    vec_free(o->JUMP_LABELS); vec_init(o->JUMP_LABELS);
    o->RAND_COUNT = 0;
    vec_free(o->MEMORY); vec_init(o->MEMORY);
    vec_free(o->BLOCKS); vec_init(o->BLOCKS);
    o->PROC_COUNTER = 0;

    ld_init(&o->LABELS, o->a);
    Arena *a = o->a;
    Z80StrList allr = sl_of(a, ALL_REGS, 15);
    #define DUMMY(dst,req) bb_new_dummy(a, o, &(dst), &(req))
    Z80StrList bc = sl_of(a,(const char*[]){"b","c"},2);
    Z80StrList aedbc = sl_of(a,(const char*[]){"a","e","d","b","c"},5);
    Z80StrList hlaedbc = sl_of(a,(const char*[]){"h","l","a","e","d","b","c"},7);
    Z80StrList la = sl_of(a,(const char*[]){"a"},1);
    Z80StrList lhl = sl_of(a,(const char*[]){"h","l"},2);
    Z80StrList labcde = sl_of(a,(const char*[]){"a","b","c","d","e"},5);
    Z80StrList empty; vec_init(empty);
    Z80StrList memcpy_d = sl_of(a,(const char*[]){"b","c","d","e","f","h","l"},7);
    Z80StrList memcpy_r = sl_of(a,(const char*[]){"b","c","d","e","h","l"},6);

    ld_set(&o->LABELS,"*START*", labelinfo_new(a,"*START*",0,DUMMY(allr,allr),0));
    ld_set(&o->LABELS,"*__END_PROGRAM*", labelinfo_new(a,"__END_PROGRAM",0,DUMMY(allr,bc),0));
    ld_set(&o->LABELS,"__ADDF", labelinfo_new(a,"__ADDF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__SUBF", labelinfo_new(a,"__SUBF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__DIVF", labelinfo_new(a,"__DIVF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__MULF", labelinfo_new(a,"__MULF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__GEF", labelinfo_new(a,"__GEF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__GTF", labelinfo_new(a,"__GTF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__EQF", labelinfo_new(a,"__EQF",0,DUMMY(allr,aedbc),0));
    ld_set(&o->LABELS,"__STOREF", labelinfo_new(a,"__STOREF",0,DUMMY(allr,hlaedbc),0));
    ld_set(&o->LABELS,"PRINT_AT", labelinfo_new(a,"PRINT_AT",0,DUMMY(allr,la),0));
    ld_set(&o->LABELS,"INK", labelinfo_new(a,"INK",0,DUMMY(allr,la),0));
    ld_set(&o->LABELS,"INK_TMP", labelinfo_new(a,"INK_TMP",0,DUMMY(allr,la),0));
    ld_set(&o->LABELS,"PAPER", labelinfo_new(a,"PAPER",0,DUMMY(allr,la),0));
    ld_set(&o->LABELS,"PAPER_TMP", labelinfo_new(a,"PAPER_TMP",0,DUMMY(allr,la),0));
    ld_set(&o->LABELS,"RND", labelinfo_new(a,"RND",0,DUMMY(allr,empty),0));
    ld_set(&o->LABELS,"INKEY", labelinfo_new(a,"INKEY",0,DUMMY(allr,empty),0));
    ld_set(&o->LABELS,"PLOT", labelinfo_new(a,"PLOT",0,DUMMY(allr,la),0));
    ld_set(&o->LABELS,"DRAW", labelinfo_new(a,"DRAW",0,DUMMY(allr,lhl),0));
    ld_set(&o->LABELS,"DRAW3", labelinfo_new(a,"DRAW3",0,DUMMY(allr,labcde),0));
    ld_set(&o->LABELS,"__ARRAY", labelinfo_new(a,"__ARRAY",0,DUMMY(allr,lhl),0));
    ld_set(&o->LABELS,"__MEMCPY", labelinfo_new(a,"__MEMCPY",0,DUMMY(memcpy_d,memcpy_r),0));
    ld_set(&o->LABELS,"__PLOADF", labelinfo_new(a,"__PLOADF",0,DUMMY(allr,allr),0));
    ld_set(&o->LABELS,"__PSTOREF", labelinfo_new(a,"__PSTOREF",0,DUMMY(allr,allr),0));
    #undef DUMMY
}

void optimizer_init(Optimizer *o, Arena *a) {
    o->a = a;
    o->unique_id = 0;
    o->clean_asm_args = false;
    o->opt_level = 2;
    o->debug_level = 0;   /* OPTIONS.debug_level default (debug.py gate) */
    vec_init(o->JUMP_LABELS);
    vec_init(o->MEMORY);
    vec_init(o->BLOCKS);
    optimizer_reinit(o);
}

/* RE_LABEL = ^[ \t]*[_a-zA-Z][a-zA-Z\d]*[ \t]*:  (patterns.py:27).
 * Returns match-end index past ':' (and group length incl ':') or -1.
 * Identical to codegen.c's re_label_end (kept TU-local; the O3
 * _cleanup_mem path needs it independently). */
static int re_label_end(const char *s, int *glen) {
    int i=0;
    while (s[i]==' '||s[i]=='\t') i++;
    char c=s[i];
    if (!(c=='_'||(c>='a'&&c<='z')||(c>='A'&&c<='Z'))) return -1;
    i++;
    while ((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
           (s[i]>='0'&&s[i]<='9')) i++;
    while (s[i]==' '||s[i]=='\t') i++;
    if (s[i]!=':') return -1;
    *glen=i+1; return i+1;
}
/* RE_PRAGMA = ^#[ \t]?pragma[ \t]opt[ \t]  (patterns.py:39) */
static bool re_pragma(const char *s) {
    const char *p=s;
    if (*p!='#') return false; p++;
    if (*p==' '||*p=='\t') p++;
    if (strncmp(p,"pragma",6)!=0) return false; p+=6;
    if (*p!=' '&&*p!='\t') return false; p++;
    if (strncmp(p,"opt",3)!=0) return false; p+=3;
    return (*p==' '||*p=='\t');
}
static char *rstrip_ws(Arena *a, const char *s) {
    size_t n=strlen(s);
    while (n>0){char c=s[n-1]; if(c=='\r'||c=='\n'||c=='\t'||c==' ')n--;else break;}
    return arena_strndup(a, s, n);
}

/* _cleanup_mem (main.py:85-99): split "LABEL: insn" into two elements.
 * Mutates the StrVec in place (Python edits the list in place). */
static void cleanup_mem(Arena *a, Z80StrList *mem) {
    int i=0;
    while (i < mem->len) {
        const char *tmp = mem->data[i];
        int glen, end = re_label_end(tmp, &glen);
        if (end >= 0) {
            char *rstr = rstrip_ws(a, tmp);
            char *grp  = arena_strndup(a, tmp, (size_t)glen);
            if (strcmp(rstr, grp) != 0) {
                /* initial_memory[i] = tmp[match.end():].strip() */
                const char *rem = tmp + end;
                while (*rem==' '||*rem=='\t'||*rem=='\r'||*rem=='\n') rem++;
                char *remc = rstrip_ws(a, rem);
                mem->data[i] = remc;
                /* insert(i, match.group()[:-1].strip()+":") */
                char *lab = arena_strndup(a, tmp, (size_t)glen-1);
                const char *ls=lab; size_t ll=strlen(lab);
                while (*ls==' '||*ls=='\t') { ls++; if(ll)ll--; }
                while (ll>0 && (ls[ll-1]==' '||ls[ll-1]=='\t')) ll--;
                char *labc=(char*)arena_alloc(a, ll+2);
                memcpy(labc,ls,ll); labc[ll]=':'; labc[ll+1]='\0';
                vec_push(*mem, NULL);
                for (int j=mem->len-1;j>i;j--) mem->data[j]=mem->data[j-1];
                mem->data[i]=labc;
                /* i not advanced past the inserted label (Python inserts
                 * before i then i+=1 -> lands on the original-now-shifted
                 * element). */
            }
        }
        i++;
    }
}

/* cleanup_local_labels (main.py:101-176) */
typedef VEC(char *) StrV;
typedef struct UsedMap { /* defaultdict(list): label -> [MemCell*] */
    char *key; CellVec cells;
} UsedMap;
typedef VEC(UsedMap) UsedVec;

static void cleanup_local_labels(Optimizer *o, BasicBlock *block) {
    Arena *a = o->a;
    /* stacks of scopes */
    typedef struct Scope { StrV names; OMap hashes; int prc; UsedVec used; } Scope;
    VEC(Scope) S; vec_init(S);
    { Scope s0; vec_init(s0.names); omap_init(&s0.hashes);
      s0.prc=o->PROC_COUNTER; vec_init(s0.used); vec_push(S, s0); }

    /* self.MEMORY[:] = block.mem[:] */
    vec_free(o->MEMORY); vec_init(o->MEMORY);
    for (int k=0;k<block->mem.len;k++) vec_push(o->MEMORY, block->mem.data[k]);

    for (int ci=0; ci<o->MEMORY.len; ci++) {
        OMemCell *cell = o->MEMORY.data[ci];
        /* inst.upper() == "PROC" / "ENDP" */
        char iu[8]; size_t il=strlen(cell->inst);
        if (il<sizeof(iu)) { for(size_t k=0;k<il;k++) iu[k]=(char)toupper((unsigned char)cell->inst[k]); iu[il]=0; }
        else iu[0]=0;

        if (strcmp(iu,"PROC")==0) {
            Scope s; vec_init(s.names); omap_init(&s.hashes);
            s.prc=o->PROC_COUNTER; vec_init(s.used);
            vec_push(S, s);
            o->PROC_COUNTER += 1;
            continue;
        }
        if (strcmp(iu,"ENDP")==0) {
            if (S.len > 1) {
                Scope *top = &S.data[S.len-1];
                Scope *prev = &S.data[S.len-2]; (void)prev;
                for (int u=0; u<top->used.len; u++) {
                    const char *label = top->used.data[u].key;
                    /* if label in stack[-1] */
                    bool in_names=false;
                    for (int z=0;z<top->names.len;z++)
                        if(!strcmp(top->names.data[z],label)){in_names=true;break;}
                    if (in_names) {
                        const char *newl = omap_get(&top->hashes, label);
                        for (int cc=0; cc<top->used.data[u].cells.len; cc++)
                            omemcell_replace_label(top->used.data[u].cells.data[cc],
                                                   label, newl);
                    }
                }
                S.len--;  /* stack/hashes/stackprc/used .pop() */
            }
            continue;
        }

        const char *tmp = cell->asm_->asm_;
        /* tmp.upper().startswith("LOCAL") */
        bool islocal = (strlen(tmp)>=5);
        if (islocal) for (int k=0;k<5;k++)
            if (toupper((unsigned char)tmp[k])!="LOCAL"[k]) { islocal=false; break; }
        if (islocal) {
            Scope *top=&S.data[S.len-1];
            /* tmp[5:].split(",") */
            const char *p=tmp+5;
            for (;;) {
                const char *comma=strchr(p,',');
                const char *e=comma?comma:p+strlen(p);
                /* lbl.strip() */
                const char *b=p; const char *ee=e;
                while (b<ee && (*b==' '||*b=='\t')) b++;
                while (ee>b && (ee[-1]==' '||ee[-1]=='\t')) ee--;
                char *lbl=arena_strndup(a,b,(size_t)(ee-b));
                bool present=false;
                for (int z=0;z<top->names.len;z++)
                    if(!strcmp(top->names.data[z],lbl)){present=true;break;}
                if (!present) {
                    vec_push(top->names, lbl);
                    char buf[256];
                    snprintf(buf,sizeof(buf),"PROC%d.%s",top->prc,lbl);
                    omap_set(a,&top->hashes,lbl,buf);
                }
                if (!comma) break;
                p=comma+1;
            }
            /* cell.asm = f";{cell.asm!s}"  (comment it out) */
            const char *old=cell->asm_->asm_;
            char *cm=(char*)arena_alloc(a,strlen(old)+2);
            cm[0]=';'; strcpy(cm+1,old);
            omemcell_set_asm(cell, cm);
            continue;
        }

        if (cell->is_label) {
            const char *label=cell->inst;
            for (int si=S.len-1; si>=0; si--) {
                bool in=false;
                for (int z=0;z<S.data[si].names.len;z++)
                    if(!strcmp(S.data[si].names.data[z],label)){in=true;break;}
                if (in) {
                    const char *nl=omap_get(&S.data[si].hashes,label);
                    size_t nn=strlen(nl);
                    char *lc=(char*)arena_alloc(a,nn+2);
                    memcpy(lc,nl,nn); lc[nn]=':'; lc[nn+1]=0;
                    omemcell_set_asm(cell, lc);
                    break;
                }
            }
            continue;
        }

        /* for label in cell.used_labels */
        Z80StrList ul = omemcell_used_labels(a, cell);
        for (int li=0; li<ul.len; li++) {
            const char *label=ul.data[li];
            bool used=false;
            for (int si=S.len-1; si>=0; si--) {
                bool in=false;
                for (int z=0;z<S.data[si].names.len;z++)
                    if(!strcmp(S.data[si].names.data[z],label)){in=true;break;}
                if (in) {
                    const char *nl=omap_get(&S.data[si].hashes,label);
                    omemcell_replace_label(cell, label, nl);
                    used=true;
                    break;
                }
            }
            if (!used) {
                Scope *top=&S.data[S.len-1];
                int slot=-1;
                for (int u=0;u<top->used.len;u++)
                    if(!strcmp(top->used.data[u].key,label)){slot=u;break;}
                if (slot<0) {
                    UsedMap um; um.key=arena_strdup(a,label);
                    vec_init(um.cells); vec_push(top->used, um);
                    slot=top->used.len-1;
                }
                vec_push(top->used.data[slot].cells, cell);
            }
        }
        vec_free(ul);
    }

    /* drop commented-out cells: for i in range(len-1,-1,-1):
     *   if MEMORY[i].asm.asm[0] == ";": pop */
    for (int i=o->MEMORY.len-1; i>=0; i--) {
        const char *ac=o->MEMORY.data[i]->asm_->asm_;
        if (ac[0]==';') {
            for (int j=i;j<o->MEMORY.len-1;j++) o->MEMORY.data[j]=o->MEMORY.data[j+1];
            o->MEMORY.len--;
        }
    }

    /* block.mem = self.MEMORY ; block.asm = [...] (asm list unused by
     * the ported O3 consumers — only block.mem drives everything). */
    vec_free(block->mem); vec_init(block->mem);
    for (int k=0;k<o->MEMORY.len;k++) vec_push(block->mem, o->MEMORY.data[k]);

    for (int z=0; z<S.len; z++) { vec_free(S.data[z].names);
        omap_free(&S.data[z].hashes); vec_free(S.data[z].used); }
    vec_free(S);
}

/* get_labels (main.py:178-185) */
static void get_labels(Optimizer *o, BasicBlock *bb) {
    for (int i=0;i<bb->mem.len;i++) {
        OMemCell *cell=bb->mem.data[i];
        if (cell->is_label)
            ld_set_force(&o->LABELS, cell->inst,
                         labelinfo_new(o->a, cell->inst, cell->addr, bb, i));
    }
}

/* initialize_memory (main.py:187-191) */
static void initialize_memory(Optimizer *o, BasicBlock *bb) {
    optimizer_reinit(o);
    vec_free(o->MEMORY); vec_init(o->MEMORY);
    for (int k=0;k<bb->mem.len;k++) vec_push(o->MEMORY, bb->mem.data[k]);
    get_labels(o, bb);
}

/* str.join + RE_PRAGMA filter (the O3 return, main.py:259-261) */
static char *join_filtered(Arena *a, const Z80StrList *v) {
    /* flatten not needed: b.code yields flat str list per block; the
     * caller already flattens. Here join non-pragma lines with "\n". */
    size_t total=1;
    int first=1;
    for (int i=0;i<v->len;i++) {
        if (re_pragma(v->data[i])) continue;
        total += strlen(v->data[i]) + (first?0:1);
        first=0;
    }
    char *out=(char*)arena_alloc(a,total);
    size_t w=0; first=1;
    for (int i=0;i<v->len;i++) {
        if (re_pragma(v->data[i])) continue;
        if (!first) out[w++]='\n';
        size_t l=strlen(v->data[i]);
        memcpy(out+w,v->data[i],l); w+=l;
        first=0;
    }
    out[w]='\0';
    return out;
}

/* Optimizer.optimize (main.py:193-261). */
char *optimizer_optimize(Optimizer *o, Arena *a, Z80StrList initial_memory,
                         int opt_level, OptLe2Fn le2) {
    o->opt_level = opt_level;
    /* self.MEMORY.clear(); self.PROC_COUNTER = 0 */
    vec_free(o->MEMORY); vec_init(o->MEMORY);
    o->PROC_COUNTER = 0;

    /* ---- THE INERTNESS GATE (main.py:198-200) --------------------- *
     * Python: self._cleanup_mem(initial_memory)
     *         if OPTIONS.optimization_level <= 2: return join(...)
     * The byte-proven le2 (codegen.c) performs _cleanup_mem + the
     * RE_PRAGMA-filtered "\n".join over the SAME initial_memory and is
     * unchanged from HEAD, so the O<=2 output is byte-identical. */
    if (opt_level <= 2)
        return le2(a, initial_memory);

    /* ---- O>2 machinery (main.py:202-261) -------------------------- */
    /* _cleanup_mem(initial_memory) (the O3 path runs it here; identical
     * algorithm to the one folded into le2). */
    Z80StrList mem; vec_init(mem);
    for (int i=0;i<initial_memory.len;i++) vec_push(mem, initial_memory.data[i]);
    cleanup_mem(a, &mem);

    /* _BASICBLOCK_TYPE.clean_asm_args = OPTIONS.optimization_level > 3 */
    o->clean_asm_args = (opt_level > 3);

    /* bb = self._BASICBLOCK_TYPE(initial_memory, self) */
    BasicBlock *bb = bb_new(a, o, (const char *const *)mem.data, mem.len);

    cleanup_local_labels(o, bb);
    initialize_memory(o, bb);

    /* self.BLOCKS = basic_blocks = get_basic_blocks(bb) */
    BBVec basic_blocks = get_basic_blocks(bb);
    vec_free(o->BLOCKS); o->BLOCKS = basic_blocks;

    /* __DEBUG__ block (main.py:210-219). __DEBUG__(msg, 1) writes to
     * OPTIONS.stderr only when 1 <= OPTIONS.debug_level (debug.py:18-23);
     * the codegen meter runs with debug_level == 0 so this is a faithful
     * no-op (no stderr, no asm effect) there. Ported with the same gate;
     * the b.requires()/b.destroys() calls run only when actually
     * debugging — exactly Python's behaviour. */
    if (o->debug_level >= 1) {
        for (int i=0;i<basic_blocks.len;i++) {
            BasicBlock *b2 = basic_blocks.data[i];
            fprintf(stderr, "debug: --- BASIC BLOCK: %d ---\n", b2->id);
            Z80StrList rq, ds;
            bb_requires(b2, 0, -1, &rq);
            bb_destroys(b2, 0, &ds);
            fprintf(stderr, "debug: Requires: %d regs\n", rq.len);
            fprintf(stderr, "debug: Destroys: %d regs\n", ds.len);
            fprintf(stderr, "debug: Size/Time elided\n");
            fprintf(stderr, "debug: --- END ---\n");
            vec_free(rq); vec_free(ds);
        }
    }

    /* LABELS["*START*"].basic_block.add_goes_to(basic_blocks[0]) etc. */
    LabelInfo *start = ld_get(&o->LABELS, "*START*");
    bb_add_goes_to(start->basic_block, basic_blocks.data[0]);
    start->basic_block->next = basic_blocks.data[0];
    basic_blocks.data[0]->prev = start->basic_block;
    if (ld_has(&o->LABELS, END_PROGRAM_LABEL)) {
        LabelInfo *ep = ld_get(&o->LABELS, END_PROGRAM_LABEL);
        LabelInfo *epp = ld_get(&o->LABELS, "*__END_PROGRAM*");
        bb_add_goes_to(ep->basic_block, epp->basic_block);
    }

    /* jump-over-jump simplification (main.py:228-246) */
    for (int li=0; li<o->JUMP_LABELS.len; li++) {
        const char *label = o->JUMP_LABELS.data[li];
        LabelInfo *info = ld_get(&o->LABELS, label);
        BasicBlock *blk = info->basic_block;
        if (blk->is_dummy) continue;
        OMemCell *first = bb_get_next_exec_instruction(blk);
        if (first == NULL ||
            (strcmp(first->inst,"jp")!=0 && strcmp(first->inst,"jr")!=0))
            continue;
        /* for blk in list(self.LABELS[label].used_by) */
        BBVec snap; vec_init(snap);
        for (int k=0;k<info->used_by.len;k++) vec_push(snap, info->used_by.data[k]);
        for (int k=0;k<snap.len;k++) {
            BasicBlock *ub = snap.data[k];
            OMemCell *last = ub->mem.data[ub->mem.len-1];
            if (first->cond == NULL ||
                (last->cond && first->cond && strcmp(last->cond,first->cond)==0)) {
                const char *new_label = first->opers.len>0?first->opers.data[0]:"";
                /* blk[-1].asm = blk[-1].code.replace(label, new_label) */
                const char *code = omemcell_code(last);
                /* str.replace (ALL occurrences, not \b-bounded) */
                const char *q=code; int cnt=0; size_t ll=strlen(label);
                while (ll && (q=strstr(q,label))!=NULL){cnt++;q+=ll;}
                size_t nl=strlen(new_label), cl=strlen(code);
                char *nc=(char*)arena_alloc(a, cl + (nl>ll? (nl-ll)*cnt:0) + 1);
                size_t w=0; const char *r=code;
                while (*r) {
                    if (ll && strncmp(r,label,ll)==0) {
                        memcpy(nc+w,new_label,nl); w+=nl; r+=ll;
                    } else nc[w++]=*r++;
                }
                nc[w]=0;
                omemcell_set_asm(last, nc);
                bb_delete_comes_from(blk, ub);
                /* self.LABELS[label].used_by.remove(blk) */
                for (int z=0;z<info->used_by.len;z++)
                    if (info->used_by.data[z]==ub){
                        for(int y=z;y<info->used_by.len-1;y++)
                            info->used_by.data[y]=info->used_by.data[y+1];
                        info->used_by.len--; break; }
                LabelInfo *ni = ld_get(&o->LABELS, new_label);
                if (ni) {
                    bool has=false;
                    for (int z=0;z<ni->used_by.len;z++) if(ni->used_by.data[z]==ub){has=true;break;}
                    if(!has) vec_push(ni->used_by, ub);
                    bb_add_goes_to(ub, ni->basic_block);
                }
            }
        }
        vec_free(snap);
    }

    /* for x in basic_blocks: x.compute_cpu_state() */
    for (int i=0;i<basic_blocks.len;i++)
        bb_compute_cpu_state(basic_blocks.data[i]);

    /* for x in basic_blocks: x.optimize(filtered_patterns_list) */
    for (int i=0;i<basic_blocks.len;i++)
        bb_optimize(basic_blocks.data[i]);

    /* ignored marking (main.py:255-257) — DEAD CODE IN THE ORACLE.
     *
     *   for x in basic_blocks:
     *       if x.comes_from == [] and len([... JUMP_LABELS ... ]):
     *           x.ignored = True
     *
     * `x.comes_from` is a *set* (basicblock.py:60), and in Python a set
     * is NEVER == to a list, even when both are empty: `set() == []` is
     * always False. So this guard can never fire and `x.ignored` is
     * permanently False for every block — Python emits ALL blocks here.
     *
     * A literal C port that tested `x->comes_from.len == 0` would make
     * this dead branch LIVE (an empty C vector is "empty"), removing
     * blocks Python keeps (e.g. an externally-GOTO'd label inside a dead
     * WHILE 0 loop: whilefalse1). To stay byte-identical to the oracle we
     * must reproduce the set!=list mismatch: the condition is always
     * False, so nothing is ever marked ignored. (Faithful no-op.) */

    /* return "\n".join(flatten(x.code for x in basic_blocks if not x.ignored)
     *                   if not RE_PRAGMA.match) */
    Z80StrList flat; vec_init(flat);
    for (int i=0;i<basic_blocks.len;i++) {
        BasicBlock *x=basic_blocks.data[i];
        if (x->ignored) continue;
        Z80StrList c = bb_code(a, x);
        for (int k=0;k<c.len;k++) vec_push(flat, c.data[k]);
        vec_free(c);
    }
    char *res = join_filtered(a, &flat);
    vec_free(flat); vec_free(mem);
    return res;
}
