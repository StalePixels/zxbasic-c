/*
 * ply_engine.c — C port of PLY's LALR(1) parse engine.
 *
 * Faithful line-for-line port of src/ply/yacc.py LRParser.parse() (the
 * non-tracking, non-debug path). Branch comments cite the yacc.py line that
 * each block reproduces (line numbers from the pinned src/ply/yacc.py).
 *
 * Table lookup: PLY does `actions[state].get(ltype)` / `goto[s][pname]`,
 * i.e. a dict get returning None when absent. We binary-search the per-state
 * sorted (sym,val) row; "absent" == lookup miss == PLY's None.
 */
#include "ply_engine.h"
#include <stdlib.h>
#include <string.h>

#define PLY_ERROR_RECOVERY_COUNT 3 /* yacc.py:79 error_count = 3 */

/* dict.get over a sorted PlyRow. Returns found via *out; result is whether
 * the key was present (PLY: value is not None). */
static bool row_get(const PlyRow *row, int sym, int *out) {
    int lo = 0, hi = row->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int s = row->ents[mid].sym;
        if (s == sym) {
            *out = row->ents[mid].val;
            return true;
        }
        if (s < sym)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

/* ---- parallel parse stacks (statestack ints / symstack PlySym) ---- */
typedef struct {
    int *states;
    int nstates, cap_states;
    PlySym *syms;
    int nsyms, cap_syms;
} Stacks;

static void stk_init(Stacks *s) {
    s->cap_states = 256;
    s->states = malloc(sizeof(int) * s->cap_states);
    s->nstates = 0;
    s->cap_syms = 256;
    s->syms = malloc(sizeof(PlySym) * s->cap_syms);
    s->nsyms = 0;
}
static void stk_free(Stacks *s) {
    free(s->states);
    free(s->syms);
}
static void push_state(Stacks *s, int st) {
    if (s->nstates == s->cap_states) {
        s->cap_states *= 2;
        s->states = realloc(s->states, sizeof(int) * s->cap_states);
    }
    s->states[s->nstates++] = st;
}
static void push_sym(Stacks *s, PlySym sym) {
    if (s->nsyms == s->cap_syms) {
        s->cap_syms *= 2;
        s->syms = realloc(s->syms, sizeof(PlySym) * s->cap_syms);
    }
    s->syms[s->nsyms++] = sym;
}

void ply_parser_init(PlyParser *p, PlyLexFn lex, PlyActionFn action,
                     PlyErrorFn error, void *ud) {
    memset(p, 0, sizeof(*p));
    p->lex = lex;
    p->action = action;
    p->error = error;
    p->ud = ud;
    p->errorok = false;
    p->errok_token_set = false;
    p->cur_lineno = 1;
}

/* PLY errok(): set self.errorok = True and stash the next lookahead. The
 * engine's main loop reads errorok right after calling errorfunc. */
void ply_errok(PlyParser *p, const PlySym *next_lookahead) {
    p->errorok = true;
    if (next_lookahead) {
        p->errok_token = *next_lookahead;
        p->errok_token_set = true;
    } else {
        p->errok_token_set = false;
    }
}

void *ply_parse(PlyParser *p) {
    Stacks stk;
    stk_init(&stk);

    /* yacc.py:278-285 lookahead / lookaheadstack / errorcount */
    bool have_lookahead = false;
    PlySym lookahead;
    memset(&lookahead, 0, sizeof(lookahead));

    /* lookaheadstack: PLY pushes the saved lookahead during error recovery.
     * It never grows beyond a couple entries here. */
    PlySym lookaheadstack[8];
    int laksp = 0;

    int errorcount = 0;

    /* yacc.py:314-318 start state (0, $end) */
    push_state(&stk, 0);
    {
        PlySym end;
        end.type = PLY_END_ID;
        end.lineno = 0;
        end.value = NULL;
        push_sym(&stk, end);
    }
    int state = 0;

    void *retval = NULL;

    while (1) { /* yacc.py:319 while True */
        int t;
        bool have_t;

        /* yacc.py:327 if state not in defaulted_states */
        int def = ply_defaulted[state];
        if (def == 0) {
            /* yacc.py:328-336 get next token if no lookahead */
            if (!have_lookahead) {
                if (laksp > 0) {
                    lookahead = lookaheadstack[--laksp];
                    have_lookahead = true;
                } else {
                    PlySym tok;
                    memset(&tok, 0, sizeof(tok));
                    if (p->lex(p->ud, &tok)) {
                        lookahead = tok;
                        have_lookahead = true;
                    } else {
                        /* yacc.py:333-335 EOF -> $end */
                        lookahead.type = PLY_END_ID;
                        lookahead.lineno = p->cur_lineno;
                        lookahead.value = NULL;
                        have_lookahead = true;
                    }
                }
            }
            /* yacc.py:338-339 t = actions[state].get(ltype) */
            have_t = row_get(&ply_action[state], lookahead.type, &t);
        } else {
            /* yacc.py:341 t = defaulted_states[state] (a reduce: negative) */
            t = def;
            have_t = true;
        }

        if (have_t) {
            if (t > 0) {
                /* yacc.py:350-364 shift */
                if (p->trace) p->trace(p->ud, 'S', t);
                push_state(&stk, t);
                state = t;
                push_sym(&stk, lookahead);
                have_lookahead = false;
                if (errorcount)
                    errorcount--;
                continue;
            }
            if (t < 0) {
                /* yacc.py:366-465 reduce by production -t */
                if (p->trace) p->trace(p->ud, 'R', -t);
                int prodno = -t;
                const PlyProd *pr = &ply_prod[prodno];
                int plen = pr->len;
                int pname = pr->name_sym;

                PlySym sym;
                sym.type = pname;
                sym.lineno = 0;
                sym.value = NULL;

                bool action_ok = true;
                void *result_value = NULL;
                int result_lineno = 0;

                if (plen) {
                    /* yacc.py:386-428 plen>0
                     * targ = symstack[-plen-1:]; targ[0]=sym
                     * The RHS slice handed to the action is symstack[-plen:]
                     * (PLY's pslice indices 1..plen). pslice[0] is the result. */
                    PlySym *rhs = &stk.syms[stk.nsyms - plen];

                    /* PLY default lineno propagation: p.lineno(1) etc. read
                     * the RHS symbols' lineno; result lineno is set by the
                     * action. Default sym.lineno stays 0 unless action sets. */
                    if (pr->rule >= 0) {
                        action_ok = p->action(p->ud, prodno, rhs, plen,
                                               &result_value, &result_lineno);
                    } else {
                        /* No callable (augmented start S'->start): PLY just
                         * carries pslice[0] = None; here value stays NULL. */
                        action_ok = true;
                    }

                    if (action_ok) {
                        sym.value = result_value;
                        sym.lineno = result_lineno;
                        /* del symstack[-plen:]; del statestack[-plen:] */
                        stk.nsyms -= plen;
                        stk.nstates -= plen;
                        /* symstack.append(sym); state = goto[...][pname] */
                        push_sym(&stk, sym);
                        int g;
                        row_get(&ply_goto[stk.states[stk.nstates - 1]], pname, &g);
                        state = g;
                        push_state(&stk, state);
                    } else {
                        /* yacc.py:416-426 except SyntaxError (plen>0)
                         * lookaheadstack.append(lookahead)
                         * symstack.extend(targ[1:-1])
                         * statestack.pop(); state = statestack[-1]
                         * sym.type=error; lookahead=sym; errorcount=3 */
                        if (have_lookahead) {
                            lookaheadstack[laksp++] = lookahead;
                            have_lookahead = false;
                        }
                        /* targ[1:-1] == rhs[0 .. plen-2]; rhs[plen-1] dropped.
                         * symstack currently still holds the plen RHS syms
                         * (we did NOT delete them on the error path). PLY's
                         * symstack at this point is the pre-reduce stack with
                         * the plen syms removed (they were sliced into targ),
                         * then targ[1:-1] re-extended. Reproduce: remove the
                         * plen RHS syms, then push back the first plen-1. */
                        stk.nsyms -= plen;
                        for (int i = 0; i < plen - 1; i++)
                            push_sym(&stk, rhs[i]);
                        stk.nstates--; /* statestack.pop() */
                        state = stk.states[stk.nstates - 1];
                        sym.type = PLY_ERROR_ID;
                        sym.value = NULL;
                        lookahead = sym;
                        have_lookahead = true;
                        errorcount = PLY_ERROR_RECOVERY_COUNT;
                        p->errorok = false;
                    }
                    continue;
                } else {
                    /* yacc.py:430-465 plen==0 (epsilon production) */
                    if (pr->rule >= 0) {
                        action_ok = p->action(p->ud, prodno, NULL, 0,
                                               &result_value, &result_lineno);
                    } else {
                        action_ok = true;
                    }
                    if (action_ok) {
                        sym.value = result_value;
                        sym.lineno = result_lineno;
                        push_sym(&stk, sym);
                        int g;
                        row_get(&ply_goto[stk.states[stk.nstates - 1]], pname, &g);
                        state = g;
                        push_state(&stk, state);
                    } else {
                        /* yacc.py:454-463 except SyntaxError (plen==0)
                         * lookaheadstack.append(lookahead)
                         * statestack.pop(); state=statestack[-1]
                         * sym.type=error; lookahead=sym; errorcount=3 */
                        if (have_lookahead) {
                            lookaheadstack[laksp++] = lookahead;
                            have_lookahead = false;
                        }
                        stk.nstates--;
                        state = stk.states[stk.nstates - 1];
                        sym.type = PLY_ERROR_ID;
                        sym.value = NULL;
                        lookahead = sym;
                        have_lookahead = true;
                        errorcount = PLY_ERROR_RECOVERY_COUNT;
                        p->errorok = false;
                    }
                    continue;
                }
            }
            if (t == 0) {
                /* yacc.py:467-475 accept */
                if (p->trace) p->trace(p->ud, 'A', 0);
                PlySym *n = &stk.syms[stk.nsyms - 1];
                retval = n->value;
                goto done;
            }
        }

        /* yacc.py:477 if t is None — parsing error */
        {
            if (p->trace) p->trace(p->ud, 'E', lookahead.type);
            /* yacc.py:493-526 first-error handling */
            if (errorcount == 0 || p->errorok) {
                errorcount = PLY_ERROR_RECOVERY_COUNT;
                p->errorok = false;
                /* errtoken = lookahead; if $end -> None */
                PlySym errtoken_storage = lookahead;
                bool errtoken_is_eof = (lookahead.type == PLY_END_ID);
                p->errok_token_set = false;
                if (p->error) {
                    p->error(p->ud, errtoken_is_eof ? NULL : &errtoken_storage);
                    if (p->errorok) {
                        /* yacc.py:504-510 user did panic recovery; tok is the
                         * next lookahead. */
                        if (p->errok_token_set) {
                            lookahead = p->errok_token;
                            have_lookahead = true;
                        } else {
                            have_lookahead = false;
                        }
                        continue;
                    }
                }
            } else {
                /* yacc.py:525-526 */
                errorcount = PLY_ERROR_RECOVERY_COUNT;
            }

            /* yacc.py:532-538 case 1: statestack has 1 entry */
            if (stk.nstates <= 1 && lookahead.type != PLY_END_ID) {
                have_lookahead = false;
                state = 0;
                laksp = 0; /* nuke pushback stack */
                continue;
            }

            /* yacc.py:544-546 case 2: at EOF, bail */
            if (lookahead.type == PLY_END_ID) {
                retval = NULL;
                goto done;
            }

            /* yacc.py:548-577 */
            if (lookahead.type != PLY_ERROR_ID) {
                PlySym *top = &stk.syms[stk.nsyms - 1];
                if (top->type == PLY_ERROR_ID) {
                    /* yacc.py:550-557 error on top of stack: nuke input sym */
                    have_lookahead = false;
                    continue;
                }
                /* yacc.py:559-569 create error symbol as new lookahead */
                PlySym et;
                et.type = PLY_ERROR_ID;
                et.lineno = lookahead.lineno;
                et.value = NULL;
                /* lookaheadstack.append(lookahead); lookahead = et */
                lookaheadstack[laksp++] = lookahead;
                lookahead = et;
                have_lookahead = true;
            } else {
                /* yacc.py:570-576 lookahead is error: pop a symbol */
                stk.nsyms--;
                /* lookahead.lineno/lexpos = popped sym's (tracking only) */
                stk.nstates--;
                state = stk.states[stk.nstates - 1];
            }
            continue;
        }
    }

done:
    stk_free(&stk);
    return retval;
}
