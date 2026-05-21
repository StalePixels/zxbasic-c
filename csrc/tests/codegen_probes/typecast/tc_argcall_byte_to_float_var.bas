' numeric->numeric (integral->decimal widening) arg typecast: byte var -> float byval.
Sub s (p As Float)
   Dim q As Float
   q = p
End Sub

Dim b As Byte
b = 7
s (b)
