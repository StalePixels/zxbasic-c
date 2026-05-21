' W160: a FASTCALL SUB declared with more than one parameter.
' zxbparser p_function_def_header: convention==fastcall and len(params)>1.
' Called once so W170 (never-called) does not also fire.
Sub FastCall s(a As uByte, b As uByte)
  Poke 16384, a + b
End Sub
s(1, 2)
