' Phase 1 semantic-fidelity — LET drifted control (S1.1).
'
' UBYTE lvalue, UINTEGER RHS variable: Python's p_assignment wraps the
' RHS in make_typecast(variable.type_, ...) (zxbparser.py:1115) so the
' LET sentence's child[1] is a TYPECAST. The C port (parser.c:1516-1519)
' adds the raw expression with no TYPECAST. probe_let asserts the RHS
' TYPECAST-wrapper presence; RED at S1.1, GREEN after the S1.2 fix.

DIM a AS UBYTE
DIM e AS UINTEGER
LET a = e
