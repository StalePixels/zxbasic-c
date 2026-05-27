; RED PROBE — zxbasm parser RD vs PLY divergence.
; Python emits: error: Syntax error. Unexpected token 'HL' [HL]
; C    emits  : error: Syntax error. Unexpected token 'hl' [HL]
; Divergence  : C's hand-written parser surfaces the lower-cased lexer token
; text instead of the source spelling (or PLY's canonical upper-case for
; register keywords). PLY port should reproduce Python's exact rendering.
    org 0x8000
    ld a, hl
    ret
