' Left shift with a FLOAT left-hand operand. Per Python's parser
' (zxbparser.p_expr_shl_expr / p_expr_shr_expr, src/zxbc/zxbparser.py:
' 2406-2421, 2424-2439), a float-or-fixed LHS to SHL or SHR is
' type-cast to ulong BEFORE the BINARY node is built. The IC stream
' therefore emits shlu32 / shru32 (not the non-existent "shlf" /
' "shrf"). If the C parser is missing the float->ulong typecast, the
' backend hits "no _QUAD_TABLE entry for IC 'shlf'".
'
' Repro surfaced from NextBuild/Sources/Layer2/RGB24to8bit.bas during
' the parse-meter sweep. Both Python and C should accept this with the
' float silently promoted to ulong for the shift.
DIM r AS float
r = 7.0
PRINT r << 2
PRINT r >> 1
