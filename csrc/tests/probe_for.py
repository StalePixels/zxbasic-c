#!/usr/bin/env python3
"""probe_for — FOR bound-expr TYPECAST-wrapper presence (Phase 1).

ONE named property: the number of TYPECAST nodes that are *direct
children* of the SENTENCE("FOR") node. Python's p_for_sentence_start
wraps start/end/step in make_typecast(variable.type_, ...)
(zxbparser.py:1620-1622); the C port (parser.c:1878-1886) adds the raw
expressions with no TYPECAST. Scoped to the FOR sentence's own children
so an unrelated TYPECAST elsewhere cannot confound the meter.

This is the Phase 1 named calibration. Exit 0 MATCH, 1 MISMATCH
(drift), 2 error.
"""

import sys

import probe_common as pc


def main(argv):
    if len(argv) != 3:
        print("usage: probe_for.py <fixture.bas> <zxbc-ast-dump-bin>", file=sys.stderr)
        return 2
    bas, dump_bin = argv[1], argv[2]
    try:
        pyr = pc.python_root(bas)
        if pyr is None:
            print(f"FOR {bas} :: python parse error", file=sys.stderr)
            return 2
        cr = pc.c_root(bas, dump_bin)
    except Exception as exc:  # C parse error / IO — bucketed by harness
        print(f"FOR {bas} :: error: {exc}", file=sys.stderr)
        return 2

    py_for = pc.py_find_first(pyr, "FOR")
    c_for = pc.c_find_first(cr, "FOR")
    if py_for is None or c_for is None:
        print(
            f"FOR {bas} :: FOR sentence not found "
            f"(py={py_for is not None} c={c_for is not None})",
            file=sys.stderr,
        )
        return 2

    py_tc = sum(1 for ch in pc.py_children(py_for) if pc.py_token(ch) == "TYPECAST")
    c_tc = sum(1 for ch in pc.c_children(c_for) if ch.get("tag") == "TYPECAST")
    return pc.emit("FOR", bas, "direct TYPECAST children of FOR", py_tc, c_tc)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
