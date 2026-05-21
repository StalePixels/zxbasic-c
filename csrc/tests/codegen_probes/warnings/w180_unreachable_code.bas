' W180: statement after GOTO (before the next label) is unreachable (opt>0).
' optimize.py visit_BLOCK: is_ender(GOTO) then forward scan -> warning_unreachable_code.
Goto skip
Print "dead"
skip:
Print "ok"
