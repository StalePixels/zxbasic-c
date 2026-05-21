' Explicit CAST builtin with a STRING operand: CAST(Integer, stringVar).
' grammar: CAST LP numbertype COMMA expr RP — operand may be a string expr,
' so this hits typecast.py:67-69 string->value error via make_typecast.
Dim t As String
Dim n As Integer
t = "x"
n = Cast(Integer, t)
End
