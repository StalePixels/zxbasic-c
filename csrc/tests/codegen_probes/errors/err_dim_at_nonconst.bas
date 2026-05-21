' REJECT: DIM ... AT a non-constant address expression.
' zxbparser p_var_decl_at:676 syntax_error_address_must_be_constant.
Dim b As Byte
b = 100
Dim a As Byte At b
