' W140: useless empty IF (empty THEN body, no ELSE) at opt>=1.
' optimize.py visit_IF: chk.is_null(then_, else_) -> warning_empty_if.
Dim a As Byte
a = 1
If a Then
End If
