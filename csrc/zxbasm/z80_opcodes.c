/* z80_opcodes.c -- Binary search lookup for Z80 opcode table
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "z80_opcodes.h"
#include <string.h>

const Z80Opcode *z80_find_opcode(const char *mnemonic)
{
    int lo = 0;
    int hi = Z80_OPCODE_COUNT - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(mnemonic, Z80_OPCODES[mid].asm_name);
        if (cmp == 0) {
            return &Z80_OPCODES[mid];
        } else if (cmp < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    return NULL;
}
