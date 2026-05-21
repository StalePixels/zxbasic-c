' numeric->string arg typecast: ubyte var -> string byval param.
' Python typecast.py:72-74 => error "Cannot convert value to string. Use STR()".
Sub s (p As String)
   Print p
End Sub

Dim b As uByte
b = 65
s (b)
