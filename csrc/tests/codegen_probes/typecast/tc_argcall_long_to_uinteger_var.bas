' numeric->numeric arg typecast: long var -> uinteger byval (narrowing integral).
Sub s (p As uInteger)
   Poke 16384, p
End Sub

Dim l As Long
l = 100000
s (l)
