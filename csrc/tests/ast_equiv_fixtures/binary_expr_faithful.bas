' Phase 5 calibration — faithful case (Sprint 7).
'
' Goal: a minimal program that exercises a BINARY expression with two
' VAR operands of the same type, so Python's parser produces a BINARY
' subtree without inserting any TYPECAST coercion. The C port's AST,
' once Sprint 8 ships zxbc-ast-dump, should match byte-identical.
'
' This is the "the harness is wired right when it should report EQUAL"
' assertion — a non-divergent control case.

DIM a AS UBYTE
DIM b AS UBYTE
LET a = a + b
