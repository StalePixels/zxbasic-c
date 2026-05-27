; RED PROBE - zxbasm missing PLY-EOF footer.
;
; A preprocessor-character-only line (`#define @`) errors twice:
; (1) lexer's `illegal preprocessor character '@'`
; (2) parser's `Syntax error. Unexpected end of line` at the trailing
;     newline (no operand token after `define`).
;
; Python (src/zxbasm/asmparse.py:981-989) then ALSO emits:
;   `General syntax error at assembler (unexpected End of File?)`
; — PLY calls `p_error(None)` when the parse ends at EOF without the
; stack reaching the accept state. C currently never emits the footer.
#define @
