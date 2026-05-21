' REJECT: duplicated function declaration.
' zxbparser ~2926 "duplicated declaration for function '%s'".
Function f As Integer
  Return 1
End Function
Function f As Integer
  Return 2
End Function
Dim x As Integer
x = f()
