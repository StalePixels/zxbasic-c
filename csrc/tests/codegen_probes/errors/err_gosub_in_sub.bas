' REJECT: GOSUB inside a SUB/FUNCTION scope.
' zxbparser p_goto_part:1360 "GOSUB not allowed within SUB or FUNCTION".
Sub s
  GoSub 100
End Sub
s()
100 Print 1
