' RED probe — DIM <id>(bounds) without an `as <type>` clause must emit
' [W100] for the array's implicit default-float type.
'
' Python anchor: src/api/symboltable/symboltable.py:702 in declare_array:
'     if type_.implicit:
'         warning_implicit_type(lineno, id_, type_)
'
' C state: csrc/zxbc/parser.c dim_build_array (~:7106) doesn't call
' warn_implicit_type at all — the W100 for DIM-array with no AS clause
' is silently omitted.
'
' Affects corpus fixture strict7 (parse meter SM 12).

DIM a(1 TO 3)
