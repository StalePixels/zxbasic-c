' REJECT: LHS string-slice assignment with a numeric RHS.
' zxbparser p_let_substr:1270 syntax_error_expected_string (RHS not string).
Dim s As String
s = "abcd"
s(1 To 2) = 5
