' DO WHILE with EXIT DO and CONTINUE DO.
Dim i As Byte
i = 0
Do While i < 10
  i = i + 1
  If i = 3 Then Continue Do
  If i = 7 Then Exit Do
  Print i
Loop
