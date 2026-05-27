; RED PROBE — zxbasm warning message format divergence (W200 tag missing).
; Python emits: warning: [W200] Value will be truncated
; C    emits  : warning: value will be truncated
; Divergence  : (1) Python emits a leading [W200] code tag — produced by
; warning_value_will_be_truncated in src/api/errmsg.py. C's emit path is
; missing the [W200] tag entirely. (2) The leading word case ("Value" vs
; "value") differs. Both are byte-cmp visible in stderr.
    org 0x8000
    defb 1000
    ret
