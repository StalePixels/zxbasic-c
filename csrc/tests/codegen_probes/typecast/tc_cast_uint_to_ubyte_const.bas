' Explicit CAST builtin, constant operand: CAST(uByte, 300) — static-fold + mask.
Dim b As uByte
b = Cast(uByte, 300)
Poke 16384, b
