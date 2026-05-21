' REJECT: string-slice syntax applied to a numeric variable.
' check.check_type via strslice:70 -> "Wrong expression type 'integer'. Expected 'string'".
Dim n As Integer
n = 5
Print n(1 To 2)
