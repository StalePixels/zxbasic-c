' REJECT: a VAR used as a FUNCTION (bare-id statement call).
' Python: p_statement_call (bare ID) -> make_sub_call -> access_func ->
'   syntax_error_unexpected_class (api/errmsg.py:320-323).
' C: pd_sub_call -> symboltable_access_func -> syntax_error_unexpected_class
'   (uppercase, vowel-aware article — n1='' for 'var', n2='' for 'function').
' Pins the funccall3 single-emit invariant (no preflight duplicate).
LET a = 5
a
