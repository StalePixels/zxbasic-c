; RED PROBE — zxbasm error-cascade divergence.
; Python emits exactly one error: "Syntax error. Unexpected token 'foo' [ID]"
; (the PROC parser rejects 'foo' as an unexpected token because PROC takes no
; name in this grammar arm; the parser does NOT subsequently complain about
; the missing ENDP because parse-recovery suppresses the scope-open).
; C    emits  : the same syntax-error line PLUS an extra cascade
;               "error: Missing ENDP to close this scope"
; Divergence  : C's recursive-descent parser opens the PROC scope before it
;               reports the syntax error, then the end-of-file scan finds an
;               unclosed scope and re-reports. PLY-driven parse recovery
;               drops the half-open scope so the cascade does not fire.
    org 0x8000
    proc foo
    ret
