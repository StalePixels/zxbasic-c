' RED probe — array initializer with an invalid element must propagate
' None / abort declare_array, not cascade into check_bound.
'
' Python: `@a(1)` references an undeclared array; the @-of-undeclared
' reduces to None, and the const_vector containing it should reduce to
' None (or the array_decl_ini path should return without running
' check_bound). Python emits ONE error:
'   ":2: error: Undeclared array \"a\""
'
' C: emits the same Undeclared-array error, then continues into
' check_bound which counts 2 surviving elements against the expected 3
' and emits a CASCADING:
'   ":2: error: Mismatched vector size. Expected 3 elements, got 2."
'
' Affects corpus fixture arrlabels10d (parse meter SM 9).

DIM a(1 TO 3) as UInteger => {@a(1), @a + 1, 4.0}
