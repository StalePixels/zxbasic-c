' Constant-folded arithmetic on a FLOAT-looking literal must re-derive
' the folded NUMBER's type from its VALUE, not keep/propagate float.
'
' Python: the literal 3.14 is typed FIXED at construction, because
' SymbolNUMBER.__init__ (src/symbols/number.py:40-44) types ANY float
' value in (-32768.0, 32767) as TYPE.fixed. common_type(fixed, ubyte)
' is fixed, both operands are TYPECAST to fixed (f16-quantizing the
' value), and the fold in SymbolBINARY.make_node
' (src/symbols/binary.py: `return SymbolNUMBER(func(a.value, b.value),
' type_=type_, ...)` with type_=None) re-derives the result type from
' the folded value -> FIXED again. PRINT therefore emits the 4-byte
' f16 constant (`ld de, 6 / ld hl, 18350`) and calls .core.__PRINTF16.
'
' C (csrc/zxbc): the folded constant comes out typed FLOAT instead --
' PRINT emits the 5-byte 40-bit FP constant and calls .core.__PRINTF,
' and the heap/__MEM_INIT runtime gets dragged in. Stage-1 ASM and the
' end-to-end binary both diverge (PROBE-DIFF-ASM).
'
' Found 2026-06-11 during user-verification ad-hoc sweep: a bare
' `PRINT 3.14 * 2` one-liner diverges while the whole inherited corpus
' and all 129 probes were GREEN -- corpus-silence class.
PRINT 3.14 * 2
