' RED probe -- `#pragma push(case_insensitive)` / `#pragma pop(case_insensitive)`
' must save and restore the option value. C's parser.c case 374/375 treats
' the productions as a NOP, so a TRUE set between push and pop leaks past
' the pop and stays TRUE for the rest of the compilation unit.
'
' Python anchor: src/zxbc/zxbparser.py:3248-3263 p_preproc_pragma_push /
' p_preproc_pragma_pop call OPTIONS[name].push() / .pop() which use the
' Option.stack list from src/api/options.py:146-160 — push() saves the
' CURRENT value, pop() restores the previously-saved one.
'
' Real-world NextBuild Sources hit by this through #include <nextlib.bas>,
' which wraps its entire body in push(case_insensitive) / case_insensitive
' = TRUE / pop(case_insensitive). The pop must restore FALSE; if it
' doesn't, subsequent user code with a VAR/SUB case-difference (e.g.
' `dim dmaloop ...` then `SUB DMALoop(...)`) collides under the leaked-TRUE
' setting and the C port rejects with "'X' is a VAR, not a FUNCTION /
' SUB" where Python compiles cleanly.
'
' Affected NextBuild programs:
'   - DMAaudio/DMAPlay.bas:45  'DMALoop' is a VAR, not a FUNCTION
'   - Nextscii-tilemap/tm-netscii-2.bas:418  'Palette' is a VAR, not a FUNCTION

#pragma push(case_insensitive)
#pragma case_insensitive = TRUE
#pragma pop(case_insensitive)
dim dmaloop as ubyte
DMALoop(dmaloop)
sub DMALoop(byval v as ubyte)
end sub
