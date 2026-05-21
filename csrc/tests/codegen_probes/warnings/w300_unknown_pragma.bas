' W300: a #pragma naming an unknown OPTIONS attribute is ignored with a warning.
' zxbparser p_preproc_line_pragma_option -> UndefinedOptionError -> warning_ignoring_unknown_pragma.
#pragma zzznotanoption = 1
Print 1
