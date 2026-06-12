' RED probe — LF variant of the asm `;`-comment continuation-char
' bug.  Latent pre-existing: even on plain-LF source the comment-blind
' C join fired inside a `;` asm comment ending in `\`, eating the next
' defb line.  Python never joins (asm COMMENT `;.*` eats the marker).
asm
    defb 1   ; \
    defb 2   ; ]
    defb 3   ; _
    defb 4   ; UK Pound
end asm
