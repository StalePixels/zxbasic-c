' Included by w170_w190_include_filename_attribution.bas.
' Holds the two functions whose W170/W190 warnings must attribute to
' THIS file's name (not the including .bas):
'   - neverUsedFn : never called  -> [W170] "is never called and has been ignored"
'   - noReturnFn  : called, but body never RETURNs -> [W190] "should return a value"
Function neverUsedFn As Integer
  Return 7
End Function

Function noReturnFn As Integer
  Print 1
End Function
