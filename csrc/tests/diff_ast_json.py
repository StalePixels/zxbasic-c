#!/usr/bin/env python3
"""Structural diff between two AST JSON dumps.

Inputs are produced by `dump_python_ast.py` (Python side) and
`zxbc-ast-dump` (C side, Sprint 8). Schema:

    {"tag": str, "line": int|null, "attrs": {...}, "children": [...]}

Output is a human-readable diff on stdout — one line per difference
site, with a structural path indicator like `root.children[1].attrs.type`,
so a reviewer can find the divergence without re-parsing both files.
The last stdout line is always either `EQUAL` or `DIFF: <count> sites`,
machine-greppable for the harness.

Exit codes:
    0  — EQUAL or DIFF emitted (the harness inspects the summary line)
    2  — argv / file-load error

Diff conservatism: missing children, extra children, and child reorderings
all produce diff sites at the parent level (not flattened recursively),
so a 5-extra-children divergence reads as one site, not 5.
"""

from __future__ import annotations

import json
import sys


def _walk(py, c, path: str, sites: list[str]) -> None:
    if py is None and c is None:
        return
    if py is None:
        sites.append(f"{path}: MISSING in python (C has tag={c.get('tag')!r})")
        return
    if c is None:
        sites.append(f"{path}: MISSING in c (python has tag={py.get('tag')!r})")
        return

    py_tag = py.get("tag")
    c_tag = c.get("tag")
    if py_tag != c_tag:
        sites.append(f"{path}.tag: {py_tag!r} (py) vs {c_tag!r} (c)")
        # Different tags — children comparison would just produce noise.
        return

    py_line = py.get("line")
    c_line = c.get("line")
    if py_line != c_line:
        sites.append(f"{path}.line: {py_line!r} (py) vs {c_line!r} (c)")

    py_attrs = py.get("attrs") or {}
    c_attrs = c.get("attrs") or {}
    for key in sorted(set(py_attrs) | set(c_attrs)):
        if py_attrs.get(key) != c_attrs.get(key):
            sites.append(
                f"{path}.attrs.{key}: {py_attrs.get(key)!r} (py) vs {c_attrs.get(key)!r} (c)"
            )

    py_kids = py.get("children") or []
    c_kids = c.get("children") or []
    if len(py_kids) != len(c_kids):
        sites.append(
            f"{path}.children: count {len(py_kids)} (py) vs {len(c_kids)} (c)"
        )
    for i, (pk, ck) in enumerate(zip(py_kids, c_kids)):
        _walk(pk, ck, f"{path}.children[{i}]", sites)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: diff_ast_json.py <py_ast.json> <c_ast.json>", file=sys.stderr)
        return 2

    try:
        with open(argv[1]) as fh:
            py = json.load(fh)
        with open(argv[2]) as fh:
            c = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"diff_ast_json: {exc}", file=sys.stderr)
        return 2

    sites: list[str] = []
    _walk(py, c, "root", sites)

    if not sites:
        print("EQUAL")
        return 0

    for s in sites:
        print(s)
    print(f"DIFF: {len(sites)} sites")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
