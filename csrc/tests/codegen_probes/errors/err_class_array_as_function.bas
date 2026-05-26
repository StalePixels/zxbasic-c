' REJECT: a CONST used as a FUNCTION via bare-id statement call.
' Python: p_statement_call -> make_sub_call -> access_func ->
'   syntax_error_unexpected_class ("'a' is a CONST, not a FUNCTION").
' Exercises a different wrong_class enum value (CONST vs VAR), still going
' through the single-emit access_func path — no preflight duplicate.
CONST a = 5
a
