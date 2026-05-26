' RED probe — when a CONST declaration fails because its initializer
' is not constant, the identifier must NOT remain registered as
' CLASS_const in the symbol table. Python p_var_decl_ini at
' zxbparser.py:712-720 emits "Initializer expression is not constant"
' and RETURNS WITHOUT calling declare_const, so MAXMOBS stays
' undeclared (or transitions to a plain implicit-float var on next
' reference).
'
' Downstream effect at line 3 (DIM with bounds referencing MAXMOBS):
' Python sees MAXMOBS as a var (W100 implicit float at the
' reference) and check.is_static returns False, emitting
' "Array bounds must be constants" (bound.py:43-44).
'
' C today promotes MAXMOBS to CLASS_const at parser.c:6921 BEFORE
' the static check at parser.c:7016; the err_not_constant fires but
' the class promotion is not rolled back. At line 3 the bound's
' check_is_static sees CLASS_const and returns true, then
' eval_to_num fails (no actual value) and the wrong message
' "Unknown upper bound for array dimension" fires (bound.py:54
' analog at parser.c:6486). The :3 W100 also goes missing.
'
' Affects corpus fixture dim_const_crash (parse meter SM list).

const MAXMOBS as ubyte = MHEIGHT
dim mobCoords(0 to MAXMOBS, 0 to 1) as ubyte
