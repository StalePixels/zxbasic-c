' LET coercion, string RHS into numeric LHS: assign string var into ubyte var.
' Python => error "Cannot convert string to a value. Use VAL()".
Dim b As uByte
Dim t As String
t = "x"
b = t
End
