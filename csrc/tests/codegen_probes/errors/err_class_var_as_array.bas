' REJECT: a CONST used as a FOR-loop variable.
' Python: p_for_sentence -> resolve var (access_id with default_class=var)
'   then check class -> syntax_error_unexpected_class ("'a' is a CONST,
'   not a VAR").  Exercises the const->var FOR-var rejection — a third
'   emit site, distinct from bare-ID and function-as-FOR-var.
CONST a = 5
FOR a = 1 TO 10
NEXT a
