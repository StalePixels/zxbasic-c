' REJECT: bare RETURN inside a FUNCTION.
' zxbparser p_return:2106 "Syntax Error: Function must RETURN a value.".
Function f As Integer
  Return
End Function
Dim x As Integer
x = f()
