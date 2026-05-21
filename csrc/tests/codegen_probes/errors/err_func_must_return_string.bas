' REJECT: String FUNCTION returning a numeric value.
' zxbparser p_return_expr:2134 "Function must return a string, not a numeric value".
Function f As String
  Return 5
End Function
Dim s As String
s = f()
