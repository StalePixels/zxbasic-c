' string->numeric arg via STR() result (mirrors lcd2.bas shape): STR$ -> byte param.
' Python typecast.py:67-69 => "Cannot convert string to a value. Use VAL()".
Function f (a As Byte) As uByte
   Return a
End Function

Dim n As uByte
n = f (Str(5))
Poke 16384, n
