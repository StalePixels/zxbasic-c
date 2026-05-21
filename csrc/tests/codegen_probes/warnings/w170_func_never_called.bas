' W170: a SUB defined but never called (opt>1). A SUB (not FUNCTION) avoids
' W190 also firing. optimize.py visit_FUNCDECL: not entry.accessed.
Sub neverCalled
  Poke 16384, 1
End Sub
Print 1
