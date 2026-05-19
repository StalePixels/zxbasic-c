/*
 * Memory model for the Z80 assembler.
 * Mirrors src/zxbasm/memory.py
 */
#include "zxbasm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ----------------------------------------------------------------
 * Namespace helpers
 * ---------------------------------------------------------------- */
#define DOT '.'
#define DOT_STR "."

char *normalize_namespace(AsmState *as, const char *ns)
{
    if (!ns || !*ns) return arena_strdup(&as->arena, ".");

    StrBuf sb;
    strbuf_init(&sb);
    strbuf_append_char(&sb, DOT);

    const char *p = ns;
    while (*p) {
        /* skip dots */
        while (*p == DOT) p++;
        if (!*p) break;
        /* copy segment */
        const char *start = p;
        while (*p && *p != DOT) p++;
        if (sb.len > 1) strbuf_append_char(&sb, DOT);
        strbuf_append_n(&sb, start, (size_t)(p - start));
    }

    if (sb.len == 0) strbuf_append_char(&sb, DOT);

    char *result = arena_strdup(&as->arena, strbuf_cstr(&sb));
    strbuf_free(&sb);
    return result;
}

/* Check if a string is all decimal digits */
static bool is_decimal(const char *s)
{
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }
    return true;
}

/* Check if label is a temporary label reference like "1F" or "2B" */
static bool is_temp_label_ref(const char *s)
{
    if (!s || !*s) return false;
    const char *p = s;
    while (*p && isdigit((unsigned char)*p)) p++;
    if (p == s) return false;
    return (*p == 'B' || *p == 'F') && *(p + 1) == '\0';
}

/* ----------------------------------------------------------------
 * Memory initialization
 * ---------------------------------------------------------------- */
void mem_init(Memory *m, Arena *arena)
{
    memset(m, 0, sizeof(*m));
    m->index = 0;
    m->org_value = 0;

    /* Initialize label scopes: start with one global scope */
    m->scope_count = 1;
    m->scope_cap = 4;
    m->label_scopes = arena_alloc(arena, sizeof(HashMap) * (size_t)m->scope_cap);
    hashmap_init(&m->label_scopes[0]);

    vec_init(m->scope_lines);
    vec_init(m->org_blocks);

    hashmap_init(&m->tmp_labels);
    hashmap_init(&m->tmp_label_lines);
    hashmap_init(&m->tmp_pending);

    /* instr_at is zeroed by memset above */

    m->namespace_ = arena_strdup(arena, ".");
    vec_init(m->namespace_stack);
}

/* ----------------------------------------------------------------
 * ORG management
 * ---------------------------------------------------------------- */
void mem_set_org(AsmState *as, int value, int lineno)
{
    if (value < 0 || value > 65535) {
        asm_error(as, lineno,
                  "Memory ORG out of range [0 .. 65535]. Current value: %i",
                  value);
        return;
    }
    /* Clear temporary labels on ORG change (matches Python) */
    /* TODO: implement tmp label clearing if needed */
    as->mem.index = value;
    as->mem.org_value = value;
}

/* ----------------------------------------------------------------
 * Label name mangling (id_name in Python)
 * ---------------------------------------------------------------- */
static void id_name(AsmState *as, const char *label, const char *namespace_,
                    char **out_name, char **out_ns)
{
    Memory *m = &as->mem;

    if (!namespace_)
        namespace_ = m->namespace_;

    *out_ns = arena_strdup(&as->arena, namespace_);

    /* Temporary labels: just integer numbers or nF/nB */
    if (is_decimal(label) || is_temp_label_ref(label)) {
        *out_name = arena_strdup(&as->arena, label);
        return;
    }

    /* If label starts with '.', use it as-is */
    if (label[0] == DOT) {
        *out_name = arena_strdup(&as->arena, label);
        return;
    }

    /* Mangle: namespace.label */
    StrBuf sb;
    strbuf_init(&sb);
    strbuf_append(&sb, namespace_);
    strbuf_append_char(&sb, DOT);
    strbuf_append(&sb, label);

    char *mangled = arena_strdup(&as->arena, strbuf_cstr(&sb));
    strbuf_free(&sb);

    /* Normalize */
    *out_name = normalize_namespace(as, mangled);
}

/* ----------------------------------------------------------------
 * Label declaration
 * ---------------------------------------------------------------- */
void mem_declare_label(AsmState *as, const char *label, int lineno,
                       Expr *value_expr, bool local)
{
    Memory *m = &as->mem;
    char *ex_label, *ns;
    id_name(as, label, NULL, &ex_label, &ns);

    bool is_address = (value_expr == NULL);
    int64_t value = 0;

    if (value_expr == NULL) {
        value = m->index;
    } else {
        if (!expr_try_eval(as, value_expr, &value)) {
            /* Can't resolve now — defer to second pass. */
            value = 0;
        }
    }

    /* Temporary labels */
    if (is_decimal(label)) {
        /* Store temporary label with filename:lineno key */
        Label *lbl = arena_alloc(&as->arena, sizeof(Label));
        lbl->name = ex_label;
        lbl->lineno = lineno;
        lbl->value = value;
        lbl->defined = true;
        lbl->local = false;
        lbl->is_address = true;
        lbl->namespace_ = ns;
        lbl->current_ns = arena_strdup(&as->arena, m->namespace_);
        lbl->is_temporary = true;
        lbl->direction = 0;

        /* Store keyed by file:line:name */
        char key[512];
        snprintf(key, sizeof(key), "%s:%d:%s",
                 as->current_file ? as->current_file : "(stdin)",
                 lineno, ex_label);
        hashmap_set(&m->tmp_labels, key, lbl);

        /* Track line numbers per file for bisect */
        const char *fname = as->current_file ? as->current_file : "(stdin)";
        /* Store line list - simple approach with vec */
        typedef VEC(int) IntVec;
        IntVec *lines = hashmap_get(&m->tmp_label_lines, fname);
        if (!lines) {
            lines = arena_alloc(&as->arena, sizeof(IntVec));
            vec_init(*lines);
            hashmap_set(&m->tmp_label_lines, fname, lines);
        }
        /* Append if not duplicate */
        if (lines->len == 0 || lines->data[lines->len - 1] != lineno) {
            vec_push(*lines, lineno);
        }
        return;
    }

    /* Normal labels */
    HashMap *scope = &m->label_scopes[m->scope_count - 1];
    Label *existing = hashmap_get(scope, ex_label);

    if (existing) {
        if (existing->defined) {
            asm_error(as, lineno, "label '%s' already defined at line %i",
                      existing->name, existing->lineno);
            return;
        }
        /* Define previously forward-referenced label */
        existing->value = value;
        existing->defined = true;
        existing->lineno = lineno;
        existing->is_address = is_address;
        existing->namespace_ = ns;
    } else {
        Label *lbl = arena_alloc(&as->arena, sizeof(Label));
        lbl->name = ex_label;
        lbl->lineno = lineno;
        lbl->value = value;
        lbl->defined = true;
        lbl->local = local;
        lbl->is_address = is_address;
        lbl->namespace_ = ns;
        lbl->current_ns = arena_strdup(&as->arena, m->namespace_);
        lbl->is_temporary = false;
        lbl->direction = 0;
        hashmap_set(scope, ex_label, lbl);
    }

    /* Note: We do NOT set byte_set here for label-only addresses.
     * In Python, set_memory_slot() does set memory_bytes[org] = 0,
     * but dump() uses an align buffer that drops trailing label-only
     * bytes. By not setting byte_set, our simpler dump logic achieves
     * the same effect — trailing label addresses don't extend output. */
}

/* ----------------------------------------------------------------
 * Label lookup
 * ---------------------------------------------------------------- */
Label *mem_get_label(AsmState *as, const char *label, int lineno)
{
    Memory *m = &as->mem;
    char *ex_label, *ns;
    id_name(as, label, NULL, &ex_label, &ns);

    /* Temporary label? */
    if (is_temp_label_ref(label)) {
        Label *lbl = arena_alloc(&as->arena, sizeof(Label));
        lbl->name = arena_strdup(&as->arena, label);  /* keep B/F suffix in internal name */
        lbl->lineno = lineno;
        lbl->value = 0;
        lbl->defined = false;
        lbl->local = false;
        lbl->is_address = false;
        lbl->namespace_ = ns;
        lbl->current_ns = arena_strdup(&as->arena, m->namespace_);
        lbl->is_temporary = true;

        /* Parse direction from last char */
        size_t len = strlen(label);
        char dir = label[len - 1];
        lbl->direction = (dir == 'B') ? -1 : (dir == 'F') ? 1 : 0;

        /* Register as pending for later resolution */
        const char *fname = as->current_file ? as->current_file : "(stdin)";
        typedef VEC(Label *) LabelVec;
        LabelVec *pending = hashmap_get(&m->tmp_pending, fname);
        if (!pending) {
            pending = arena_alloc(&as->arena, sizeof(LabelVec));
            vec_init(*pending);
            hashmap_set(&m->tmp_pending, fname, pending);
        }
        vec_push(*pending, lbl);
        return lbl;
    }

    /* Search scopes from innermost to outermost */
    for (int i = m->scope_count - 1; i >= 0; i--) {
        Label *lbl = hashmap_get(&m->label_scopes[i], ex_label);
        if (lbl) return lbl;
    }

    /* Not found — create undefined label in current scope */
    Label *lbl = arena_alloc(&as->arena, sizeof(Label));
    lbl->name = ex_label;
    lbl->lineno = lineno;
    lbl->value = 0;
    lbl->defined = false;
    lbl->local = false;
    lbl->is_address = false;
    lbl->namespace_ = ns;
    lbl->current_ns = arena_strdup(&as->arena, m->namespace_);
    lbl->is_temporary = false;
    lbl->direction = 0;
    hashmap_set(&m->label_scopes[m->scope_count - 1], ex_label, lbl);
    return lbl;
}

/* ----------------------------------------------------------------
 * LOCAL label setting
 * ---------------------------------------------------------------- */
void mem_set_label(AsmState *as, const char *label, int lineno, bool local)
{
    Memory *m = &as->mem;
    char *ex_label, *ns;
    id_name(as, label, NULL, &ex_label, &ns);

    HashMap *scope = &m->label_scopes[m->scope_count - 1];
    Label *existing = hashmap_get(scope, ex_label);

    if (existing) {
        if (existing->local == local) {
            asm_warning(as, lineno, "label '%s' already declared as LOCAL", label);
        }
        existing->local = local;
        existing->lineno = lineno;
    } else {
        Label *lbl = arena_alloc(&as->arena, sizeof(Label));
        lbl->name = ex_label;
        lbl->lineno = lineno;
        lbl->value = 0;
        lbl->defined = false;
        lbl->local = local;
        lbl->is_address = false;
        lbl->namespace_ = arena_strdup(&as->arena, m->namespace_);
        lbl->current_ns = arena_strdup(&as->arena, m->namespace_);
        lbl->is_temporary = false;
        lbl->direction = 0;
        hashmap_set(scope, ex_label, lbl);
    }
}

/* ----------------------------------------------------------------
 * PROC/ENDP scope management
 * ---------------------------------------------------------------- */
void mem_enter_proc(AsmState *as, int lineno)
{
    Memory *m = &as->mem;

    /* Grow scope array if needed */
    if (m->scope_count >= m->scope_cap) {
        int new_cap = m->scope_cap * 2;
        HashMap *new_scopes = arena_alloc(&as->arena, sizeof(HashMap) * (size_t)new_cap);
        memcpy(new_scopes, m->label_scopes, sizeof(HashMap) * (size_t)m->scope_count);
        m->label_scopes = new_scopes;
        m->scope_cap = new_cap;
    }

    hashmap_init(&m->label_scopes[m->scope_count]);
    m->scope_count++;
    vec_push(m->scope_lines, lineno);
}

void mem_exit_proc(AsmState *as, int lineno)
{
    Memory *m = &as->mem;

    if (m->scope_count <= 1) {
        asm_error(as, lineno, "ENDP in global scope (with no PROC)");
        return;
    }

    /* Transfer non-local labels to global scope */
    HashMap *local_scope = &m->label_scopes[m->scope_count - 1];
    HashMap *global_scope = &m->label_scopes[0];

    /* Iterate local scope and transfer non-local labels */
    for (int i = 0; i < local_scope->capacity; i++) {
        HashEntry *entry = &local_scope->entries[i];
        if (!entry->occupied || !entry->key) continue;

        Label *lbl = (Label *)entry->value;
        if (lbl->local) {
            if (!lbl->defined) {
                asm_error(as, lineno, "Undefined LOCAL label '%s'", lbl->name);
                return;
            }
            continue;
        }

        /* Transfer to global */
        Label *existing = hashmap_get(global_scope, lbl->name);
        if (!existing) {
            hashmap_set(global_scope, lbl->name, lbl);
        } else {
            if (!existing->defined && lbl->defined) {
                existing->value = lbl->value;
                existing->defined = true;
                existing->lineno = lbl->lineno;
            } else if (lbl->defined) {
                existing->value = lbl->value;
                existing->defined = true;
                existing->lineno = lbl->lineno;
            }
        }
    }

    hashmap_free(local_scope);
    m->scope_count--;
    vec_pop(m->scope_lines);
}

/* ----------------------------------------------------------------
 * Instruction addition
 * ---------------------------------------------------------------- */
void mem_add_instruction(AsmState *as, AsmInstr *instr)
{
    Memory *m = &as->mem;

    if (as->error_count > 0) return;

    /* Ensure memory slot exists at current org */
    if (!m->byte_set[m->index]) {
        m->bytes[m->index] = 0;
        m->byte_set[m->index] = true;
    }

    /* Record instruction start address */
    instr->start_addr = m->index;

    /* Store instruction at its start address for second-pass resolution */
    if (m->index < MAX_MEM) {
        m->instr_at[m->index] = instr;
    }

    /* Find or create org block */
    OrgBlock *blk = NULL;
    for (int i = 0; i < m->org_blocks.len; i++) {
        if (m->org_blocks.data[i].org == m->org_value) {
            blk = &m->org_blocks.data[i];
            break;
        }
    }
    if (!blk) {
        OrgBlock new_blk;
        new_blk.org = m->org_value;
        vec_init(new_blk.instrs);
        vec_push(m->org_blocks, new_blk);
        blk = &m->org_blocks.data[m->org_blocks.len - 1];
    }
    vec_push(blk->instrs, instr);

    /* Emit bytes.
     *
     * DEFS is special-cased: its byte count is unbounded (DEFS 4768 etc.)
     * so it must NOT be funnelled through the fixed 256-byte stack buffer.
     * Mirrors Python Memory.add_instruction() (src/zxbasm/memory.py:160-161)
     * which does `for byte in instr.bytes(): self.__set_byte(...)`, i.e. the
     * location counter advances by EXACTLY len(instr.bytes()). For a pending
     * DEFS, asm.py:80 returns `(0,) * N` (N zero bytes); for a resolved one,
     * asm.py:91 returns `(num,) * arg0` (N copies of fill & 0xFF) — N in
     * both passes, no ceiling. We write the fill byte straight into the
     * memory image with the same MAX_MEM overflow guard as the copy loop. */
    if (instr->type == ASM_DEFS) {
        uint8_t fill = 0;
        int n = asm_defs_resolve(as, instr, &fill);
        for (int i = 0; i < n; i++) {
            if (m->index + i >= MAX_MEM) {
                asm_error(as, instr->lineno, "Memory overflow at address %d", m->index + i);
                return;
            }
            m->bytes[m->index + i] = fill;
            m->byte_set[m->index + i] = true;
        }
        m->index += n;
        return;
    }

    uint8_t buf[256];
    int n = asm_instr_bytes(as, instr, buf, sizeof(buf));

    for (int i = 0; i < n; i++) {
        if (m->index + i >= MAX_MEM) {
            asm_error(as, instr->lineno, "Memory overflow at address %d", m->index + i);
            return;
        }
        m->bytes[m->index + i] = buf[i];
        m->byte_set[m->index + i] = true;
    }
    m->index += n;
}

/* ----------------------------------------------------------------
 * Resolve temporary labels (for dump)
 * ---------------------------------------------------------------- */
static void resolve_temp_label(AsmState *as, const char *fname, Label *lbl)
{
    Memory *m = &as->mem;
    typedef VEC(int) IntVec;
    IntVec *lines = hashmap_get(&m->tmp_label_lines, fname);
    if (!lines || lines->len == 0) return;

    /* Get the base name (strip B/F) */
    char base_name[64];
    size_t len = strlen(lbl->name);
    if (len > 0 && (lbl->name[len-1] == 'B' || lbl->name[len-1] == 'F')) {
        snprintf(base_name, sizeof(base_name), "%.*s", (int)(len - 1), lbl->name);
    } else {
        snprintf(base_name, sizeof(base_name), "%s", lbl->name);
    }

    if (lbl->direction == -1) {
        /* Search backward from lbl->lineno */
        for (int i = lines->len - 1; i >= 0; i--) {
            int line = lines->data[i];
            if (line > lbl->lineno) continue;
            char key[512];
            snprintf(key, sizeof(key), "%s:%d:%s", fname, line, base_name);
            Label *def = hashmap_get(&m->tmp_labels, key);
            if (def && def->defined) {
                /* Python Label.__eq__ compares name AND namespace */
                if (def->namespace_ && lbl->namespace_ &&
                    strcmp(def->namespace_, lbl->namespace_) != 0)
                    continue;
                lbl->value = def->value;
                lbl->defined = true;
                return;
            }
        }
    } else if (lbl->direction == 1) {
        /* Search forward from lbl->lineno */
        for (int i = 0; i < lines->len; i++) {
            int line = lines->data[i];
            if (line <= lbl->lineno) continue;
            char key[512];
            snprintf(key, sizeof(key), "%s:%d:%s", fname, line, base_name);
            Label *def = hashmap_get(&m->tmp_labels, key);
            if (def && def->defined) {
                /* Python Label.__eq__ compares name AND namespace */
                if (def->namespace_ && lbl->namespace_ &&
                    strcmp(def->namespace_, lbl->namespace_) != 0)
                    continue;
                lbl->value = def->value;
                lbl->defined = true;
                return;
            }
        }
    }
}

/* ----------------------------------------------------------------
 * Memory map (mirrors src/zxbasm/memory.py:294-299
 *
 *   return "\n".join(sorted(
 *       "%04X: %s" % (x.value, x.name)
 *       for x in self.global_labels.values() if x.is_address))
 *
 * Builds the joined "hex address: label" listing for the global
 * scope. Address labels only (EQU constants have is_address False).
 * Formatted lines are sorted lexicographically (Python sorted() on a
 * list of ASCII str, == strcmp here) and joined with '\n' with NO
 * trailing newline.  Result is arena-allocated.
 * ---------------------------------------------------------------- */
typedef struct {
    Arena *arena;
    char **lines;
    int count;
    int cap;
} MemMapCollector;

static bool mem_memory_map_collect(const char *key, void *value, void *userdata)
{
    (void)key;
    MemMapCollector *c = (MemMapCollector *)userdata;
    Label *lbl = (Label *)value;

    /* Python: if x.is_address */
    if (!lbl->is_address) return true;

    /* Python: "%04X: %s" % (x.value, x.name)
     * x.name for these (non-temporary, declared) labels == x._name,
     * which maps to the C Label.name field. Python "%X" on a Python
     * int widens naturally for values > 0xFFFF; %04llX matches that
     * for non-negative addresses without masking or truncation. */
    const char *name = lbl->name ? lbl->name : "";
    int needed = snprintf(NULL, 0, "%04llX: %s",
                          (long long)lbl->value, name);
    char *line = arena_alloc(c->arena, (size_t)needed + 1);
    snprintf(line, (size_t)needed + 1, "%04llX: %s",
             (long long)lbl->value, name);

    if (c->count >= c->cap) {
        int new_cap = c->cap ? c->cap * 2 : 8;
        char **grown = arena_alloc(c->arena, sizeof(char *) * (size_t)new_cap);
        if (c->count > 0)
            memcpy(grown, c->lines, sizeof(char *) * (size_t)c->count);
        c->lines = grown;
        c->cap = new_cap;
    }
    c->lines[c->count++] = line;
    return true;
}

static int mem_memory_map_cmp(const void *a, const void *b)
{
    /* Python sorted() on ASCII strings == strcmp ordering */
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char *mem_memory_map(Memory *m, Arena *arena)
{
    MemMapCollector c = { arena, NULL, 0, 0 };

    /* Python: self.global_labels == self.local_labels[0] */
    hashmap_foreach(&m->label_scopes[0], mem_memory_map_collect, &c);

    qsort(c.lines, (size_t)c.count, sizeof(char *), mem_memory_map_cmp);

    /* Python: "\n".join(...) — separator only, no trailing newline,
     * empty string when there are zero qualifying labels. */
    StrBuf sb;
    strbuf_init(&sb);
    for (int i = 0; i < c.count; i++) {
        if (i > 0) strbuf_append_char(&sb, '\n');
        strbuf_append(&sb, c.lines[i]);
    }

    char *result = arena_strdup(arena, strbuf_cstr(&sb));
    strbuf_free(&sb);
    return result;
}

/* ----------------------------------------------------------------
 * Memory dump — resolve all pending labels and emit binary
 * ---------------------------------------------------------------- */
int mem_dump(AsmState *as, int *org_out, uint8_t **data_out, int *data_len)
{
    Memory *m = &as->mem;

    /* Find the range of used memory */
    int min_addr = -1, max_addr = -1;
    for (int i = 0; i < MAX_MEM; i++) {
        if (m->byte_set[i]) {
            if (min_addr < 0) min_addr = i;
            max_addr = i;
        }
    }

    /* Resolve temporary labels */
    for (int i = 0; i < m->tmp_pending.capacity; i++) {
        HashEntry *entry = &m->tmp_pending.entries[i];
        if (!entry->occupied || !entry->key) continue;
        const char *fname = entry->key;
        typedef VEC(Label *) LabelVec;
        LabelVec *pending = (LabelVec *)entry->value;
        for (int j = 0; j < pending->len; j++) {
            resolve_temp_label(as, fname, pending->data[j]);
            if (!pending->data[j]->defined) {
                /* Python error() uses the BARE decimal label name
                 * (src/zxbasm/memory.py:173, label.name == '1'); C keeps
                 * the B/F direction suffix in the internal name
                 * (mem_get_label, memory.c:259). Strip it for the
                 * message only — don't perturb the stored name. */
                const char *nm = pending->data[j]->name;
                char bare[64];
                size_t nl = nm ? strlen(nm) : 0;
                if (nl > 1 && nl < sizeof(bare) &&
                    (nm[nl - 1] == 'B' || nm[nl - 1] == 'F')) {
                    memcpy(bare, nm, nl - 1);
                    bare[nl - 1] = '\0';
                    nm = bare;
                }
                asm_error(as, pending->data[j]->lineno,
                          "Undefined temporary label '%s'", nm);
            }
        }
    }

    /* Check all global labels are defined */
    HashMap *global = &m->label_scopes[0];
    for (int i = 0; i < global->capacity; i++) {
        HashEntry *entry = &global->entries[i];
        if (!entry->occupied || !entry->key) continue;
        Label *lbl = (Label *)entry->value;
        if (!lbl->defined) {
            asm_error(as, lbl->lineno, "Undefined GLOBAL label '%s'", lbl->name);
        }
    }

    /* Python Memory.dump() (src/zxbasm/memory.py:169-181) runs the
     * temporary- and GLOBAL-label sweeps BEFORE the byte loop and does
     * not short-circuit on empty output: an undefined label in a unit
     * that emits no bytes (e.g. `L EQU UNDEF + 1`) must still error.
     * Hence the sweeps above precede the empty-memory early return. */
    if (as->error_count > 0) {
        *org_out = (min_addr < 0) ? 0 : min_addr;
        *data_out = NULL;
        *data_len = 0;
        return -1;
    }

    if (min_addr < 0) {
        *org_out = 0;
        *data_out = NULL;
        *data_len = 0;
        return 0;
    }

    /* Second pass: re-resolve pending instructions and overwrite memory.
     * Mirrors Python Memory.dump() which iterates addresses and re-resolves.
     * Python: a.arg = a.argval(); a.pending = False; tmp = a.bytes() */
    for (int i = min_addr; i <= max_addr; i++) {
        if (as->error_count > 0) break;

        AsmInstr *instr = m->instr_at[i];
        if (!instr || !instr->pending) continue;

        /* Re-resolve args now that all labels are defined */
        for (int j = 0; j < instr->arg_count; j++) {
            if (instr->args[j]) {
                int64_t val;
                if (expr_try_eval(as, instr->args[j], &val))
                    instr->resolved_args[j] = val;
            }
        }
        instr->pending = false;

        /* DEFS special-case: unbounded byte count, so bypass the fixed
         * 256-byte stack buffer (see mem_add_instruction). Mirrors Python
         * Memory.dump() (src/zxbasm/memory.py:194-200): once resolved,
         * a.bytes() yields N copies of (fill & 0xFF) (asm.py:91) which are
         * written back over memory_bytes[i + r] for r in range(len(tmp)).
         * Same length N as the first (pending) pass, so the location
         * counter set up there is already correct. */
        if (instr->type == ASM_DEFS) {
            uint8_t fill = 0;
            int n = asm_defs_resolve(as, instr, &fill);
            for (int j = 0; j < n && (i + j) < MAX_MEM; j++) {
                m->bytes[i + j] = fill;
            }
            continue;
        }

        uint8_t buf[256];
        int n = asm_instr_bytes(as, instr, buf, sizeof(buf));

        /* Overwrite memory at the instruction's start address */
        for (int j = 0; j < n && (i + j) < MAX_MEM; j++) {
            m->bytes[i + j] = buf[j];
        }
    }

    if (as->error_count > 0) {
        *org_out = min_addr;
        *data_out = NULL;
        *data_len = 0;
        return -1;
    }

    /* Build output */
    int len = max_addr - min_addr + 1;
    uint8_t *output = arena_alloc(&as->arena, (size_t)len);
    memcpy(output, &m->bytes[min_addr], (size_t)len);

    *org_out = min_addr;
    *data_out = output;
    *data_len = len;
    return 0;
}
