' RED probe — when a function body contains a call to an undeclared
' identifier (with parens), the [W150] "Parameter ... is never used"
' warning for unused params of the enclosing function MUST still
' fire. Python's leave_scope (symboltable.py:268-277) iterates the
' current scope's params unconditionally and emits W150 for any
' unaccessed non-byref param; body errors do not suppress it.
'
' C suppresses the W150 in this case — something about the
' undeclared-as-call path is either marking the unused param as
' accessed, or short-circuiting the leave_scope param-walk.
'
' Minimal trigger: `undecl()` (parenthesised call form) inside the
' enclosing function's body. `undecl` (no parens) does NOT trigger
' the bug — both Python and C emit the W150 correctly.
'
' Affects corpus fixture funccall6 (parse meter SM list).

Function SNETfopen(fname$, mode$) as Byte
    return SNETopen(undecl(), fname$)
End Function
