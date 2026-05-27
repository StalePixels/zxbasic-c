; RED PROBE — zxbasm DEFB silent-truncation at >256 items in one directive.
;
; A single `db` directive carrying more than 256 expressions overflows
; the fixed 256-byte scratch buffer in csrc/zxbasm/memory.c
; (write_instr / fixup pass): `uint8_t buf[256]; asm_instr_bytes(...)`.
; asm_instr_bytes correctly stops at out_size, so the location counter
; advances by 256 and the remaining bytes are silently dropped.
;
; Python emits 260 bytes; C emits 256. Default-flags raw `cmp` divergence.
;
; This was the last NextBuild Sources DIVERGE-BIN
; (XMASmodplay/XMAS2020intro2.bas, Δ=−16): one `db` line with 272 items
; emitting an array literal, capped to 256 by the scratch buffer.
;
; INCBIN data was already special-cased to bypass the scratch buffer
; (memory.c:510-525); the DEFB expression-list path was not.
    org 0x8000
mylabel:
    db 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
