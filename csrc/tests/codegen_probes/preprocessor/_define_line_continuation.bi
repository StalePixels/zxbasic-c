' Included by define_line_continuation_via_include.bas. Mirrors the
' multi-line #DEFINE pattern from nextlib.bas (TAB-indented, no space
' before the trailing `\`).

#DEFINE NextReg(REG,VAL) \
	ASM\
	DW $91ED\
	DB REG\
	DB VAL\
	END ASM
