#!/usr/bin/env python3
"""probe_binary — binary-operand coercion shape (Phase 1 regression guard).

ONE named property: the structural shape of the first BINARY subtree —
(tag, resolved-type, recursive child shapes). The re-audit found binary
operand coercion (constant fold + typecast-to-common-type) faithful at
HEAD; this probe asserts it STAYS faithful while Phase 1 fixes FOR/LET/
DIM. Scoped strictly to the BINARY subtree so the (unrelated) DIM-scalar
VARDECL-placement drift elsewhere in the tree cannot make this guard
spuriously red. Expected MATCH from day one. Exit 0 MATCH, 1 MISMATCH
(regression), 2 error.
"""

import sys

import probe_common as pc


def py_shape(node):
    return (
        pc.py_token(node),
        pc.py_type(node),
        tuple(py_shape(c) for c in pc.py_children(node)),
    )


def c_shape(node):
    return (
        node.get("tag"),
        pc.c_type(node),
        tuple(c_shape(c) for c in pc.c_children(node)),
    )


def main(argv):
    if len(argv) != 3:
        print(
            "usage: probe_binary.py <fixture.bas> <zxbc-ast-dump-bin>",
            file=sys.stderr,
        )
        return 2
    bas, dump_bin = argv[1], argv[2]
    try:
        pyr = pc.python_root(bas)
        if pyr is None:
            print(f"BINARY {bas} :: python parse error", file=sys.stderr)
            return 2
        cr = pc.c_root(bas, dump_bin)
    except Exception as exc:
        print(f"BINARY {bas} :: error: {exc}", file=sys.stderr)
        return 2

    py_bin = pc.py_find_first(pyr, "BINARY")
    c_bin = pc.c_find_first(cr, "BINARY")
    if py_bin is None or c_bin is None:
        print(
            f"BINARY {bas} :: BINARY node not found "
            f"(py={py_bin is not None} c={c_bin is not None})",
            file=sys.stderr,
        )
        return 2

    return pc.emit(
        "BINARY", bas, "BINARY subtree shape", py_shape(py_bin), c_shape(c_bin)
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
