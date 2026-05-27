' CODE of a string literal must fold at parse time
' (Python: builtin.py:74-77 make_node fold on is_string single-arg ->
' SymbolNUMBER(ord(s[0]), TYPE.ubyte)). The runtime
' `ld hl,.LABEL... ; xor a ; call .core.__ASC` is NEVER emitted
' for a compile-time string-literal argument.
' NextBuild Layer2/RGB24to8bit.bas drove this — multiple
'   if keyin = code "1"
' branches diverge binary if the fold is missing.
Dim keyin As uByte
keyin = 49
If keyin = code "1"
   Print "match"
End If
