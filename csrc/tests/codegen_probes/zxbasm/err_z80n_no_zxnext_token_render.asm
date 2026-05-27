; RED PROBE - zxbasm error-cascade suppression mis-fires when the
; previous statement made a successful label decl AND then errored.
;
; Reduced from tests/functional/asm/no_zxnext.asm lines 8-9. Without
; --zxnext, MUL is not a recognised mnemonic and falls through to ID
; (which `asm : ID` reduces as a label declaration). The trailing
; `D,E` is then a fresh syntax error reported at the unexpected `D`.
; The NEXT line (`ADD HL,A`, also non-zxnext) is a clean ADD-with-bad-
; operand and should re-emit its own PLY-shape unexpected-token report.
;
; Python: emits `'D' [D]` on line N (MUL was declared then `D,E` errored)
;   and `'A' [A]` on line N+1 (clean re-error after a decl-having line).
; C  pre-fix: emits `'D' [D]` on line N, but line N+1 degrades to a bare
;   `Syntax error` with NO token render. Root cause: asm_unexpected's
;   cross-statement suppression read `decl_since_err` which it clobbers
;   to false on every call — so even though line N had set it true via
;   mem_declare_label(MUL), the trailing asm_unexpected on `D` reset it
;   and line N+1 was mis-classified as a no-decl cascade follower.
;
; Fix: track stmt_had_decl separately (reset per-statement in
; parse_program; set by every mem_declare_label callsite) and read it
; in asm_unexpected for the cross-statement decision.
;
; Scope: this probe covers the immediate-next-line cascade only. The
; broader no_zxnext.asm fixture exercises PLY's full `errorcount = 3`
; mechanism across many bad lines, which is a separate structural
; surgery and out of scope for this wave.
    org 0x8000
    MUL D,E
    ADD HL,A
    ret
