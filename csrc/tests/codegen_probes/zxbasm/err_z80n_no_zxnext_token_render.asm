; RED PROBE - zxbasm error-cascade suppression mis-fires on consecutive
; per-line errors, killing the token render on every error after the first.
;
; Reduced from tests/functional/asm/no_zxnext.asm. Without --zxnext, the
; Z80N mnemonics LDIX/LDWS/... fall through to ID and become label
; declarations; MUL/ADD/etc on subsequent lines surface as syntax errors
; on their operand tokens.
;
; Python: emits each line's bad-token error with the PLY token render
;   `Syntax error. Unexpected token 'D' [D]`, `'A' [A]`, etc.
; C    : emits line N (first error) correctly with token render, but every
;   subsequent error degrades to a bare `Syntax error` with NO token.
;
; Root cause: the prev_err_no_decl suppression flag was set by line N's
; asm_unexpected even though line N did make a successful label decl
; (the ID-as-label reduce). decl_since_err gets clobbered to false when
; asm_unexpected fires, so line N+1's lookup `prev_err_no_decl && !decl_since_err`
; mis-classifies line N+1 as a should-be-suppressed cascade follower.
    org 0x8000
    MUL D,E
    ADD HL,A
    ADD HL,0201h
    ret
