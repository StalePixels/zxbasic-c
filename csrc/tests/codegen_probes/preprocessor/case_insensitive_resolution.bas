' RED probe — #pragma case_insensitive = TRUE must affect identifier
' resolution, not just be parsed and set as an option.
'
' Surfaced 2026-05-26 while compiling
' _ref/NextBuild/Sources/TilemapScroll/testmap.bas via the nextbuild.py-style
' invocation. The library's nextlib.bas opens with `#pragma case_insensitive
' = TRUE`, then testmap.bas (still under that pragma) calls `memcopy(...)`
' all-lowercase. The upstream stdlib's memcopy.bas declares `sub fastcall
' MemCopy(...)` (mixed case). Python resolves the call via case-insensitive
' lookup; C errors `Undeclared function "memcopy"`.
'
' Python anchor:
'   - src/api/config.py:52         CASE_INS = "case_insensitive"
'   - src/api/symboltable/symboltable.py:115
'                                  entry.caseins = OPTIONS.case_insensitive
'   - lookups must honor entry.caseins (the case-insensitive comparison
'     happens during get_entry / access_call / access_id when the entry was
'     declared under the pragma).
'
' C state:
'   - csrc/zxbc/options.h:41           bool case_insensitive
'   - csrc/zxbc/parser.c:3769          option is set on the pragma
'   - BUT the option is never consulted in symbol lookup — there is no
'     equivalent of entry.caseins in the C symbol table.
'
' Acceptance: C zxbc output byte-identical to Python's for this input.

#pragma case_insensitive = TRUE

Sub fastcall MyHelper(x as uinteger)
  Return
End Sub

' Call with mixed casing — should resolve under case_insensitive=TRUE
myhelper(1)
MYHELPER(2)
MyHelper(3)
