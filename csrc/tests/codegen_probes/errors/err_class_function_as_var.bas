' REJECT: a FUNCTION used as a FOR-loop variable.
' Python: p_for_sentence -> resolve var (access_id with default_class=var)
'   then check class -> syntax_error_unexpected_class ("'f' is a FUNCTION,
'   not a VAR").  Exercises the FOR-var resolution path (a separate emit
'   site from the bare-ID statement-call path).
Function f
    Return 1
End Function
FOR f = 1 TO 10
NEXT f
