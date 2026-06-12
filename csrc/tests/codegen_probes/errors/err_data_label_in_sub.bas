' REJECT: DATA statements inside a SUB/FUNCTION emit BOTH the
' "DATA not allowed within Functions nor Subs" error AND a
' "Label '.DATA.__DATA__N' already used" cascade on the 2nd+ DATA.
'
' Python p_data (zxbparser.py:1734) calls make_label(DATA_PTR_CURRENT)
' -> declare_label BEFORE the FUNCTION_LEVEL guard (zxbparser.py:1742).
' The first in-SUB DATA declares the auto-label '.DATA.__DATA__0'
' cleanly then hits the guard (returns early, so DATA_PTR_CURRENT is
' NOT advanced); the second in-SUB DATA re-declares the SAME label
' name and declare_label (symboltable.py:594) raises
' "Label '...' already used at ..." -- emitted FIRST, then the guard's
' "DATA not allowed" -- before the C had a collision check on the
' p_data label path (it used symboltable_access_label raw, no declare).
Sub foo()
  DATA 1, 2, 3
  DATA 4, 5, 6
End Sub
foo()
