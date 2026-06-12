' RED probe — W150 filename attribution for an unused module-level DIM in an
' #included file.
'
' Python emits W150 ("Variable ... is never used") for an unaccessed
' module-level variable in the VAR-translation phase
' (src/arch/z80/visitor/var_translator.py:29 visit_VARDECL — and :50
' visit_ARRAYDECL for arrays), passing the entry's stored filename
' explicitly:
'     src.api.errmsg.warning_not_used(entry.lineno, entry.name,
'                                     fname=entry.filename)
' That stored filename is global_.FILENAME captured when the symbol was
' first created (src/symbols/id_/_id.py:58) — i.e. the file that was
' #line-active when the variable was DECLARED, here the #included file.
'
' The C port emitted this W150 from csrc/zxbc/var_translator.c
' (vt_visit_vardecl:817 and vt_visit_arraydecl:642) using cs->current_file,
' which by the var-translation phase has reverted to the main .bas — so
' before the fix C attributed the warning to the including .bas with the
' included file's line number: wrong file, plausible line. (The W150 for
' `localUsedThenDropped` below correctly attributes to THIS .bas because it
' fires from the optimizer LET path with fname=lvalue.filename already.)
'
' This is the same family as the already-fixed W170/W190 include-filename
' attribution work (commit c59df290c), but on the W150 module-variable
' path which that fix did not cover. Surfaced by the released-corpus
' `maritrini` program (plScore in engine.bas vs maritrini.bas:69).
'
' Fix: csrc/zxbc/var_translator.c now swaps cs->current_file to the
' entry's stored filename for the duration of each W150 emit, then
' restores — the same fname= analogue used by the W170/W190 emits.
'
' Acceptance: C zxbc stderr (and full contract) byte-identical to Python.

#include "_w150_included_unused_var.bi"

Dim localUsedThenDropped As UByte
localUsedThenDropped = 1
