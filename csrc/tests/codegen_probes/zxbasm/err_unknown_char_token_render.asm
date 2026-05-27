; RED PROBE — zxbasm lexer-to-parser token rendering for non-ASCII / unknown
; lexemes.
; Python emits: error: Syntax error. Unexpected token '~' [BXOR]
; C    emits  : error: Syntax error. Unexpected token '?' [14]
; Divergence  : Python's PLY lex maps '~' to the BXOR token (named token).
; C's lexer emits '?' as the token text and prints the numeric token id [14]
; instead of the canonical name. The probe pins both the token-text and the
; bracketed token-name (PLY port should walk a tokens-table by id).
    org 0x8000
    ~~~ broken ~~~
    ret
