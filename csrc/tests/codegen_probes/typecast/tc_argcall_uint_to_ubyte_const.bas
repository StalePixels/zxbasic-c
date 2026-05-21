' numeric->numeric arg typecast: uinteger CONSTANT -> ubyte byval param.
' Constant operand => static fold path (typecast.py is_const branch);
' 300 masked to ubyte may emit W120 conversion-lose-digits.
Sub s (p As uByte)
   Poke 16384, p
End Sub

s (300)
