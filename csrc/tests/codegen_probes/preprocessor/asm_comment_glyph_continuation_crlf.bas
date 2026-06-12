' RED probe — asm `;`-comment whose described glyph is a
' continuation char (\ or _) must NOT line-continue.  This is the
' print42.bas font-table shape: a CRLF asm block where comment lines
' end in `\`+CRLF or `_`+CRLF.  Python's asm COMMENT rule `;.*`
' (zxbpplex.py:106) consumes the marker before t_asm_CONTINUE:100 can
' see it, so NO join happens; the next defb survives.  The C pre-token
' join (comment-blind) wrongly fired after d04b2112b made it CR-aware,
' eating the following defb and shifting every later #line down by one.
asm
    defb 1   ; \
    defb 2   ; ]
    defb 3   ; _
    defb 4   ; UK Pound
end asm
