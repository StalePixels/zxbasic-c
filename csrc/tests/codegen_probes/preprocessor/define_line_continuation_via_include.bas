' RED probe — backslash line-continuation in #DEFINE within an #included file.
'
' Top-level .bas with a #DEFINE \-continuation works in C zxbc.
' But when the same #DEFINE is in an #included file, the zxbc include
' path errors `illegal preprocessor character '\''` on lines 2+ of the
' macro body. Python's zxbc accepts the form via include without issue.
'
' Surfaced 2026-05-26 attempting to compile
' _ref/NextBuild/Sources/TilemapScroll/testmap.bas
'   #include <nextlib.bas>   ' a real Spectrum Next library
' nextlib.bas:13-18 is `#DEFINE NextReg(REG,VAL) \ <TAB>ASM\ <TAB>DW $91ED\ ...`
' which the zxbc include path rejects but Python's does not.
'
' This probe `#include`s the companion file containing the multi-line
' #DEFINE. The probe runner compares the full contract (exit, stderr,
' Stage-1 ASM, end-to-end binary) against the Python oracle.
'
' Acceptance: C zxbc output byte-identical to Python's for this input.

#include "_define_line_continuation.bi"

NextReg($07, 3)
