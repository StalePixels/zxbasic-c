' REJECT: an array parameter passed ByVal.
' zxbparser p_param_byval_definition:3091 syntax_error_cannot_pass_array_by_value.
Sub s(ByVal a() As Byte)
End Sub
Dim arr(10) As Byte
s(arr)
