' RED probe -- single-line `asm : ... : end asm` on one line must
' fully exit asm mode in the preprocessor. C's zxbpp leaves
' `in_asm` set when asm/end-asm appear on the same line separated
' by colons, so subsequent BASIC lines containing '=' hit the
' asm-mode catch-all "illegal preprocessor character '='" rule.
'
' Python's preprocessor handles this cleanly: the `end asm` token
' on the same line restores INITIAL state for the next line.
'
' Affects real-world NextBuild Sources: any program using the
' compact `asm : x : end asm` shorthand followed by a BASIC `dim`
' or `const` with an initializer. The stage-02 sweep reports
' 5 C-CRASH cases + 2 BOTH-FAIL cascades on this exact symptom
' (Collisiontests/collisiontest2.bas, HoleyMoley/holeymoley.bas,
' TopDownExample/alien-b.bas, alien-levelslide.bas, etc.) plus
' the cascade through #include nextlib.bas which contains the
' same shape.

asm : di : ei : end asm
dim x as ubyte = 1
