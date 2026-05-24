#!/usr/bin/env python3
# ply_tokdump.py — dump the REAL zxbc lexer's token stream for a .bas file as
# "<id> <type> <lineno> <value-repr>" lines, using the SAME terminal id
# convention as gen_ply_tables.py (id == index in zxblex.tokens).
#
# Used to (a) feed the C ply_engine trace harness a token-type stream that is
# byte-identical to Python's, isolating engine-loop validation from lexer
# validation; and (b) later, as the oracle for the C lexer's own parity.
#
# Usage: ply_tokdump.py <file.bas>

import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from src.zxbc import zxbparser, zxblex  # noqa: E402
from src.zxbc.zxblex import tokens as TOKENS  # noqa: E402

ID = {t: i for i, t in enumerate(TOKENS)}


def main():
    path = sys.argv[1]
    with open(path) as f:
        src = f.read()
    zxbparser.init()
    lexer = zxblex.lexer
    lexer.input(src)
    out = []
    while True:
        tok = lexer.token()
        if not tok:
            break
        tid = ID.get(tok.type, -1)
        out.append("%d\t%s\t%d\t%r" % (tid, tok.type, tok.lineno, tok.value))
    out.append("%d\t$end\t%d\t" % (len(TOKENS), lexer.lineno))
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
