' LET coercion: assign a float var into a byte var (decimal->integral narrowing).
Dim b As Byte
Dim f As Float
f = 3.9
b = f
Poke 16384, b
