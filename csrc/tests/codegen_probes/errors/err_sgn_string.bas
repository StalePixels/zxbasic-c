' REJECT: SGN() of a string expression (expects numeric).
' zxbparser p_sgn:3490 "Expected a numeric expression, got TYPE.string instead".
Dim x As Integer
x = Sgn("hello")
Print x
