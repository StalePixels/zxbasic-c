' RED probe — `#define <invalid-char>` must emit TWO errors:
'   1. "illegal preprocessor character '<c>'" (from Python's catch-all
'      lex rule, zxbpplex.py:399-401 / 407-409, in the prepro_define
'      state).
'   2. "Syntax error. Unexpected end of line" (from PLY's p_error,
'      zxbpp.py:885-892, when the parser then sees DEFINE NEWLINE
'      because the lex catch-all consumed the bad char without
'      returning a token).
'
' C today emits ONE non-Python message from handle_define
' (preproc.c:808): "expected identifier after #define" — the
' first-char illegality detection and the EOL recovery are both
' missing.
'
' Affects corpus fixture baspreprocerr2 (parse meter SM list).

#define @
