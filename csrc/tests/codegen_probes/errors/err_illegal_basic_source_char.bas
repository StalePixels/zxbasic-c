' RED probe -- illegal characters in BASIC source outside strings
' and comments must be rejected by the preprocessor with the exact
' message "illegal preprocessor character X", with no further
' downstream parse output. Python zxbpplex.py 399-401 catch-all
' fires from the prepro INITIAL state and consumes the bad char
' without producing a token, so the BASIC compiler lexer never
' sees it.
'
' Characters that trip the catch-all in INITIAL state (empirically,
' verified against Python 3.13): backtick, question mark, and
' a lone backslash.
'
' C today catches these in the BASIC compiler lexer with a
' different message "ignoring illegal character" lexer.c 1229
' and then cascades into a syntax-error and an implicit-type
' W100, producing three diagnostics instead of one.
'
' Affects corpus fixture extra_chars1 (parse meter SM list).

LET `a = 1
