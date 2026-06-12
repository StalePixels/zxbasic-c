Class: parser-recovery-cascade (MATTERS)
Corpus rows: souls, berksman, looking-for-csscgc2012, 3-reyes-magos, abydos
After a syntax error inside a SUB/FUNCTION, PLY's `function_declaration : function_header program_co END error` production fires an extra "Unexpected token 'END'. Expected 'END FUNCTION' or 'END SUB' instead." that the C hand-written recovery omits; the cascade also drifts the "Too many errors" cap line (3-reyes-magos, abydos).
