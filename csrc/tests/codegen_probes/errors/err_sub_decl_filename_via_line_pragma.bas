' RED probe — "sub declared but not implemented" must use the entry's
' filename (the #line-active filename when the SUB was declared), not the
' currently-active filename at error-emit time.
'
' Python: "inner.bas:5: error: sub 'Foo' declared but not implemented"
' C:      attributes to a different filename.
'
' The pattern mirrors corpus fixture bad_fname_err4: two #line directives,
' a sub declared (no implementation) under the inner, called, then the
' active filename switches back. Python uses entry.filename (inner.bas);
' C uses the current active filename at error-emit time (outer.bas).
'
' Same shape as the just-closed label lineno-of-first-reference fix
' (compiler.c:1657 commit ff42d044) — port to filename too.

#line 1 "outer.bas"
#line 1 "inner.bas"

declare sub Foo(x as ubyte)

Foo(1)

#line 8 "outer.bas"
