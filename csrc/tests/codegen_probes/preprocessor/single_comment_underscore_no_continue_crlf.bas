' RED probe — single-comment (') ending in standalone `_` must NOT
' continue.  Python t_singlecomment_CONTINUE (zxbpplex.py:192) is
' BACKSLASH ONLY, so a `'` comment ending ` _` does not line-continue
' even though the `_` is a standalone marker (space before it).  The
' next code line must survive.
dim a as ubyte ' note _
dim b as ubyte
a = 1
b = 2
