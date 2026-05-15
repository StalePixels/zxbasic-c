#!/usr/bin/env python3
"""probe_let — LET RHS TYPECAST-wrapper presence (Phase 1).

ONE named property: the number of TYPECAST nodes that are *direct
children* of the SENTENCE("LET") node. Python's p_assignment wraps the
RHS in make_typecast(variable.type_, ...) (zxbparser.py:1115) when the
RHS type differs from the lvalue type; the C port (parser.c:1516-1519)
adds the raw expression with no TYPECAST. The lvalue child is never a
TYPECAST, so a direct-TYPECAST-child count cleanly isolates the RHS
coercion property. Exit 0 MATCH, 1 MISMATCH (drift), 2 error.
"""

import sys

import probe_common as pc


def main(argv):
    if len(argv) != 3:
        print("usage: probe_let.py <fixture.bas> <zxbc-ast-dump-bin>", file=sys.stderr)
        return 2
    bas, dump_bin = argv[1], argv[2]
    try:
        pyr = pc.python_root(bas)
        if pyr is None:
            print(f"LET {bas} :: python parse error", file=sys.stderr)
            return 2
        cr = pc.c_root(bas, dump_bin)
    except Exception as exc:
        print(f"LET {bas} :: error: {exc}", file=sys.stderr)
        return 2

    py_let = pc.py_find_first(pyr, "LET")
    c_let = pc.c_find_first(cr, "LET")
    if py_let is None or c_let is None:
        print(
            f"LET {bas} :: LET sentence not found "
            f"(py={py_let is not None} c={c_let is not None})",
            file=sys.stderr,
        )
        return 2

    py_tc = sum(1 for ch in pc.py_children(py_let) if pc.py_token(ch) == "TYPECAST")
    c_tc = sum(1 for ch in pc.c_children(c_let) if ch.get("tag") == "TYPECAST")
    return pc.emit("LET", bas, "direct TYPECAST children of LET", py_tc, c_tc)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
