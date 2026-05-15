#!/usr/bin/env python3
"""probe_dim_scalar — scalar-DIM VARDECL tree-placement (Phase 1).

ONE named property: the number of VARDECL nodes present in the parsed
tree for a bare scalar `DIM ... AS type` program. Python's p_var_decl
returns p[0]=None (zxbparser.py:652) — declarations are drained into a
trailing data_ast block built *after* parser.parse(), so the parsed
tree carries ZERO VARDECL. The C port emits VARDECL nodes INLINE at the
statement site (parser.c:2162-2208).

Targets *placement* (presence in the parse tree), NOT per-name
cardinality — the re-audit found C's one-VARDECL-per-name count already
faithful, so a count-based probe would be tautologically green. Exit 0
MATCH, 1 MISMATCH (drift), 2 error.
"""

import sys

import probe_common as pc


def main(argv):
    if len(argv) != 3:
        print(
            "usage: probe_dim_scalar.py <fixture.bas> <zxbc-ast-dump-bin>",
            file=sys.stderr,
        )
        return 2
    bas, dump_bin = argv[1], argv[2]
    try:
        pyr = pc.python_root(bas)
        if pyr is None:
            print(f"DIM {bas} :: python parse error", file=sys.stderr)
            return 2
        cr = pc.c_root(bas, dump_bin)
    except Exception as exc:
        print(f"DIM {bas} :: error: {exc}", file=sys.stderr)
        return 2

    py_vardecl = sum(1 for n in pc.py_walk(pyr) if pc.py_token(n) == "VARDECL")
    c_vardecl = sum(1 for n in pc.c_walk(cr) if n.get("tag") == "VARDECL")
    return pc.emit(
        "DIM", bas, "VARDECL nodes in parsed tree", py_vardecl, c_vardecl
    )


if __name__ == "__main__":
    sys.exit(main(sys.argv))
