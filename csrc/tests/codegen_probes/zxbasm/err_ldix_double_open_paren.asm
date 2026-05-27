; RED PROBE - zxbasm IX-indexed addressing should reject malformed
; double-open-paren shape `(ix (...))`. Python emits a syntax error at
; the spurious `)`; C silently accepts and emits a binary.
;
; Python: `:2: error: Syntax error. Unexpected token ')' [RP]` (exit 1).
; C     : exit 0, binary emitted (no error).
;
; Anchor: the indexed-address production should match `(IX +/- expr)` and
; reject `(IX <anything-non-+/->`. The C parser is permitting tokens after
; IX without enforcing the displacement operator, then the inner `(` is
; eaten as a sub-expression, leaving the outer `)` orphaned but ignored.
    org 0x8000
    ld (ix (- 12 + 5)), 0
    ret
