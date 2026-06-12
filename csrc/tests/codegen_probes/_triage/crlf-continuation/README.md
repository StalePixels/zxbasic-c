Class: crlf-continuation (MATTERS)
Corpus rows: knights-demons-dx (secondary divergence)
NOTE: crlf_define.bas MUST keep CRLF line endings (.gitattributes `*.bas -crlf` preserves them). A `\` line-continuation followed by CRLF: Python's lexer rule `[\\_]\r?\n` accepts it; the C preprocessor checks the last char before `\n` which is `\r`, so the `\` is left in the line and errors `illegal preprocessor character '\'`.
