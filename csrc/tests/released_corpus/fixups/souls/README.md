# fixup: souls

**Problem (frontend failure, parity):** `souls.bas` lines 5–6 use hardcoded
Windows *absolute* include paths:

```
#include <c:/programacion/souls/sprites.bas>
#include <c:/programacion/souls/mapas.bas>
```

Those paths don't exist off the author's machine, so both the Python oracle and
the C port fail identically with `file 'c:/programacion/souls/sprites.bas' not
found` — a `FRONTEND-EQUAL` (real parity, no binary). The referenced files
**are** present in the archive (`sprites.bas`, `mapas.bas`, next to `souls.bas`).

**Fix:** `souls.bas.patch` rewrites *only* those two lines to local quote-form
includes (`#include "sprites.bas"` / `#include "mapas.bas"`). It ships as a
unified diff — not as the full file — because `souls.bas` is third-party
program source and this corpus never commits third-party bytes (we commit our
delta only). `fetch.sh` applies it to `work/souls/souls.bas` after extraction.

This is a **corpus-side portability fix** — it does not change program logic and
does not touch the compiler. It promotes souls from a front-end fixture to a
full end-to-end build that exercises both compilers identically.
