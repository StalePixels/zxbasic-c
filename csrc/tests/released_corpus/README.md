# Released-program corpus — real-world Python-vs-C parity

A drop-in-equivalence meter that compiles **actual shipped ZX BASIC programs**
(games, demos, engines, utilities from
[zxbasic.readthedocs.io/released_programs](https://zxbasic.readthedocs.io/en/latest/released_programs/))
through **both** the Python reference oracle and the C port, and compares the
output. It is the real-world complement to the synthetic
[`../codegen_probes/`](../codegen_probes/) suite: where the probes drive
hand-picked codepaths, these fixtures stress the compiler the way real authors
did — deep `#include` chains, inline `asm`/`incbin`, banking, big programs.

> This whole folder is **additive and self-contained**. It does not touch the
> compiler, the inherited `tests/`, or any other harness. Nothing here is
> byte-truth on its own — the Python oracle is. We only assert *C matches Python*.

## What gets compared

Identical contract to `codegen_probes/run_probes.sh`, judged by the **first**
divergence:

| # | Part | How |
|---|------|-----|
| 1 | exit code | end-to-end run, must match |
| 2 | stderr | path-normalised cmp (warnings + semantic errors surface here) |
| 3 | stage-1 ASM | `--output-format=asm`, path-normalised cmp |
| 4 | end-to-end binary | `-o out.bin`, **raw** cmp (never normalise binary) |

All four match → `EQUAL`, then split into two **tiers** (derived at runtime, so
they can never go stale):

- **`BINARY-EQUAL`** — `EQUAL` *and* Python produced a real binary: full
  end-to-end byte-for-byte parity on a self-contained program.
- **`FRONTEND-EQUAL`** — `EQUAL` but no binary: Python and C both stop at the
  **same** point, **identically** (typically a missing `incbin` artifact or an
  external library the source assumes). This still proves preprocess + parse +
  codegen-to-asm parity. These are the **flyby-fix candidates** (see below).

Any `DIFF-*` is a genuine C-port finding: the C output diverges from Python.

## Why most real programs don't build standalone (and why that's fine)

Real releases ship `.bas` plus a build recipe (`.bat`/`Makefile`) that runs
external tooling — screen compressors (`rcs`), packers (`zx7`/`zx0`), `bin2tap`
— to generate `incbin` artifacts, and pull in helper libraries (`zx7.bas`,
sprite engines) not bundled in the source zip. Without those, **assembly** fails
— but it fails *the same way in Python and C*, so the program is still a valid
**front-end** parity fixture. We don't need a program to build to learn whether
the C port parses and codegens it identically.

## Layout

```
released_corpus/
├── manifest.tsv      # TRACKED: one row per program (urls, sha256, entry, flags)
├── fetch.sh          # capture archives -> cache/, verify sha256, stage -> work/, overlay fixups/
├── run_corpus.sh     # the parity meter (above)
├── fixups/<id>/      # TRACKED: our corpus-side fixes (missing libs / artifacts /
│                     #   replacement .bas) overlaid on work/<id>/ after extract
├── cache/            # gitignored: captured third-party archives (source of truth once verified)
└── work/             # gitignored: extracted + fixup-overlaid program trees
```

**Third-party program bytes are never committed.** Only the manifest (URLs +
`sha256`), the harness, and our own `fixups/` are tracked. This sidesteps
re-licensing mixed-freeware/GPL code into the repo and keeps it small.

### Drift guard

The manifest commits a `sha256` for every archive. On fetch, a freshly
downloaded archive whose hash doesn't match is **rejected** (not silently
tested) — so upstream re-packs or link-rot surface as `DRIFT`, not as phantom
failures. Once an archive is cached with a matching hash, re-runs use the
captured copy and never hit the network: the captured archive is the real
source of truth, immune to upstream drift.

## Usage

```bash
# 1. Capture + stage everything (downloads once into the gitignored cache):
./fetch.sh                      # or: ./fetch.sh zen pixel-quest   (subset)

# 2. Run the parity meter:
./run_corpus.sh                 # all staged programs
./run_corpus.sh zen             # one program
./run_corpus.sh --diff zen      # one program, dumping the first divergence

# Re-download a row (e.g. after a DRIFT or to refresh):
FORCE=1 ./fetch.sh zen
```

Requires Python 3.12 (oracle, auto-detected; pin via `PY=...`), a built
`csrc/build/bin/zxbc`, plus `curl`/`unzip` (and `tar`/`unrar` for the few
non-zip archives).

## CI role — decision 2026-06-11

**Local/manual for now; NOT wired into `make test` or CI.** Two reasons: the
suite is by-design not green (the `DIFF-*` rows are open C-port findings, so it
cannot gate), and `fetch.sh` needs network access to ~29 third-party hosts
(spectrumcomputing, web.archive.org, blogs) — flaky and impolite from CI
runners without a repo-side artifact cache. Revisit as a tracked-baseline
meter (fail only if the DIFF count *regresses*, same spirit as the probe RED
count) once the findings close and/or the cache has a CI-reachable mirror.

## manifest.tsv format

Tab-separated, `#` comments allowed. Columns:

| col | meaning |
|-----|---------|
| `id` | kebab slug, also the cache/work/fixups dir name |
| `name` | human title |
| `origin_url` | the release / landing page |
| `archive_url` | direct source-archive URL, or `NONE` |
| `sha256` | hex digest of the archive (drift guard), or `NONE` |
| `archive_type` | `zip` / `tar.gz` / `rar` / `bas` |
| `entry_bas` | main `.bas`, relative to `work/<id>/`, or `NONE` for libraries |
| `flags` | codegen flags passed to **both** Py and C (default `-O2`); the harness adds the output flags itself |
| `notes` | freeform |

## Flyby fixes (`fixups/`)

When a `FRONTEND-EQUAL` program is cheaply completable — it only lacks a
freely-available helper library or a regenerable artifact — drop the missing
file(s) into `fixups/<id>/` (mirroring the path inside `work/<id>/`). `fetch.sh`
overlays them after extraction, promoting the program to `BINARY-EQUAL`. We only
add whole files we can source/author cleanly; **edits to third-party files ship
as `<path>.patch` unified diffs** (applied by `fetch.sh` with `patch -p0`), so
only our delta is committed, never the program's bytes. Programs needing
proprietary build tools to regenerate `incbin` blobs are left as front-end
fixtures and noted as such.
**Fixups never patch the compiler** — that's a separate concern.

## Status

<!-- STATUS:BEGIN — regenerated by hand from ./run_corpus.sh output -->
2026-06-11 (Python 3.12 oracle vs C `zxbc`, after the DIFF-EXIT fix):
**29 programs tested, all stage and run cleanly.**

| verdict | count | meaning |
|---------|------:|---------|
| `BINARY-EQUAL` | 2 | full end-to-end byte parity |
| `FRONTEND-EQUAL` | 10 | parse/codegen parity (both stop identically) |
| `DIFF-EXIT` | 0 | — (was 2; both fixed) |
| `DIFF-STDERR` | 17 | **C-port finding**: diagnostics diverge |
| `DIFF-ASM` / `DIFF-BIN` | 0 | — |

→ **17 open divergences surfaced on real shipped programs**, 12 at parity.

**`BINARY-EQUAL` (2):** fourspriter, **o-trix** (newly fixed — see below).

**`DIFF-EXIT` (0) — both fixed 2026-06-11:** the C assembler modelled memory
as a flat 64K array and aborted with `Memory overflow at address 65536` the
instant the location counter reached `0x10000`, where Python's sparse-dict
memory model (`src/zxbasm/memory.py`) emits past 64K freely (only an explicit
`ORG` is range-checked to `[0..65535]`). On byte-identical stage-1 ASM, C
exited 5 and Python exited 0. Fixed by sizing the assembler image to the
practical Z80 reach (`csrc/zxbasm/zxbasm.h` `MAX_MEM = 0x20000`); probe
`codegen_probes/zxbasm/over_64k_emission.asm` locks it.
- `o-trix` — was the sharpest finding; now **BINARY-EQUAL** (identical 38126-byte
  binary).
- `retrobsesion` — overflow gone (now exits 0 like Python), but a **separate**
  string-array-constant codegen divergence remains, so it moved to `DIFF-STDERR`
  (the include-filename-case warning attribution now surfaces as its first
  divergence; the binary also differs behind it — a deeper open finding, see
  below).

**`DIFF-STDERR` (17):** 3-reyes-magos, abydos, ad-lunam, ad-lunam-plus, berksman,
breakspace, knights-demons-dx, looking-for-csscgc2012, maritrini, pixel-quest,
pixel-quest-2000, retrobsesion, saltarin, souls, walking-around-porto, zen,
zen-ii. Both compilers reach the same exit code but emit different diagnostics.
These cluster into a few still-open classes: error-recovery position drift
(`Too many errors. Giving up!` at a different line — 3-reyes-magos, abydos),
divergent recovery errors after a first error (ad-lunam, berksman),
`file not found` line-number quirks (zen, breakspace — note the line-0 form is
on the *Python* side and is itself an upstream PLY-lexer quirk), and
include-filename-case attribution (retrobsesion: source `#INCLUDE <ATTR.BAS>`
resolves to on-disk `attr.bas`; C reports the disk case in warnings, Python the
as-requested case). `saltarin` + `walking-around-porto` both *compile* (exit 0)
yet warn differently. See `--diff <id>` for each.

**`FRONTEND-EQUAL` (10) — genuine parity, both fail the same way:** bacaball,
bacachase, chessboard-attack, escape-from-cnossus, ratul-zeki, retrobsesion-ii,
solitario, spectral-dungeons, stela, vade-retro. Mostly old-dialect syntax both
versions now reject (single-line `IF`, `ELSEIF`, `ELSE`) or undefined
symbols/missing data files — not cheap flyby-fixes.

**Flyby fixes applied (1):** `souls` had hardcoded Windows absolute include
paths (`#include <c:/programacion/souls/...>`) for files that ship in the
archive. The [`fixups/souls/`](fixups/souls/) overlay de-Windowses them; this
promoted souls past the path error and **exposed a real error-recovery
divergence** (Python flags `souls.bas:136`, C flags `souls.bas:279`) — now a
`DIFF-STDERR` finding instead of a trivial file-not-found.

**Not run (recorded in manifest comments):**
- 13 released programs have **no downloadable `.bas` source** (binary-only
  releases, dead/login-gated links).
- 4 engines (bifrost/bifrost2/nirvana/nirvana+) have a valid primary archive but
  ship the demo `.bas` in a *separate* BorielZXBasicInterface archive.
- 3 (memorama, tales-of-grupp, zx-destroyer) captured as HTML interstitials /
  ephemeral mediafire links — not reproducibly fetchable.

> The `DIFF-*` rows are findings for the C-port work, not for this corpus to fix.
> The corpus surfaces them; adjudication/fixing the compiler is separate.
<!-- STATUS:END -->
