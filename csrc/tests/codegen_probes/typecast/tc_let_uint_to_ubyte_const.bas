' LET coercion, constant RHS: assign 300 (uinteger-range) into a ubyte var.
' Static-fold mask path; may emit W120 conversion-lose-digits.
Dim b As uByte
b = 300
Poke 16384, b
