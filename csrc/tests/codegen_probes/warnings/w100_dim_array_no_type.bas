' RED probe -- DIM <id>(bounds) without an `as <type>` clause emits
' [W100] (already passes parse-only). Full-compile path must NOT emit
' the spurious [W150] "Variable 'a' is never used" for an unused
' array at the default optimization level (-O2).
'
' Python anchor: src/api/symboltable/scope.py:63-66 -- at
' OPTIONS.optimization_level > 1, scope.values(filter_by_opt=True)
' filters OUT unaccessed entries. zxbparser.py:550-551 uses this
' filter when building data_ast from SYMBOL_TABLE.arrays, so an
' unaccessed array is dropped from data_ast at O>1 and
' var_translator.py:49-50 visit_ARRAYDECL never runs on it.
' Empirical Python output at default (-O2): silent.
'
' C csrc/zxbc/var_translator.c:670-676 fires W150 for any unaccessed
' ARRAYDECL it reaches; the data_ast construction does not apply the
' O>1 filter, so visit_ARRAYDECL is reached and W150 fires.
'
' The probe-runner's full-compile asm-stage compare surfaces this
' (parse-only does not invoke var_translator).

DIM a(1 TO 3)
