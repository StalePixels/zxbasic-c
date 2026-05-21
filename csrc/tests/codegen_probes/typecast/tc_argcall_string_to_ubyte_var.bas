' string->numeric arg typecast: string var -> ubyte byval param.
' Python typecast.py:67-69 => error "Cannot convert string to a value. Use VAL()".
' This is the DEFERRED path c481f731 narrowed away (numeric->numeric only).
Sub s (p As uByte)
   Poke 16384, p
End Sub

Dim t As String
t = "x"
s (t)
