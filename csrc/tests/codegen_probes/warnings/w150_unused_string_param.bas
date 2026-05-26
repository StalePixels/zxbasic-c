' RED probe — [W150] Parameter 'X' is never used for a string param of an
' outer function that calls an inner function passing the OTHER param.
'
' Source pattern (mirrors corpus fixture funccall6):
'   Function Inner(a$, b$)
'   End function
'
'   Function Outer(used$, unused$) as Byte
'     Inner(used$, "lit")        ' used$ referenced; unused$ never accessed
'   End function
'
'   ' caller of Outer
'   print Outer("x", "y")
'
' Python: emits [W150] for 'unused' (and for the inner function's params).
' C: emits [W150] for the inner's params but NOT for Outer's 'unused'.
'
' My initial isolated probe (single Function Greet(msg$) ... print Greet(...))
' was GREEN — so the gap is structural, not type-based. The shape that
' triggers the miss is an outer function calling an inner, where the outer's
' string param goes unused. Likely the C's per-param accessed-tracking is
' confused by the param's appearance in a sibling-scope or has a
' resolution-vs-leave-scope ordering issue.
'
' Python anchor: src/api/symboltable/symboltable.py leave_scope — walks
' each scope's parameters at scope-exit and emits W150 for any not marked
' .accessed. Need to verify how `.accessed` gets set vs not in this shape.

Function Inner(a$, b$)
End function

Function Outer(used$, unused$) as Byte
    Inner(used$, "literal")
    return 1
End function

print Outer("first", "second")
