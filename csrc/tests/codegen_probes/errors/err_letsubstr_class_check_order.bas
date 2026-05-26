' RED probe — `LET id$(args, args) = rhs` where id$ was previously
' referenced as a 2-arg func_call inside an IF must emit ONE error:
'   "Cannot assign a value to 'm$'. It's not a variable"
' (Python p_substr_assignment, zxbparser.py:1248-1263 — class_ check
' fires BEFORE the index-count check).
'
' Trigger: the IF expression's `m$(s(1),s(2))` goes through Python's
' make_call (zxbparser.py:365-418). With 2 args and an unknown-class
' id$, make_call falls through line 386's `len(args) == 1` to_var
' early-out, fails the CLASS_var/const branch, and ends in
' make_func_call which CONVERTS m$ to CLASS_function via
' convert_to_function. Then p_substr_assignment sees CLASS_function
' and emits "Cannot assign a value to ... not a variable".
'
' C currently emits:
'   "Accessing string with too many indexes. Expected only one."
' because parser.c's make_call equivalent (parse_call_or_array /
' make_call_node) does NOT call convert_to_function for the 2-arg
' string-id case — m$ stays CLASS_var (or unknown). Then case 81
' falls through to the index-count check and fires the cascading
' "too many indexes" error.
'
' Affects corpus fixture substr_err (parse meter SM list).

DIM s(1 TO 2) as UInteger
IF m$(s(1),s(2))="\b" THEN LET m$(s(1),s(2))="\c"
