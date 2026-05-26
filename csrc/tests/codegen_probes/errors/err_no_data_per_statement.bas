' RED probe — No DATA defined: per-statement lineno attribution.
'
' Python emits `error: No DATA defined` at the lineno of EACH READ/RESTORE
' statement when DATA is never declared. C's --parse-only path fires a single
' fallback at lineno 0 instead.
'
' Source (this file):
'   line 3:  RESTORE     ; Python: ":3: error: No DATA defined"
'   line 5:  READ a      ; Python: ":5: error: No DATA defined"
'
' Python anchor:
'   src/arch/z80/visitor/translator.py:485  syntax_error_no_data_defined(node.lineno) in visit_RESTORE
'   src/arch/z80/visitor/translator.py:500  syntax_error_no_data_defined(node.lineno) in visit_READ
'   src/api/errmsg.py:299-300                helper: error(lineno, "No DATA defined")
'
' C state:
'   csrc/zxbc/translator.c:2448 / :2497    err_no_data_defined(cs, node->lineno)  — correct, but doesn't run at --parse-only.
'   csrc/zxbc/main.c:378                    zxbc_error(&cs, 0, "No DATA defined")  — fallback with lineno 0, fires at --parse-only.
'
' Acceptance: C's --parse-only stderr byte-identical to Python's for this fixture:
'   ":3: error: No DATA defined" + ":5: error: No DATA defined" (no `:0:` line).

RESTORE

READ a
