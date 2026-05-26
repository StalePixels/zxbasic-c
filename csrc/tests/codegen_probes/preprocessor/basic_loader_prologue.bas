' RED probe — BASIC-loader (-B) prologue codegen divergence.
'
' When compiling with `-B` (BASIC loader header) — required for .tap/.tzx
' programs that boot under BASIC — the C and Python compilers emit different
' Z80 sequences for the prologue that captures the stack pointer.
'
' C emits:
'   ld (NNNN), sp        ; ED 73 NN NN  — Z80-native single instruction (4 bytes)
'
' Python emits:
'   ld hl, 0             ; 21 00 00
'   add hl, sp           ; 39
'   ld (NNNN), hl        ; 22 NN NN
'                        ; (7 bytes; 8080-compatible sequence)
'
' Both correct; C is shorter. Per the byte-for-byte drop-in contract C must
' MATCH Python at the pinned commit, not be incidentally better.
'
' Surfaced 2026-05-26 compiling _ref/NextBuild/Sources/TilemapScroll/testmap.bas
' through nextbuild-c.py — C produced a 3992-byte binary, Python 3961, first
' divergence at offset 8 in the prologue.
'
' Python anchor: src/arch/zx48k/loader.bas / .py (the BASIC-loader-header
' codegen path). The C analogue lives in csrc/zxbc/loader_runtime.c (or
' wherever the -B path emits the header).
'
' Acceptance: a minimal `-B`-mode program produces a byte-identical binary
' under C and Python. The probe runner invokes both with `-B -a -f tap` and
' compares the binary.

' Minimal program — the prologue is what diverges, content is incidental.
PRINT "Hello"
