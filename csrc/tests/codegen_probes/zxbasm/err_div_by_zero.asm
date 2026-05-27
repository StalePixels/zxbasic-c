; RED PROBE — zxbasm expression evaluator divergence.
; Python emits: error: Division by 0  (exit code 1, no .bin)
; C    emits  : nothing — silently succeeds with exit 0 (and emits a binary).
; Divergence  : src/zxbasm/expr.py guards integer-division with an explicit
;               "Division by 0" error; the C expression evaluator is missing
;               that guard entirely. This is a silent-acceptance bug
;               (FALSE_NEG would normally be a directive-1 hard violation,
;               but div-by-zero is upstream-reachable so it's listed
;               as an error-fidelity gap).
    org 0x8000
    ld a, 10/0
    ret
