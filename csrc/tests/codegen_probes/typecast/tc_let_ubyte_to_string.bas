' LET coercion, numeric RHS into string LHS: assign ubyte var into string var.
' Python => error "Cannot convert value to string. Use STR()".
Dim b As uByte
Dim t As String
b = 65
t = b
End
