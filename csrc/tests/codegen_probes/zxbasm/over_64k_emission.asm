; Emission past the 64K address boundary must NOT be a fatal error.
;
; Python's assembler (src/zxbasm/memory.py) keeps emitted bytes in a
; sparse dict keyed by `org` and increments the location counter with no
; ceiling (__set_byte, memory.py:81-89); Memory.dump (memory.py:179)
; iterates range(org, max(addr)+1) so bytes written at addresses >= 65536
; are emitted in the output binary. Only an explicit ORG directive is
; range-checked to [0..65535] (memory.py:51-52).
;
; The C port modelled memory as a flat 65536-entry array and aborted with
; "Memory overflow at address 65536" the instant the location counter
; reached 0x10000 — diverging from Python on byte-identical input. This
; surfaced on real shipped programs: o-trix (ORG 0x8000, 38126 bytes ->
; top 0x114AD) and retrobsesion both DIFF-EXIT'd (Py=0 builds a binary,
; C=5 "Memory overflow"). Fixed by sizing the image to the practical Z80
; reach (zxbasm.h MAX_MEM = 0x20000).
;
; This fixture ORGs near the top of the address space and DEFS past
; 0xFFFF: pre-fix C exits non-zero with the overflow error; post-fix C
; and Python both assemble it and emit the same bytes (PROBE-EQUAL).
    org 0FFF0h
    defs 020h, 0AAh
