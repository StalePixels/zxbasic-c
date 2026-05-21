' byref param + same-type arg: byref typecast path (no conversion, type match).
' Exercises check.py byref branch (must be ID/ARRAYLOAD) with typecast no-op.
Sub s (ByRef p As uInteger)
   p = 5
End Sub

Dim a As uInteger
s (a)
