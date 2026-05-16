#!/usr/bin/env python3
"""Shared substrate for Phase 1 semantic-fidelity probes.

A probe extracts ONE named semantic property from both the Python
reference and the C port and compares them. The Python side walks the
live ``src.zxbc.zxbparser`` parse tree (full attribute visibility); the
C side parses ``zxbc-ast-dump`` JSON (tag visibility). Neither the
shared dump_python_ast.py <-> zxbc-ast-dump JSON schema nor any
production code is modified — probes are narrow read-only consumers.

Determinism: each probe is its own process; ``python_root`` calls
``zxbparser.init()` once before parsing, matching the proven
dump_python_ast.py one-subprocess-per-fixture discipline.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys


def _project_root() -> str:
    # csrc/tests/probe_common.py -> repo root (three dirnames up).
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def python_root(bas_path: str):
    """Parse a .bas via the live Python reference. Returns the root
    Symbol, or None on a Python parse error (the caller buckets that)."""
    root = _project_root()
    if root not in sys.path:
        sys.path.insert(0, root)
    from src.zxbc import zxbparser  # noqa: E402 (project-relative, self-bootstrapped)

    zxbparser.init()
    with open(bas_path, encoding="utf-8") as fh:
        src = fh.read()
    return zxbparser.parser.parse(src, lexer=zxbparser.zxblex.lexer, debug=False)


def python_root_after_passes(bas_path: str, opt_level: int = 0):
    """Parse a .bas, then run Python's post-parse passes 1-3 in
    src/zxbc/zxbc.py order (Unreachable -> FunctionGraph -> Optimizer;
    zxbc.py:107-141), returning the in-place-mutated AST root. Read-only
    consumer of the reference Python — imports the passes, never edits
    `src/`. This is the "Python AST after passes 1-3" oracle for the
    Phase-2 post-pass differential probes (wired against the C visitor
    passes as they land in S2.2-S2.4); the Phase-1 probes keep using
    `python_root` (raw post-parse tree).

    `opt_level` sets OPTIONS.optimization_level before the optimizer
    (OptimizerVisitor is gated: O_LEVEL < 1 returns nodes unchanged,
    src/api/optimize.py:201-205). Returns None if the parse errored.
    """
    root = _project_root()
    if root not in sys.path:
        sys.path.insert(0, root)
    from src.zxbc import zxbparser  # noqa: E402

    zxbparser.init()
    with open(bas_path, encoding="utf-8") as fh:
        src = fh.read()
    ast = zxbparser.parser.parse(
        src, lexer=zxbparser.zxblex.lexer, debug=False
    )
    if ast is None:
        return None

    from src.api.config import OPTIONS  # noqa: E402
    import src.api.optimize as optimize  # noqa: E402

    OPTIONS.optimization_level = opt_level
    optimize.UnreachableCodeVisitor().visit(zxbparser.ast)  # pass 1
    optimize.FunctionGraphVisitor().visit(zxbparser.ast)    # pass 2
    optimize.OptimizerVisitor().visit(zxbparser.ast)        # pass 3
    return zxbparser.ast


def py_token(node) -> str:
    return getattr(node, "token", type(node).__name__)


def py_type(node):
    """The node's resolved type name, or None — mirrors the dump's
    _stringify_type so it lines up with the C side's attrs.type."""
    t = getattr(node, "type_", None)
    if t is None:
        return None
    name = getattr(t, "name", None)
    return str(name) if name is not None else repr(t)


def py_children(node) -> list:
    return list(getattr(node, "children", None) or [])


def py_walk(node):
    if node is None:
        return
    yield node
    for child in py_children(node):
        yield from py_walk(child)


def py_find_first(root, token):
    for node in py_walk(root):
        if py_token(node) == token:
            return node
    return None


def c_root(bas_path: str, dump_bin: str):
    """Run zxbc-ast-dump on a .bas; return the parsed JSON root.
    Raises RuntimeError on a C parse error (non-zero exit)."""
    proc = subprocess.run([dump_bin, bas_path], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"zxbc-ast-dump exit {proc.returncode} (C parse error): "
            f"{proc.stderr.strip()}"
        )
    return json.loads(proc.stdout)


def c_children(node) -> list:
    return list(node.get("children") or [])


def c_type(node):
    return (node.get("attrs") or {}).get("type")


def c_walk(node):
    if node is None:
        return
    yield node
    for child in c_children(node):
        yield from c_walk(child)


def c_find_first(root, tag):
    for node in c_walk(root):
        if node.get("tag") == tag:
            return node
    return None


def emit(construct: str, fixture: str, prop: str, py_val, c_val) -> int:
    """Print the single result line and return the probe exit code:
    0 == property matches Python (MATCH), 1 == drift (MISMATCH)."""
    match = py_val == c_val
    status = "MATCH" if match else "MISMATCH"
    print(
        f"{construct} {os.path.basename(fixture)} :: {prop} :: "
        f"py={py_val!r} c={c_val!r} :: {status}"
    )
    return 0 if match else 1
