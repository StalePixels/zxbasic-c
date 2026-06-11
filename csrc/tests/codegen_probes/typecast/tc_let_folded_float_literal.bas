' LET-assignment companion to arithmetic/ar_fold_float_literal_retype_fixed:
' the same folded-constant retype divergence through the implicit-typed
' LET path rather than direct PRINT of the fold.
'
' Python types literal 3.14 as FIXED (src/symbols/number.py:40-44,
' float value in (-32768.0, 32767) -> TYPE.fixed), folds 3.14 * 2 at
' common_type fixed with f16 quantization, and re-derives the folded
' SymbolNUMBER's type from the value -> FIXED. The divergence then
' surfaces BEFORE codegen, in the W100 implicit-type warning:
'   Py: warning: [W100] Using default implicit type 'fixed' for 'x'
'   C : warning: [W100] Using default implicit type 'float' for 'x'
' i.e. Python derives x's implicit type from the folded RHS (fixed);
' the C port's fold yields float and x follows it (PROBE-DIFF-STDERR;
' the ASM constant bytes diverge behind it too).
LET x = 3.14 * 2
PRINT x
