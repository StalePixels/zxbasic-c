' RED probe — Undeclared label lineno-of-first-reference attribution.
'
' Python emits `error: Undeclared label "X"` at the lineno of the FIRST
' reference to the label. When a bare identifier on its own line is the
' first reference (parsed as a statement before any GOTO/GOSUB to it),
' that's the lineno carried on the label entry; later GOTO/GOSUB
' references don't update it.
'
' Source (this file):
'   bare statement line 12:  MY_LABEL          ' first reference
'   GOTO line 13:            GOTO MY_LABEL
'
' Python:    "<file>:12: error: Undeclared label "MY_LABEL""
' C currently: "<file>:13: error: Undeclared label "MY_LABEL""
'
' Python anchor: `src/api/symboltable/symboltable.py` access_label /
' declare_label — entry.lineno is set on first creation (the bare-statement
' reference), and subsequent access_label calls (from p_goto) don't
' overwrite it. Verify via grep:
'   grep -n "lineno" src/api/symboltable/symboltable.py | grep -i label
'
' C site: the label-entry's lineno field at access/declare. Find the
' equivalent of Python's "first reference wins" rule in csrc/zxbc/compiler.c
' check_pending_labels (around the CLASS_label arm at :1572) or the label-
' creation site under symboltable_lookup / symboltable_access_label.

REM Will fail because it lacks a trailing COLON
REM Bare identifier on a line, followed by GOTO to the same name.
REM Python attributes the Undeclared-label error to the bare line (12);
REM C currently uses the GOTO line (13).










MY_LABEL
  GOTO MY_LABEL
