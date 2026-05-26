' RED probe — DECLARE FUNCTION followed by SUB definition of the same
' name must keep the original (FUNCTION) class on the symbol table
' entry. Python's flow (zxbparser.py p_function_def -> declare_func
' symboltable.py:724):
'   1. check_class detects the kind mismatch -> emits
'      ":N: error: 'name' is a FUNCTION, not a SUB" and declare_func
'      returns None. Entry stays CLASS.function in the table.
'   2. p_function_def line 3026 fetches the EXISTING (function) entry;
'      FUNCTION_LEVEL[-1].class_ == function, so the SUB-ret-type
'      check at line 2994 does NOT fire.
'   3. At END SUB, p_end_function (zxbparser.py:3169-3184) compares
'      FUNCTION_LEVEL[-1].class_ (function) to the END keyword (sub)
'      and emits ":N: error: Unexpected token 'END SUB'.
'      Should be 'END FUNCTION'".
'
' Python output (three lines):
'   :3: warning: [W100] Using default implicit type 'float' for 'func0'
'   :4: error: 'func0' is a FUNCTION, not a SUB
'   :5: error: Unexpected token 'END SUB'. Should be 'END FUNCTION'
'
' C output today: the kind-mismatch error fires, then C OVERWRITES
' the entry's class_ to SUB (parser.c:7755) and runs the SUB-ret-type
' check downstream (parser.c:9265), emitting the wrong second error
' "SUBs cannot have a return type definition" and skipping the END
' mismatch entirely.
'
' Affects corpus fixture declare4 (parse meter SM list).

DECLARE FUNCTION func0(a as Ubyte, b as Uinteger)

SUB func0(a as Ubyte, b as Uinteger) as FLOAT
END SUB
