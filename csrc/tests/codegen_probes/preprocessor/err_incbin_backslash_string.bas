' RED probe -- backslash inside a string literal in ASM mode must NOT
' trigger C's "illegal preprocessor character '\'" rule. The Python
' lexer (src/zxbpp/zxbpplex.py:327) has a STRING rule that applies to
' INITIAL/pragma/prepro/defexpr/asm/if states alike — a doubled-quoted
' string consumes everything up to the closing quote as one token, so
' a `\` inside the string never reaches the asm-mode catch-all.
'
' Real-world NextBuild Sources affected (Windows-style incbin paths):
'   - NewYearDancer/dancer.bas:206   incbin ".\data\tiles.nxp"
'   - Nextscii-tilemap/tm-netscii-2.bas:493   same construct
'
' Python ground truth (asset missing or not): zxbpp/zxbc emit no
' "illegal preprocessor character '\'" error — the backslash is part
' of the STRING token and is preserved verbatim through the preproc.

asm
incbin ".\foo.bin"
end asm
