' RED probe — string literals carrying ZX BASIC control-code escapes that
' lex to embedded NUL bytes.
'
' src/zxbc/zxblex.py's "string" lexer state expands escapes to raw bytes:
'   \{p0} -> chr(17),chr(0)   (PAPER 0)   zxblex.py:331-336
'   \{i5} -> chr(16),chr(5)   (INK 5)     zxblex.py:339-344
' So "\{p0}\{i5}RETRO" lexes to the 9-byte string
'   11 00 10 05 52 45 54 52 4F  ('R' 'E' 'T' 'R' 'O')
' i.e. a string whose SECOND byte is NUL. Python carries the byte length
' through the token/AST and emits all 9 bytes (DEFW 0009h + 9 DEFB).
'
' The C port's lexer (csrc/zxbc/lexer.c lex_string) accumulated the bytes
' correctly but then stored the value with arena_strdup (C-string), which
' truncates at the first NUL — and make_string re-measured with strlen.
' Net: the string collapsed to its 1 leading byte (DEFW 0001h + DEFB 11h),
' and the trailing bytes leaked out as stray data. End-to-end binary and
' Stage-1 ASM both diverged.
'
' Surfaced by the released-corpus `retrobsesion` program, whose title
' strings use \{p0}\{i5} attribute escapes (PAPER 0 = NUL second byte).
'
' Fix: carry the explicit byte length lexer -> token -> AST_STRING and copy
' length bytes (not strdup/strlen) so embedded NULs survive.
'
' Acceptance: C zxbc full contract (exit, stderr, Stage-1 ASM, binary)
' byte-identical to Python.

Dim a As String
a = "\{p0}\{i5}RETRO"
Print a
