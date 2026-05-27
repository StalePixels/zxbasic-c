' RED probe -- inside an inline `asm…end asm` block, a `#ifdef DEBUG …
' #else … #endif` whose `#` directives are INDENTED (leading whitespace)
' is silently mishandled by the C preprocessor: it doesn't recognise the
' directive at all (because the first-pass `in_asm` branch dispatches via
' is_directive_asm() which requires column-1 `#`, while Python's BASIC-mode
' t_INITIAL_asm_sharp regex `[ \t]*\#` absorbs leading whitespace and still
' fires the directive — even inside the asm lexer-state, since that rule
' applies to both INITIAL and asm states).
'
' Effect: C emits the indented `#ifdef`/`#else`/`#endif` lines VERBATIM into
' the preprocessed stream, then the assembler later flags them as syntax
' errors (`Syntax error. Unexpected end of line`, `#else without matching
' #ifdef`). When DEBUG IS defined (LoadFromSD.bas hits this code path), the
' true-branch text reaches asm, AND the `#else jp loadsdout` body ALSO
' reaches asm — emitting +3 extra bytes of generated code.
'
' Python anchor: src/zxbpp/zxbpplex.py:350-357 t_INITIAL_asm_sharp regex
' `[ \t]*\#` + find_column(t) == 1 — the leading whitespace is part of the
' sharp-token's match, so the token's lexpos lands at start-of-line and
' find_column == 1 holds; the directive is dispatched normally.
'
' Affected NextBuild program (DEBUG defined → true branch hits):
'   - SDCardAccess/LoadFromSD.bas   Δ=+3 bytes (else-body `jp loadsdout`
'                                   leaks into output after true branch)
' Real source site: _ref/NextBuild/Scripts/nextlib.bas:1678 inside the
' `error:` block of the SDcard load routine.

#define DEBUG
asm
	#ifdef DEBUG
		ld a, 1
	#else
		jp loadsdout
	#endif
end asm
