/*
 * Core assembler functions: init, destroy, error/warning, binary output.
 * Mirrors src/zxbasm/zxbasm.py and src/api/errmsg.py
 */
#include "zxbasm.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ----------------------------------------------------------------
 * Init / Destroy
 * ---------------------------------------------------------------- */
void asm_init(AsmState *as)
{
    memset(as, 0, sizeof(*as));
    arena_init(&as->arena, 64 * 1024);
    mem_init(&as->mem, &as->arena);
    as->err_file = stderr;
    as->max_errors = 20;
    hashmap_init(&as->error_cache);
    vec_init(as->inits);
    as->output_format = "bin";
}

void asm_destroy(AsmState *as)
{
    hashmap_free(&as->error_cache);
    /* Scope hashmaps */
    for (int i = 0; i < as->mem.scope_count; i++) {
        hashmap_free(&as->mem.label_scopes[i]);
    }
    hashmap_free(&as->mem.tmp_labels);
    hashmap_free(&as->mem.tmp_label_lines);
    hashmap_free(&as->mem.tmp_pending);
    vec_free(as->mem.scope_lines);
    for (int i = 0; i < as->mem.org_blocks.len; i++) {
        vec_free(as->mem.org_blocks.data[i].instrs);
    }
    vec_free(as->mem.org_blocks);
    vec_free(as->mem.namespace_stack);
    vec_free(as->inits);
    arena_destroy(&as->arena);
}

/* ----------------------------------------------------------------
 * Error / Warning reporting
 * Python format: "filename:lineno: error: message"
 * ---------------------------------------------------------------- */
void asm_error(AsmState *as, int lineno, const char *fmt, ...)
{
    if (as->error_count > as->max_errors) {
        /* Too many errors — bail out */
        return;
    }

    const char *fname = as->current_file ? as->current_file : "(stdin)";

    /* Format the message */
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Build full error string: "filename:lineno: error: message" */
    char full[2200];
    snprintf(full, sizeof(full), "%s:%i: error: %s", fname, lineno, msg);

    /* Dedup via error cache */
    if (hashmap_has(&as->error_cache, full)) return;
    hashmap_set(&as->error_cache, full, (void *)1);

    fprintf(as->err_file, "%s\n", full);
    as->error_count++;
}

void asm_warning(AsmState *as, int lineno, const char *fmt, ...)
{
    as->warning_count++;

    const char *fname = as->current_file ? as->current_file : "(stdin)";

    /* Format the message */
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Build full warning string: "filename:lineno: warning: message" */
    char full[2200];
    snprintf(full, sizeof(full), "%s:%i: warning: %s", fname, lineno, msg);

    /* Dedup */
    if (hashmap_has(&as->error_cache, full)) return;
    hashmap_set(&as->error_cache, full, (void *)1);

    fprintf(as->err_file, "%s\n", full);
}

/* ----------------------------------------------------------------
 * Assemble (calls parser)
 * ---------------------------------------------------------------- */

/* Declared in parser.c */
extern int parser_parse(AsmState *as, const char *input);

int asm_assemble(AsmState *as, const char *input)
{
    parser_parse(as, input);

    /* Check for unclosed scopes (missing ENDP) */
    if (as->mem.scope_count > 1) {
        int proc_line = as->mem.scope_lines.len > 0
            ? as->mem.scope_lines.data[as->mem.scope_lines.len - 1] : 0;
        asm_error(as, proc_line, "Missing ENDP to close this scope");
    }

    if (as->error_count > 0) return as->error_count;

    /* Emit #init code (mirrors Python zxbasm.py lines 167-181) */
    if (as->inits.len > 0) {
        /* Set org past current end of code */
        int max_addr = -1;
        for (int i = 0; i < MAX_MEM; i++) {
            if (as->mem.byte_set[i]) max_addr = i;
        }
        int init_org = max_addr + 1;
        as->mem.index = init_org;
        as->mem.org_value = init_org;

        for (int i = 0; i < as->inits.len; i++) {
            const char *label = as->inits.data[i].label;
            int line = as->inits.data[i].lineno;

            /* Look up the label */
            Label *lbl = mem_get_label(as, label, line);

            /* Create CALL NN instruction */
            AsmInstr *instr = arena_calloc(&as->arena, 1, sizeof(AsmInstr));
            instr->lineno = 0;
            instr->type = ASM_NORMAL;
            const Z80Opcode *op = z80_find_opcode("CALL NN");
            instr->opcode = op;
            instr->asm_name = op->asm_name;
            instr->arg_count = count_arg_slots("CALL NN", instr->arg_bytes, ASM_MAX_ARGS);

            Expr *arg = expr_label(as, lbl, line);
            instr->args[0] = arg;
            int64_t val;
            if (expr_try_eval(as, arg, &val)) {
                instr->resolved_args[0] = val;
                instr->pending = false;
            } else {
                instr->pending = true;
            }
            mem_add_instruction(as, instr);
        }

        /* Add JP NN to autorun or min_org */
        AsmInstr *jp_instr = arena_calloc(&as->arena, 1, sizeof(AsmInstr));
        jp_instr->lineno = 0;
        jp_instr->type = ASM_NORMAL;
        const Z80Opcode *jp_op = z80_find_opcode("JP NN");
        jp_instr->opcode = jp_op;
        jp_instr->asm_name = jp_op->asm_name;
        jp_instr->arg_count = count_arg_slots("JP NN", jp_instr->arg_bytes, ASM_MAX_ARGS);

        int64_t jp_target;
        if (as->has_autorun) {
            jp_target = as->autorun_addr;
        } else {
            /* Find min org */
            jp_target = 0;
            for (int i = 0; i < MAX_MEM; i++) {
                if (as->mem.byte_set[i]) { jp_target = i; break; }
            }
        }
        jp_instr->resolved_args[0] = jp_target;
        jp_instr->pending = false;
        /* No expr needed since we have the resolved value */
        mem_add_instruction(as, jp_instr);

        /* Set autorun to the init block */
        as->has_autorun = true;
        as->autorun_addr = init_org;
    }

    return as->error_count;
}

/* ----------------------------------------------------------------
 * Binary output
 * Mirrors src/outfmt/binary.py — just write raw bytes
 * ---------------------------------------------------------------- */
int asm_generate_binary(AsmState *as, const char *filename, const char *format)
{
    int org;
    uint8_t *data;
    int data_len;

    if (mem_dump(as, &org, &data, &data_len) != 0) {
        return -1;
    }

    if (!data || data_len == 0) {
        /* Create empty output file (matches Python behavior) */
        FILE *f = fopen(filename, "wb");
        if (f) fclose(f);
        return 0;
    }

    /* For now, only "bin" format is supported */
    (void)format;

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Cannot open output file: %s\n", filename);
        return -1;
    }

    fwrite(data, 1, (size_t)data_len, f);
    fclose(f);
    return 0;
}
