' RED probe — redeclaring a variable that collides with a parameter:
' Python distinguishes "already declared as a parameter at" from the
' generic "already declared at"; C emits only the generic message.
'
' Python anchor: src/api/symboltable/symboltable.py:487-495 — declare_variable
' two-branch emission:
'    if entry_.scope == SCOPE.parameter:
'        "Variable '<id>' already declared as a parameter at <file>:<line>"
'    else:
'        "Variable '<id>' already declared at <file>:<line>"
'
' C: csrc/zxbc/compiler.c:455 and csrc/zxbc/parser.c:6857 emit the generic
' message in both cases — missing the parameter-scope branch.
'
' Affects corpus fixture semantic1 (parse meter SM 13).

Function f(t as byte)
   Dim t as byte
End Function
