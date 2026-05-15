#!/usr/bin/env python3
"""Dump Python zxbc AST for a single .bas file as JSON.

Read-only consumer of `src.zxbc.zxbparser` — does not modify upstream
Python source. Designed for byte-level structural comparison against
the C port's AST dump (csrc/zxbc-ast-dump, Sprint 8).

Schema:
    {
      "tag":      "<NODE_KIND>",        # from Symbol.token (e.g. "BLOCK", "BINARY")
      "line":     <int|null>,           # from Symbol.lineno if present
      "attrs":    {"name": "...",       # if Symbol.name is present and stringifiable
                   "type": "..."},      # if Symbol.type_ is present, stringified
      "children": [<recursive>]         # non-None children only
    }

Volatile/codegen-only attributes (`.t` temp names, `._required_by`,
`._requires`, `parent`, etc.) are deliberately excluded — they vary
across parse runs even on identical input and would force every diff
to FAIL on harmless implementation detail.

Exit codes:
    0  — JSON dump emitted to stdout
    1  — Python parser raised; stderr carries the diagnostic
    2  — argv / file-load error
"""

from __future__ import annotations

import json
import os
import sys


def _stringify_type(value):
    """Render a type-like attribute (TYPEREF / BASICTYPE) compactly.

    Keeps the wire format stable across parse runs. Falls back to repr()
    for unknown shapes; the C side has the same fallback responsibility.
    """
    if value is None:
        return None
    name = getattr(value, "name", None)
    if name is not None:
        return str(name)
    return repr(value)


def _build_attrs(node):
    attrs = {}
    name = getattr(node, "name", None)
    if isinstance(name, str) and name:
        attrs["name"] = name
    type_ = getattr(node, "type_", None)
    if type_ is not None:
        attrs["type"] = _stringify_type(type_)
    return attrs


def _serialise(node):
    if node is None:
        return None
    payload = {
        "tag": getattr(node, "token", type(node).__name__),
        "line": getattr(node, "lineno", None),
        "attrs": _build_attrs(node),
        "children": [
            child for child in (_serialise(c) for c in (node.children or [])) if child is not None
        ],
    }
    return payload


def _parse(bas_path: str):
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if project_root not in sys.path:
        sys.path.insert(0, project_root)

    from src.zxbc import zxbparser  # noqa: E402  (project-relative import)

    zxbparser.init()
    with open(bas_path, encoding="utf-8") as fh:
        src = fh.read()
    return zxbparser.parser.parse(src, lexer=zxbparser.zxblex.lexer, debug=False)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: dump_python_ast.py <path/to/file.bas>", file=sys.stderr)
        return 2

    bas_path = argv[1]
    if not os.path.isfile(bas_path):
        print(f"dump_python_ast: not a file: {bas_path}", file=sys.stderr)
        return 2

    ast = _parse(bas_path)
    if ast is None:
        print("dump_python_ast: parser returned None (parse error)", file=sys.stderr)
        return 1
    json.dump(_serialise(ast), sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
