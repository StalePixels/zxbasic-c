Class: w520-whitespace-quirk (DEFERRED-PYTHON-SIDE)
Corpus rows: zen, zen-ii (secondary divergence)
A function-like `#define NAME(p)  body` with 2+ spaces (or 2+ tabs) after `)`: Python spuriously emits W520 "missing whitespace after macro name" because zxbpp.py:553 tests `defs[0] in " \t"` (substring membership), and "  " is not a substring of " \t". C correctly does not warn. Matching this means porting a Python bug.
