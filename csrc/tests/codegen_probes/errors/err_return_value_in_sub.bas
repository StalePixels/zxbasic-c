' REJECT: RETURN <value> inside a SUB.
' zxbparser p_return_expr:2125 "Syntax Error: SUBs cannot return a value".
Sub s
  Return 5
End Sub
s()
