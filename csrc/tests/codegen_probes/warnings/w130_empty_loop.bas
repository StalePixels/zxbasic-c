' W130: empty DO ... LOOP UNTIL body. Non-constant condition (variable a)
' keeps W110 from also firing, so this isolates warning_empty_loop.
Dim a As Byte
a = 1
Do
Loop Until a
