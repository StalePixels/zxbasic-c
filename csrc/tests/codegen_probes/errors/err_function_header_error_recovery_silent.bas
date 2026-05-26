' RED probe -- a parse error inside the function header (SUB/FUNCTION
' parameter list) must be SILENTLY swallowed by the
' function_header : function_def error NEWLINE error-recovery
' production (zxbparser.py:2940-2944). PLY emits NO error message
' for tokens consumed by the grammar error token; subsequent errors
' on later lines fire normally.
'
' Trigger: a reserved-word token (CODE) where the param ID is
' expected. PLY's error token consumes tokens until NEWLINE without
' producing any diagnostic.
'
' Python output for this fixture (two-error pattern -- the line-1
' header error is suppressed; only the line-2 body error and an
' EOF error fire):
'   :19: error: Syntax error. Unexpected end of line
'   :21: error: Syntax error. Unexpected end of file
'
' C today emits the line-1 header error too:
'   :18: error: Syntax Error. Unexpected token 'CODE' <CODE>
'   :19: error: Unexpected end of line
'
' Affects corpus fixtures due_par + due_inc_main (parse meter SM).

SUB test(code as Ubyte)
	k=in(12234
END SUB
