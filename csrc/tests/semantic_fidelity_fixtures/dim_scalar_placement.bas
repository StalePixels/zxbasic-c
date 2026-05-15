' Phase 1 semantic-fidelity — DIM-scalar placement drifted control (S1.1).
'
' Bare scalar `DIM ... AS type` is p[0]=None in Python (zxbparser.py:652):
' no VARDECL appears in the parsed tree (declarations are drained into a
' trailing data_ast block built later, not in parser.parse()'s output).
' The C port emits VARDECL nodes INLINE at the statement site
' (parser.c:2162-2208). probe_dim_scalar asserts VARDECL count in the
' parsed tree matches Python (0); RED at S1.1, GREEN after the S1.2 fix.
' Targets placement (presence in the parse tree), NOT per-name count
' (which the re-audit found already faithful — would be tautological).

DIM a, b, c AS UBYTE
LET a = b
