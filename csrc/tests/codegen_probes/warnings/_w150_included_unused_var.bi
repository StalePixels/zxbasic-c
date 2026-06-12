' Included by w150_include_filename_attribution.bas.
' Holds module-level variables whose [W150] "is never used" warnings must
' attribute to THIS file's name (not the including .bas). Two shapes, two
' distinct C emission sites:
'   - unusedInInclude  : declared, never referenced     -> W150 from the
'                        VAR-translation phase (var_translator.c VARDECL)
'   - assignedInInclude: assigned once, never READ back -> W150 from the
'                        optimizer LET-prune path (optimizer.c visit_LET).
' Both Python paths pass fname=entry.filename / lvalue.filename, so both
' must attribute to _w150_included_unused_var.bi here.
Dim unusedInInclude As UInteger
Dim assignedInInclude As UByte
assignedInInclude = 1
