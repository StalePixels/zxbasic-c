#!/usr/bin/env python3
# ply_trace_ref.py — reference shift/reduce decision trace.
#
# Runs PLY's REAL parse engine over a .bas file using the REAL zxbc lexer,
# but wraps every production action with a no-op that records the decision
# trace. This isolates the loop's shift/reduce/goto decisions (the mechanism
# the C ply_engine must reproduce) from the AST-building action bodies.
#
# Output (stdout), one decision per line:
#   S <state>        shift, new state
#   R <prodno>       reduce by production
#   A                accept
#   E <tok>          error (p_error fired) at token type
#
# Usage: ply_trace_ref.py <file.bas>

import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from src.ply import yacc  # noqa: E402
from src.zxbc import zxbparser, zxblex  # noqa: E402


def main():
    path = sys.argv[1]
    with open(path) as f:
        src = f.read()

    parser = zxbparser.parser
    actions = parser.action
    goto = parser.goto
    prod = parser.productions
    defaulted = parser.defaulted_states

    zxbparser.init()
    lexer = zxblex.lexer
    lexer.input(src)
    get_token = lexer.token

    trace = []

    # Faithful copy of yacc.py parse() loop, but actions are no-ops (we only
    # record the decision sequence). Error recovery uses the same logic; the
    # 3 error-productions' SyntaxError behaviour is NOT reproduced here (no-op
    # actions never raise) — so this trace is the valid-input shift/reduce
    # skeleton, which is what we validate the C loop against first.
    statestack = [0]
    symstack = [type("S", (), {"type": "$end"})()]
    state = 0
    lookahead = None
    lookaheadstack = []
    errorcount = 0
    error_count = 3

    class Sym:
        __slots__ = ("type", "value", "lineno")

    while True:
        if state not in defaulted:
            if not lookahead:
                if not lookaheadstack:
                    lookahead = get_token()
                else:
                    lookahead = lookaheadstack.pop()
                if not lookahead:
                    lookahead = Sym()
                    lookahead.type = "$end"
                    lookahead.value = None
                    lookahead.lineno = lexer.lineno
            ltype = lookahead.type
            t = actions[state].get(ltype)
        else:
            t = defaulted[state]

        if t is not None:
            if t > 0:
                trace.append("S %d" % t)
                statestack.append(t)
                state = t
                symstack.append(lookahead)
                lookahead = None
                if errorcount:
                    errorcount -= 1
                continue
            if t < 0:
                trace.append("R %d" % (-t))
                p = prod[-t]
                pname = p.name
                plen = p.len
                sym = Sym()
                sym.type = pname
                sym.value = None
                sym.lineno = 0
                if plen:
                    del symstack[-plen:]
                    del statestack[-plen:]
                symstack.append(sym)
                state = goto[statestack[-1]][pname]
                statestack.append(state)
                continue
            if t == 0:
                trace.append("A")
                break

        # error
        et = lookahead.type if lookahead else "$end"
        trace.append("E %s" % et)
        # Mirror yacc.py's default-recovery skeleton (no error productions
        # fire under no-op actions, but the engine still nukes/recovers).
        if errorcount == 0:
            errorcount = error_count
            # would call p_error here
        else:
            errorcount = error_count
        if len(statestack) <= 1 and lookahead.type != "$end":
            lookahead = None
            state = 0
            del lookaheadstack[:]
            continue
        if lookahead.type == "$end":
            break
        if lookahead.type != "error":
            sym = symstack[-1]
            if getattr(sym, "type", None) == "error":
                lookahead = None
                continue
            tt = Sym()
            tt.type = "error"
            tt.value = lookahead
            tt.lineno = getattr(lookahead, "lineno", 0)
            lookaheadstack.append(lookahead)
            lookahead = tt
        else:
            symstack.pop()
            statestack.pop()
            state = statestack[-1]
        continue

    sys.stdout.write("\n".join(trace) + "\n")


if __name__ == "__main__":
    main()
