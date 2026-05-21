' numeric->numeric arg typecast: fixed var -> integer byval (decimal->integral).
Sub s (p As Integer)
   Poke 16384, p
End Sub

Dim x As Fixed
x = 1.25
s (x)
