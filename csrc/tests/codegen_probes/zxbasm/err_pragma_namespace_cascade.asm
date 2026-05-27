; RED PROBE — zxbasm namespace-pragma error cascade & token-name divergence.
; Python emits a clean per-line "illegal character '#'" + "Syntax error.
; Unexpected token 'push_namespace' [ID]" pair, lines 2/3/4 each.
; C    emits  : numeric token id [23] instead of named [ID]; AND fabricates
;               spurious "label '.pragma' already defined at line 2" cascade
;               errors that Python never emits; AND fails to emit the
;               per-line "illegal character '#'" at all.
; Divergence  : (1) token-name table lookup (PLY emits [ID], C emits [23]).
;               (2) parse-recovery cascade — C's RD recovery inserts the
;               unconsumed identifier '.pragma' into the label table on the
;               first failure and then trips a duplicate-label check on the
;               second and third. (3) the '#' character itself never raises
;               the lexer's illegal-character error in C.
    org 0x8000
    #pragma push_namespace foo
    #pragma pop_namespace
    #pragma pop_namespace
    ret
