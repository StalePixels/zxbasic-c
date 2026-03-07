/*
 * zxbasm — ZX BASIC Assembler (C port)
 *
 * Main header file. Defines all types and state for the Z80 assembler.
 */
#ifndef ZXBASM_H
#define ZXBASM_H

#include "arena.h"
#include "strbuf.h"
#include "vec.h"
#include "hashmap.h"
#include "z80_opcodes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ----------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------- */
typedef struct Expr Expr;
typedef struct Label Label;
typedef struct AsmInstr AsmInstr;
typedef struct Memory Memory;
typedef struct AsmState AsmState;

/* ----------------------------------------------------------------
 * Token types (shared between lexer.c and parser.c)
 * ---------------------------------------------------------------- */
typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_COLON,        /* : */
    TOK_COMMA,        /* , */
    TOK_PLUS,         /* + */
    TOK_MINUS,        /* - */
    TOK_MUL,          /* * */
    TOK_DIV,          /* / */
    TOK_MOD,          /* % */
    TOK_POW,          /* ^ */
    TOK_LSHIFT,       /* << */
    TOK_RSHIFT,       /* >> */
    TOK_BAND,         /* & */
    TOK_BOR,          /* | */
    TOK_BXOR,         /* ~ */
    TOK_LP,           /* ( */
    TOK_RP,           /* ) */
    TOK_LB,           /* [ */
    TOK_RB,           /* ] */
    TOK_APO,          /* ' */
    TOK_ADDR,         /* $ (current address) */
    TOK_INTEGER,      /* integer literal */
    TOK_STRING,       /* "..." string literal */
    TOK_ID,           /* identifier */

    /* Z80 instructions */
    TOK_ADC, TOK_ADD, TOK_AND, TOK_BIT, TOK_CALL, TOK_CCF,
    TOK_CP, TOK_CPD, TOK_CPDR, TOK_CPI, TOK_CPIR, TOK_CPL,
    TOK_DAA, TOK_DEC, TOK_DI, TOK_DJNZ, TOK_EI, TOK_EX, TOK_EXX,
    TOK_HALT, TOK_IM, TOK_IN, TOK_INC, TOK_IND, TOK_INDR,
    TOK_INI, TOK_INIR, TOK_JP, TOK_JR, TOK_LD, TOK_LDD, TOK_LDDR,
    TOK_LDI, TOK_LDIR, TOK_NEG, TOK_NOP, TOK_OR, TOK_OTDR, TOK_OTIR,
    TOK_OUT, TOK_OUTD, TOK_OUTI, TOK_POP, TOK_PUSH, TOK_RES, TOK_RET,
    TOK_RETI, TOK_RETN, TOK_RL, TOK_RLA, TOK_RLC, TOK_RLCA, TOK_RLD,
    TOK_RR, TOK_RRA, TOK_RRC, TOK_RRCA, TOK_RRD, TOK_RST, TOK_SBC,
    TOK_SCF, TOK_SET, TOK_SLA, TOK_SLL, TOK_SRA, TOK_SRL, TOK_SUB,
    TOK_XOR,

    /* ZX Next instructions */
    TOK_LDIX, TOK_LDWS, TOK_LDIRX, TOK_LDDX, TOK_LDDRX,
    TOK_LDPIRX, TOK_OUTINB, TOK_MUL_INSTR, TOK_SWAPNIB, TOK_MIRROR_INSTR,
    TOK_NEXTREG, TOK_PIXELDN, TOK_PIXELAD, TOK_SETAE, TOK_TEST,
    TOK_BSLA, TOK_BSRA, TOK_BSRL, TOK_BSRF, TOK_BRLC,

    /* Pseudo-ops */
    TOK_ORG, TOK_DEFB, TOK_DEFS, TOK_DEFW, TOK_EQU, TOK_PROC,
    TOK_ENDP, TOK_LOCAL, TOK_END, TOK_INCBIN, TOK_ALIGN,
    TOK_NAMESPACE,

    /* Registers */
    TOK_A, TOK_B, TOK_C, TOK_D, TOK_E, TOK_H, TOK_L,
    TOK_I, TOK_R,
    TOK_IXH, TOK_IXL, TOK_IYH, TOK_IYL,
    TOK_AF, TOK_BC, TOK_DE, TOK_HL, TOK_IX, TOK_IY, TOK_SP,

    /* Flags (these overlap with register C and other tokens) */
    TOK_Z, TOK_NZ, TOK_NC, TOK_PO, TOK_PE, TOK_P, TOK_M,

    /* Preprocessor */
    TOK_INIT,
} TokenType;

typedef struct Token {
    TokenType type;
    int lineno;
    int64_t ival;         /* for TOK_INTEGER */
    char *sval;           /* for TOK_ID, TOK_STRING (arena-allocated) */
    char *original_id;    /* original case of identifier */
} Token;

/* ----------------------------------------------------------------
 * Lexer state
 * ---------------------------------------------------------------- */
typedef struct Lexer {
    AsmState *as;
    const char *input;
    int pos;
    int lineno;
    bool in_preproc;     /* after # at column 1 */
} Lexer;

void lexer_init(Lexer *lex, AsmState *as, const char *input);
Token lexer_next(Lexer *lex);

/* ----------------------------------------------------------------
 * Expression tree (deferred evaluation for forward references)
 * ---------------------------------------------------------------- */
typedef enum {
    EXPR_INT,          /* integer literal */
    EXPR_LABEL,        /* label reference */
    EXPR_UNARY,        /* unary operator (+, -) */
    EXPR_BINARY,       /* binary operator (+, -, *, /, ^, %, &, |, ~, <<, >>) */
} ExprKind;

struct Expr {
    ExprKind kind;
    int lineno;
    union {
        int64_t ival;          /* EXPR_INT */
        Label *label;          /* EXPR_LABEL */
        struct {               /* EXPR_UNARY */
            char op;           /* '+' or '-' */
            Expr *operand;
        } unary;
        struct {               /* EXPR_BINARY */
            int op;            /* operator char or EXPR_OP_LSHIFT, EXPR_OP_RSHIFT */
            Expr *left;
            Expr *right;
        } binary;
    } u;
};

#define EXPR_OP_LSHIFT  256
#define EXPR_OP_RSHIFT  257

/* Evaluate an expression. Returns true on success, false if unresolved.
 * If ignore_errors is true, returns false silently for undefined labels.
 * If ignore_errors is false, emits error messages. */
bool expr_eval(AsmState *as, Expr *e, int64_t *result, bool ignore_errors);

/* Try to evaluate (ignore errors). Returns true if resolved. */
bool expr_try_eval(AsmState *as, Expr *e, int64_t *result);

/* Create expression nodes (arena-allocated) */
Expr *expr_int(AsmState *as, int64_t val, int lineno);
Expr *expr_label(AsmState *as, Label *lbl, int lineno);
Expr *expr_unary(AsmState *as, char op, Expr *operand, int lineno);
Expr *expr_binary(AsmState *as, int op, Expr *left, Expr *right, int lineno);

/* ----------------------------------------------------------------
 * Labels
 * ---------------------------------------------------------------- */
struct Label {
    char *name;          /* mangled name (with namespace prefix) */
    int lineno;
    int64_t value;
    bool defined;        /* has a value been assigned? */
    bool local;          /* declared LOCAL within a PROC */
    bool is_address;     /* true if label = memory address (not EQU) */
    char *namespace_;    /* namespace where declared */
    char *current_ns;    /* namespace where referenced */

    /* Temporary label support */
    bool is_temporary;
    int direction;       /* -1 = backward (B), +1 = forward (F), 0 = not temporary */
};

/* ----------------------------------------------------------------
 * Assembly instruction
 * ---------------------------------------------------------------- */

/* Expression argument for an instruction.
 * An instruction can have 0, 1, or 2 expression arguments. */
#define ASM_MAX_ARGS 2

struct AsmInstr {
    int lineno;
    const char *asm_name;      /* mnemonic string e.g. "LD A,N" */
    const Z80Opcode *opcode;   /* pointer into opcode table (NULL for DEFB/DEFS/DEFW) */

    /* Pseudo-ops store data differently */
    enum { ASM_NORMAL, ASM_DEFB, ASM_DEFS, ASM_DEFW } type;

    /* For normal instructions: expression arguments */
    Expr *args[ASM_MAX_ARGS];
    int arg_count;
    int arg_bytes[ASM_MAX_ARGS]; /* byte width of each arg (1 or 2) */

    /* For DEFB/DEFW: variable-length expression list */
    Expr **data_exprs;
    int data_count;

    /* For DEFS: count expr and fill expr */
    Expr *defs_count;
    Expr *defs_fill;

    /* For INCBIN: raw bytes */
    uint8_t *raw_bytes;
    int raw_count;

    /* Pending resolution flag */
    bool pending;

    /* Cached resolved arg values */
    int64_t resolved_args[ASM_MAX_ARGS];

    /* Address where this instruction was placed (for second-pass resolution) */
    int start_addr;
};

/* Count 'N' argument slots in a mnemonic string */
int count_arg_slots(const char *mnemonic, int *arg_bytes, int max_args);

/* Compute bytes for an instruction. Returns byte count.
 * Writes to `out` (must be large enough). */
int asm_instr_bytes(AsmState *as, AsmInstr *instr, uint8_t *out, int out_size);

/* ----------------------------------------------------------------
 * Memory model
 * ---------------------------------------------------------------- */
#define MAX_MEM 65536

/* An org block: instructions at a given origin */
typedef struct OrgBlock {
    int org;
    VEC(AsmInstr *) instrs;
} OrgBlock;

struct Memory {
    int index;           /* current org pointer */
    int org_value;       /* last ORG directive value */

    /* Memory contents */
    uint8_t bytes[MAX_MEM];
    bool byte_set[MAX_MEM]; /* which bytes have been written */

    /* Per-address instruction mapping for second-pass resolution */
    AsmInstr *instr_at[MAX_MEM]; /* which instruction starts at this address */

    /* Labels: stack of scopes (for PROC/ENDP) */
    HashMap *label_scopes;  /* array of HashMaps */
    int scope_count;
    int scope_cap;

    /* PROC line number stack for error reporting */
    VEC(int) scope_lines;

    /* Instruction tracking per-org for dump */
    VEC(OrgBlock) org_blocks;

    /* Temporary labels */
    HashMap tmp_labels;       /* key: "filename:lineno:name" -> Label* */
    /* Per-file line lists for temporary labels */
    HashMap tmp_label_lines;  /* key: filename -> int* array */

    /* Pending temporary labels for resolution */
    HashMap tmp_pending;      /* key: filename -> Label** array */

    /* Namespace state */
    char *namespace_;
    VEC(char *) namespace_stack;
};

/* ----------------------------------------------------------------
 * Assembler state
 * ---------------------------------------------------------------- */

/* Init entry from #init directive */
typedef struct InitEntry {
    char *label;
    int lineno;
} InitEntry;

struct AsmState {
    Arena arena;
    Memory mem;

    /* Error handling */
    int error_count;
    int warning_count;
    int max_errors;
    FILE *err_file;
    HashMap error_cache;    /* dedup error messages */
    char *current_file;

    /* Options */
    int debug_level;
    bool zxnext;
    bool force_brackets;
    char *input_filename;
    char *output_filename;
    char *output_format;    /* "bin", "tap", "tzx" */
    bool use_basic_loader;
    bool autorun;
    char *memory_map_file;

    /* Parser state */
    const char *input;      /* preprocessed input text */
    int pos;                /* current position */
    int lineno;             /* current line */

    /* #init entries */
    VEC(InitEntry) inits;

    /* Autorun address (from END directive) */
    bool has_autorun;
    int64_t autorun_addr;
};

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

/* Initialize assembler state */
void asm_init(AsmState *as);

/* Destroy assembler state */
void asm_destroy(AsmState *as);

/* Assemble preprocessed input text */
int asm_assemble(AsmState *as, const char *input);

/* Generate binary output */
int asm_generate_binary(AsmState *as, const char *filename, const char *format);

/* Error/warning reporting (matches Python's errmsg format) */
void asm_error(AsmState *as, int lineno, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void asm_warning(AsmState *as, int lineno, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Memory operations */
void mem_init(Memory *m, Arena *arena);
void mem_set_org(AsmState *as, int value, int lineno);
void mem_add_instruction(AsmState *as, AsmInstr *instr);
void mem_declare_label(AsmState *as, const char *label, int lineno,
                       Expr *value, bool local);
Label *mem_get_label(AsmState *as, const char *label, int lineno);
void mem_set_label(AsmState *as, const char *label, int lineno, bool local);
void mem_enter_proc(AsmState *as, int lineno);
void mem_exit_proc(AsmState *as, int lineno);
int mem_dump(AsmState *as, int *org_out, uint8_t **data_out, int *data_len);

/* Namespace helpers */
char *normalize_namespace(AsmState *as, const char *ns);

#endif /* ZXBASM_H */
