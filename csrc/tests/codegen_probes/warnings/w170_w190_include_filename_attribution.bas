' RED probe — W170/W190 filename attribution for functions in an #included file.
'
' Python emits W170 ("never called") and W190 ("should return a value")
' in the OPTIMIZE phase (src/api/optimize.py:310 / :105), passing the
' AST node's stored filename explicitly:
'     warning_func_is_never_called(..., fname=node.entry.filename)
'     warning_function_should_return_a_value(lineno, node.name, node.filename)
' That stored filename is global_.FILENAME captured when the symbol was
' first created (src/symbols/id_/_id.py:58) — i.e. the file that was
' #line-active when the FUNCTION was DECLARED, here the #included file.
'
' The C port emitted these two warnings from the optimize/unreachable
' passes using cs->current_file, which by then has reverted to the main
' .bas — so before the fix C attributed both warnings to the including
' .bas while Python attributed them to the included .bi. (W150/W160 were
' already correct because they fire at parse/check time when
' cs->current_file still pointed at the included file.)
'
' This was surfaced by the released-corpus `retrobsesion` program, whose
' auto-included stdlib ATTR.BAS/POINT.BAS functions triggered exactly
' this W170/W190 mis-attribution.
'
' Fix: csrc/zxbc/passes/optimizer.c (W170) and unreachable.c (W190) now
' swap cs->current_file to the entry/id stored filename for the emit —
' the same fname= analogue already used by the R11 emit in compiler.c.
'
' Acceptance: C zxbc stderr (and full contract) byte-identical to Python.
' noReturnFn is called so it is not also W170-pruned, isolating W190.

#include "_w170_w190_included_funcs.bi"

Dim r As Integer
r = noReturnFn()
Print r
