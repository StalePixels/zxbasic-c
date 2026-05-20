/*
 * pyfloat.c — Python str/repr(float): the shortest decimal that
 * round-trips through float().
 *
 * Python 3's float-to-str uses Steele & White / Ryu (since 3.1). C has
 * no portable equivalent; %.17g is "safe" but produces 17 digits for
 * any value, which does NOT match Python's "shortest". Implemented here
 * as the iterative round-trip probe: print with N digits, parse back,
 * accept the smallest N whose parse equals the original. Matches
 * Python's output for every double that comes up in the corpus (PI,
 * constant-folded floats, DATA literals).
 *
 * Edge cases reproduced:
 *   inf / -inf -> "inf" / "-inf"
 *   nan        -> "nan"
 *   integer-valued positive/negative finite double -> "X.0" / "-X.0"
 *     (Python str(1.0) == '1.0' — the C cast-to-long path elsewhere
 *      hides the .0; this helper is for the cases where Python keeps it.)
 *
 * Scientific notation: Python switches to "Xe+NN" around |v|>=1e16 and
 * small magnitudes <1e-4. %.Ng already chooses between 'f' and 'e' on
 * the same boundary as Python, so the form falls out naturally; we just
 * normalise the exponent suffix to "e+NN" / "e-NN" with no leading
 * zeros (Python does the same).
 *
 * Kept in its own translation unit (separate from z80asm.c) so that the
 * small test_ast / test_symboltable / test_check targets — which use
 * ast.c but do NOT link the full optimizer + parser — can pick it up
 * directly without dragging in z80asm.c.
 */

#include "z80asm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void z80h_pyfloat_repr(double v, char *buf, int sz) {
    if (sz <= 0 || buf == NULL) return;
    /* IEEE special values. */
    if (v != v) { snprintf(buf, sz, "nan"); return; }
    if (v > 1.7976931348623157e308) { snprintf(buf, sz, "inf"); return; }
    if (v < -1.7976931348623157e308) { snprintf(buf, sz, "-inf"); return; }
    /* Integer-valued doubles within Python's "X.0" range (anything that
     * f-formats without exponent). Python str(1.0) -> '1.0'. */
    if (v == (double)(long long)v && v >= -1e16 && v <= 1e16) {
        snprintf(buf, sz, "%lld.0", (long long)v);
        return;
    }
    /* Shortest round-trip probe. */
    char tmp[64];
    for (int p = 1; p <= 17; p++) {
        snprintf(tmp, sizeof(tmp), "%.*g", p, v);
        double rv = strtod(tmp, NULL);
        if (rv == v) break;
    }
    /* Normalise the exponent suffix to match Python: 'e+NN' / 'e-NN'
     * with no leading zero on the exponent ('e+5' not 'e+05'). */
    char *e = strchr(tmp, 'e');
    if (e == NULL) e = strchr(tmp, 'E');
    if (e) {
        *e = 'e';
        char sign = '+';
        char *p = e + 1;
        if (*p == '+' || *p == '-') { sign = *p; p++; }
        while (*p == '0' && *(p + 1) != '\0') p++;
        char num[16];
        snprintf(num, sizeof(num), "%s", p);
        snprintf(e + 1, sizeof(tmp) - (size_t)(e + 1 - tmp), "%c%s", sign, num);
    }
    snprintf(buf, sz, "%s", tmp);
}
