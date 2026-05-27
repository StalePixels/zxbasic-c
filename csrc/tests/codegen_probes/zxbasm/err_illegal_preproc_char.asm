; RED PROBE — zxbasm lexer error message text divergence.
; Python emits: error: illegal preprocessor character '@'
; C    emits  : error: illegal character '@'
; Divergence  : src/zxbasm/asmlex.py's t_INITIAL_preproc_error names this
;               error "illegal preprocessor character". C's lexer truncates
;               to plain "illegal character", losing the "preprocessor"
;               context that distinguishes it from non-preproc-state lexer
;               errors.
    org 0x8000
    @@@@@
    ret
