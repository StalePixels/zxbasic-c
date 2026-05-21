' Nested FOR loops with EXIT FOR from the inner one.
Dim i As Byte
Dim j As Byte
For i = 1 To 3
  For j = 1 To 3
    If j = 2 Then Exit For
    Print i * j
  Next j
Next i
