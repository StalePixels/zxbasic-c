# --------------------------------------------------------------------
# SPDX-License-Identifier: AGPL-3.0-or-later
# © Copyright 2008-2024 José Manuel Rodríguez de la Rosa and contributors.
# See the file CONTRIBUTORS.md for copyright details.
# See https://www.gnu.org/licenses/agpl-3.0.html for details.
# --------------------------------------------------------------------

import re
import sys
from collections.abc import Callable
from typing import Any


class LexError(Exception):
    pass


class LexToken:
    def __init__(self) -> None:
        self.type: str | None = None
        self.value: Any = None
        self.lineno: int = 1
        self.lexpos: int = 0
        self.lexer: Lexer | None = None

    def __repr__(self) -> str:
        return f"LexToken({self.type},{self.value!r},{self.lineno},{self.lexpos})"


class Lexer:
    def __init__(self, obj: Any) -> None:
        self._object = obj
        self.lexdata: str = ""
        self.lexpos: int = 0
        self.lineno: int = 1
        self.statestack: list[str] = ["INITIAL"]
        self.next_token: LexToken | None = None

        # Parse states
        states = get_attr(obj, "states", ())
        state_names = {"INITIAL"}
        for s in states:
            state_names.add(s[0])

        # Parse tokens
        self.tokens = get_attr(obj, "tokens", ())

        # Collect rules
        functions: list[tuple[str, list[str], str, str, Callable[[LexToken], Any]]] = []
        strings: list[tuple[str, list[str], str, str]] = []
        self.error_handlers: dict[str, Callable[[LexToken], Any]] = {}

        # dir(obj) preserves definition order in modern Python,
        # but we want to be safe and sort functions by co_firstlineno
        for name in get_dir(obj):
            if name.startswith("t_"):
                val = get_attr(obj, name)
                if name.endswith("_error"):
                    parts = name[2:-6].split("_")
                    target_states = [p for p in parts if p in state_names]
                    if not target_states:
                        target_states = ["INITIAL"]
                    for s in target_states:
                        self.error_handlers[s] = val
                else:
                    parts = name[2:].split("_")
                    target_states = []
                    i = 0
                    # Consume state names from the left, leaving at least one part for the rule name
                    while i < len(parts) - 1 and parts[i] in state_names:
                        target_states.append(parts[i])
                        i += 1

                    if not target_states:
                        target_states = ["INITIAL"]
                        rule_name = "_".join(parts)
                    else:
                        rule_name = "_".join(parts[i:])

                    if callable(val):
                        pattern = val.__doc__
                        if pattern:
                            functions.append((name, target_states, rule_name, pattern, val))
                    elif isinstance(val, str):
                        strings.append((name, target_states, rule_name, val))

        # Sort functions by co_firstlineno to match definition order in file
        def get_line_no(item: tuple[str, list[str], str, str, Callable[[LexToken], Any]]) -> int:
            func = item[4]
            try:
                if hasattr(func, "__code__"):
                    return func.__code__.co_firstlineno
                if hasattr(func, "__func__") and hasattr(func.__func__, "__code__"):
                    return func.__func__.__code__.co_firstlineno
            except Exception:
                pass
            return 0

        functions.sort(key=get_line_no)

        # Sort strings by pattern length descending
        strings.sort(key=lambda x: len(x[3]), reverse=True)

        # Build rules per state
        state_rules: dict[str, list[tuple[str, re.Pattern[str], Callable[[LexToken], Any] | None]]] = {
            s: [] for s in state_names
        }

        # Add functions first
        for name, target_states, rule_name, pattern, func in functions:
            try:
                rx = re.compile(pattern)
                for s in target_states:
                    state_rules[s].append((rule_name, rx, func))
            except Exception as e:
                print(f"Error compiling pattern {pattern!r} for rule {name}: {e}", file=sys.stderr)
                raise e

        # Add strings second
        for name, target_states, rule_name, pattern in strings:
            try:
                rx = re.compile(pattern)
                for s in target_states:
                    state_rules[s].append((rule_name, rx, None))
            except Exception as e:
                print(f"Error compiling pattern {pattern!r} for rule {name}: {e}", file=sys.stderr)
                raise e

        self.compiled_rules = state_rules

    def input(self, data: str) -> None:
        self.lexdata = data
        self.lexpos = 0
        self.lineno = 1
        self.statestack = ["INITIAL"]
        self.next_token = None

    def begin(self, state: str) -> None:
        if state not in self.compiled_rules:
            raise LexError(f"Undefined state: {state}")
        self.statestack[-1] = state

    def push_state(self, state: str) -> None:
        if state not in self.compiled_rules:
            raise LexError(f"Undefined state: {state}")
        self.statestack.append(state)

    def pop_state(self) -> None:
        if len(self.statestack) > 1:
            self.statestack.pop()
        else:
            self.statestack = ["INITIAL"]

    def skip(self, n: int) -> None:
        self.lexpos += n

    def clone(self) -> Lexer:
        c = Lexer(self._object)
        c.lexdata = self.lexdata
        c.lexpos = self.lexpos
        c.lineno = self.lineno
        c.statestack = list(self.statestack)
        c.next_token = self.next_token
        return c

    def token(self) -> LexToken | None:
        if self.next_token is not None:
            t = self.next_token
            self.next_token = None
            return t

        while self.lexpos < len(self.lexdata):
            state = self.statestack[-1]
            rules = self.compiled_rules.get(state, [])

            matched = False
            for rule_name, rx, val in rules:
                m = rx.match(self.lexdata, self.lexpos)
                if m:
                    matched = True
                    matched_text = m.group(0)

                    t = LexToken()
                    t.type = rule_name
                    t.value = matched_text
                    t.lineno = self.lineno
                    t.lexpos = self.lexpos
                    t.lexer = self

                    # Advance position BEFORE calling function
                    self.lexpos += len(matched_text)

                    if val:
                        res = val(t)
                        if res is None:
                            # Ignored token
                            break
                        return res
                    # Convert to token type (which is rule_name)
                    return t

            if matched:
                continue

            # No rule matched
            err_handler = self.error_handlers.get(state)
            if err_handler:
                t = LexToken()
                t.type = "error"
                t.value = self.lexdata[self.lexpos :]
                t.lineno = self.lineno
                t.lexpos = self.lexpos
                t.lexer = self

                old_pos = self.lexpos
                res = err_handler(t)
                if self.lexpos == old_pos:
                    self.lexpos += 1
                if res is not None:
                    return res
            else:
                raise LexError(
                    f"Lexical error: Illegal character {self.lexdata[self.lexpos]!r} at position {self.lexpos}"
                )

        return None


def get_attr(obj: Any, name: str, default: Any = None) -> Any:
    if isinstance(obj, dict):
        return obj.get(name, default)
    return getattr(obj, name, default)


def get_dir(obj: Any) -> list[str]:
    if isinstance(obj, dict):
        return list(obj.keys())
    return dir(obj)


def lex(object: Any = None) -> Lexer:
    if object is None:
        frame = sys._getframe(1)
        object = frame.f_globals
    return Lexer(object)
