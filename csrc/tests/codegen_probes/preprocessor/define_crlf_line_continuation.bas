' RED probe — backslash line-continuation in a #DEFINE on CRLF source.
' Surfaced by knights-demons-dx, whose release ships CRLF line endings.
' Python's preprocessor continuation rule is r"[\\_]\r?\n" (zxbpplex.py),
' which tolerates the optional CR before the newline. The C preproc join
' loops tested the byte immediately before '\n' (the CR) for the backslash
' and so MISSED the continuation, leaking the trailing '\' into the token
' stream and emitting "illegal preprocessor character '\'" on lines 2+ of
' the macro body. Python accepts this form silently.
#define switchMusic() \
    asm                   \
        call 54721        \
    end asm
switchMusic()
