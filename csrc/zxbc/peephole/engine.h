/*
 * engine.h — Port of src/arch/z80/peephole/engine.py + the M5 emitter
 * integration contract.
 *
 * Python module globals PATTERNS / MAXLEN become a module-static engine
 * state (no globals on the caller side; state passed nowhere by M5 —
 * Python's _output_join also relies on the module singleton). The arena
 * for persistent pattern allocation is threaded in at peephole_init().
 *
 * Backend.main.py:746-764 (_output_join) — the M5 caller — will:
 *     peephole.main()                    -> peephole_main()
 *     for i in range(len(asm)):
 *         while peephole.apply_match(asm, PATTERNS_<=lvl, i): ...
 * The exposed API is a faithful transcription target for that port:
 *   - peephole_main()/peephole_init()  : load + idempotent cache
 *   - peephole_maxlen()                : the global MAXLEN
 *   - peephole_pattern_count()/_level()/_pattlen() : iterate loaded
 *     patterns (level == OLEVEL) for the caller's level filtering
 *   - peephole_apply_match(asm, level_cap, index): try patterns with
 *     level <= level_cap at `index`, mutate StrVec in place, return
 *     whether it changed (== Python engine.apply_match over the
 *     level-filtered pattern list).
 */
#ifndef ZXBC_PEEPHOLE_ENGINE_H
#define ZXBC_PEEPHOLE_ENGINE_H

#include <stdbool.h>
#include "arena.h"
#include "backend.h"   /* StrVec = typedef VEC(char*) (pulls vec/arena/...) */

/* engine.init() : MAXLEN=0, PATTERNS.clear(). Binds the arena used for
 * all persistent pattern allocation (mirrors how csrc modules take an
 * Arena*). Safe to call repeatedly. */
void peephole_init(Arena *arena);

/* engine.main(force=False) : load+parse the embedded .opt table into
 * PATTERNS (stable-sorted by OFLAG, MAXLEN accumulated) unless already
 * loaded (cache on truthiness of MAXLEN). Must peephole_init() first
 * (the M5 port calls peephole_init() once with &cs->arena, then
 * peephole_main()). */
void peephole_main(void);
void peephole_main_force(void);   /* force=True */

int  peephole_maxlen(void);       /* global MAXLEN */
int  peephole_pattern_count(void);
int  peephole_pattern_level(int idx);   /* OLEVEL */
int  peephole_pattern_pattlen(int idx); /* len(pattern.patt) */
const char *peephole_pattern_fname(int idx);

/* engine.apply_match(asm_list, [p for p in PATTERNS if p.level<=level_cap],
 *                     index) — mutates `asm` in place; True iff changed. */
bool peephole_apply_match(StrVec *asm_, int level_cap, int index);

/* ---- pattern accessors for the O3 BasicBlock.optimize loop ----------
 * basicblock.py:434-496 runs its OWN match loop (NOT engine.apply_match)
 * because it must (a) use the CPU-state-monkey-patched evaluator and
 * (b) recompute the per-index match for the SAME pattern list filtered
 * by `OPTIONS.optimization_level >= p.level >= 3` (main.py:251). These
 * accessors expose the loaded OptPattern internals so the optimizer's
 * basicblock.c can replay that loop faithfully against the existing,
 * already-proven pattern/template/evaluator port. */
#include "pattern.h"
#include "template.h"
#include "evaluator.h"

int                  peephole_pattern_flag(int idx);   /* O_FLAG */
const BlockPattern  *peephole_pattern_patt(int idx);
const BlockTemplate *peephole_pattern_templ(int idx);
Ev                  *peephole_pattern_cond(int idx);
int                  peephole_pattern_ndefines(int idx);
const char          *peephole_pattern_define_var(int idx, int di);
Ev                  *peephole_pattern_define_expr(int idx, int di);

/* Internal smoke (no tests/ dependency): proves 028_o2 on the _end
 * sequence (`exx / pop hl / exx / pop iy / pop ix`) fires exactly twice
 * giving `exx / pop hl / pop iy / pop ix / exx`, and that
 * NEEDS("exx",["sp","iy"]) / NEEDS("exx",["sp","ix"]) are both false.
 * Returns 0 on success, non-zero (failed assertion id) otherwise. */
int peephole_selfcheck(void);

#endif /* ZXBC_PEEPHOLE_ENGINE_H */
