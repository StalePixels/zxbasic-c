' byref param with a DIFFERENT-typed arg: byte var passed ByRef to uInteger param.
' make_typecast on a byref ID of mismatched type — exercises whether the byref
' branch errors or silently casts. This is a corner the corpus does not cover.
Sub s (ByRef p As uInteger)
   p = 5
End Sub

Dim a As Byte
s (a)
