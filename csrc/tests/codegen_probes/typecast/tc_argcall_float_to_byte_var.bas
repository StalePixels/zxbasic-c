' numeric->numeric (decimal->integral) arg typecast: float var -> byte byval.
Sub s (p As Byte)
   Poke 16384, p
End Sub

Dim f As Float
f = 3.5
s (f)
