' W190: a FUNCTION whose body does not end in RETURN. Called once so W170
' (never-called) does not also fire. optimize.py visit_FUNCTION.
Function f As Integer
  Print 1
End Function
Dim x As Integer
x = f()
Print x
