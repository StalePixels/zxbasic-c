' Named CONST used in BOTH a narrow-typed (ubyte) call argument AND a
' wider-typed (float) binary expression. Python's SymbolTYPECAST.make_node
' (src/symbols/typecast.py:84-86) REPLACES the CONST id reference with a
' freshly-built SymbolNUMBER before mutating value/type, so the underlying
' shared CONST symbol-table node is never mutated. Each call site sees the
' CONST with its original (narrow) type.
'
' The C make_typecast (csrc/zxbc/compiler.c) previously mutated the shared
' CONST id's `type_` in-place. The first call site to widen the CONST in a
' binary common_type promotion (here: `plane1 + baseframe` where baseframe
' is float) permanently re-typed the CONST to float; any OTHER call site
' (here: `Foo(plane1)` as a ubyte argument) then emitted a 5-byte FIXED
' push instead of the 1-byte UBYTE push.
'
' Repro surfaced from NextBuild/Sources/Sprites/ScaleRotataSprite.bas
' (stage-02 P6, Δ+156): CONST plane1/plane2/plane3 used in both ubyte-param
' UpdateSprite() calls and float-arithmetic `plane1+baseframe` expressions.
SUB Foo(BYVAL p AS UBYTE)
END SUB

CONST plane1 = 0
DIM baseframe AS float
baseframe = 0
Foo(plane1)
Foo(plane1 + baseframe)
