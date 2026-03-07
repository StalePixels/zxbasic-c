/*
 * Assembly instruction: opcode encoding and byte emission.
 * Mirrors src/zxbasm/asm_instruction.py and src/zxbasm/asm.py
 */
#include "zxbasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Count 'N' argument slots in a mnemonic string.
 * E.g. "LD A,N" -> 1 arg of 1 byte
 *      "LD BC,NN" -> 1 arg of 2 bytes
 *      "NEXTREG N,N" -> 2 args of 1 byte each
 *      "LD (IX+N),N" -> 2 args of 1 byte each
 */
int count_arg_slots(const char *mnemonic, int *arg_bytes, int max_args)
{
    int count = 0;
    const char *p = mnemonic;

    while (*p) {
        if (*p == 'N') {
            int n = 0;
            while (*p == 'N') { n++; p++; }
            /* Check it's a word boundary: preceded by non-alpha, followed by non-alpha */
            if (count < max_args) {
                arg_bytes[count] = n;
                count++;
            }
        } else {
            p++;
        }
    }
    return count;
}

/* Convert integer to little-endian bytes */
static void int_to_le(int64_t val, int n_bytes, uint8_t *out)
{
    uint64_t v = (uint64_t)val;
    uint64_t mask = (n_bytes >= 8) ? ~0ULL : ((1ULL << (n_bytes * 8)) - 1);
    v &= mask;
    for (int i = 0; i < n_bytes; i++) {
        out[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

/* Compute bytes for an instruction */
int asm_instr_bytes(AsmState *as, AsmInstr *instr, uint8_t *out, int out_size)
{
    if (instr->type == ASM_DEFB) {
        /* DEFB: each expression -> 1 byte */
        int n = 0;
        if (instr->raw_bytes) {
            /* INCBIN data */
            if (instr->raw_count > out_size) return 0;
            memcpy(out, instr->raw_bytes, (size_t)instr->raw_count);
            return instr->raw_count;
        }
        for (int i = 0; i < instr->data_count; i++) {
            if (n >= out_size) break;
            if (instr->pending) {
                out[n++] = 0;
            } else {
                int64_t val = 0;
                expr_eval(as, instr->data_exprs[i], &val, false);
                if (val > 255 && !as->error_count) {
                    asm_warning(as, instr->lineno, "value will be truncated");
                }
                out[n++] = (uint8_t)(val & 0xFF);
            }
        }
        return n;
    }

    if (instr->type == ASM_DEFW) {
        /* DEFW: each expression -> 2 bytes (LE) */
        int n = 0;
        for (int i = 0; i < instr->data_count; i++) {
            if (n + 2 > out_size) break;
            if (instr->pending) {
                out[n++] = 0;
                out[n++] = 0;
            } else {
                int64_t val = 0;
                expr_eval(as, instr->data_exprs[i], &val, false);
                uint16_t w = (uint16_t)(val & 0xFFFF);
                out[n++] = (uint8_t)(w & 0xFF);
                out[n++] = (uint8_t)(w >> 8);
            }
        }
        return n;
    }

    if (instr->type == ASM_DEFS) {
        /* DEFS count, fill */
        int64_t count_val = 0;
        int64_t fill_val = 0;

        if (instr->defs_count) {
            if (!expr_eval(as, instr->defs_count, &count_val, instr->pending))
                count_val = 0;
        }
        if (instr->defs_fill) {
            if (!expr_eval(as, instr->defs_fill, &fill_val, instr->pending))
                fill_val = 0;
        }

        if (fill_val > 255 && !instr->pending) {
            asm_warning(as, instr->lineno, "value will be truncated");
        }

        int n = (int)count_val;
        if (n > out_size) n = out_size;
        if (n < 0) n = 0;
        uint8_t fill = (uint8_t)(fill_val & 0xFF);
        memset(out, fill, (size_t)n);
        return n;
    }

    /* Normal instruction */
    if (!instr->opcode) return 0;

    const char *opcode_str = instr->opcode->opcode;
    int size = instr->opcode->size;

    /* Resolve arguments if pending */
    int64_t arg_vals[ASM_MAX_ARGS] = {0};
    if (!instr->pending) {
        for (int i = 0; i < instr->arg_count; i++) {
            arg_vals[i] = instr->resolved_args[i];
        }
    } else {
        /* Try to resolve */
        for (int i = 0; i < instr->arg_count; i++) {
            if (instr->args[i]) {
                if (!expr_try_eval(as, instr->args[i], &arg_vals[i])) {
                    /* Still pending — emit zeros */
                    arg_vals[i] = 0;
                }
            }
        }
    }

    /* Parse opcode string and emit bytes */
    int n = 0;
    int argi = 0;
    const char *p = opcode_str;

    while (*p && n < out_size) {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        if (*p == 'X' && *(p+1) == 'X') {
            /* Argument placeholder */
            int arg_width = instr->arg_bytes[argi];
            int_to_le(arg_vals[argi], arg_width, &out[n]);
            n += arg_width;
            p += 2;
            /* Skip additional XX for multi-byte args (e.g. NN = XX XX = 2 bytes) */
            for (int skip = 1; skip < arg_width; skip++) {
                if (*p == ' ' && *(p+1) == 'X' && *(p+2) == 'X') {
                    p += 3;
                }
            }
            argi++;
        } else {
            /* Hex byte */
            char hex[3] = {p[0], p[1], '\0'};
            out[n++] = (uint8_t)strtol(hex, NULL, 16);
            p += 2;
        }
    }

    if (n != size && !as->error_count) {
        /* Internal error: size mismatch */
        /* This shouldn't happen if opcodes are correct */
    }

    return n;
}
