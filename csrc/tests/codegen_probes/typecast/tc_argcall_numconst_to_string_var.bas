' numeric CONSTANT -> string param: a numeric literal arg to a String param.
' Python typecast.py:72-74 => error "Cannot convert value to string. Use STR()".
Sub s (p As String)
   Print p
End Sub

s (65)
