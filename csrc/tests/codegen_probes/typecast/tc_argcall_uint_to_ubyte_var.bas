' numeric->numeric arg typecast: uinteger variable -> ubyte byval param.
' The path c481f731 deliberately handles. Variable operand => runtime cast.
Sub s (p As uByte)
   Poke 16384, p
End Sub

Dim a As uInteger
a = 300
s (a)
