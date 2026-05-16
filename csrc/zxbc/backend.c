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

/* ---- _QUAD_TABLE dispatch (main.py:151-611) -------------------------- */
static StrVec quad_emit(Backend *b, Quad *q) {
    if (strcmp(q->instr, IC_END) == 0)    return emit_end(b, q);
    if (strcmp(q->instr, IC_INLINE) == 0) return emit_inline(b, q);
    /* Python KeyErrors here; for S5.2 only end/inline are produced. Later
     * S5.x add entries. Loud, not silent. */
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
