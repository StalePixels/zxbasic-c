' Phase 5 calibration — drifted case (Sprint 7).
'
' Goal: a minimal FOR loop where the loop variable type and the bound
' expression types disagree, so Python's parser inserts TYPECAST nodes
' in the FOR subtree to coerce the bounds. The C port (per the
' production-fidelity audit) does NOT insert these TYPECAST nodes — so
' the AST diff should report DIFF on a TYPECAST-shaped divergence site.
'
' (Note: literal numeric bounds like `FOR x = 0 TO 100` get folded to
' the loop var's type at parse time and don't trigger TYPECAST. Variable
' bounds with a wider declared type DO trigger it — verified to produce
' 2 TYPECAST nodes in Python's AST.)
'
' This is the "the harness catches real port drift when it should
' report DIFF" assertion — the divergent control case.
'
' Production-fidelity audit reference: docs/captures/zxbasic-c/
' production-fidelity.md flagged FOR's missing-TYPECAST as the canonical
' Round 1 backlog item; Sprint 9's verify-phase5-calibration reads it
' from this fixture.

DIM x AS BYTE
DIM e AS UINTEGER

FOR x = e TO e + 10
  LET x = x
NEXT
