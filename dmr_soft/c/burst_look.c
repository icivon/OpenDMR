// a burst log (37 bytes a burst: dmr_soft.c dmr_soft_burst_log) through dmr_l2.c: each burst's middle against the
// SYNC patterns, its Slot Type / EMB, a data burst's BPTC and LC (PC tool)
#include <stdio.h>
#include <string.h>
#include "dmr_l2.h"
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    uint8_t e[37];
    for (int n = 0; f && fread(e, 37, 1, f) == 1; n++) {
        uint64_t m = dmr_burst_middle(e);
        int best = 0, bd = 99;
        for (int k = 1; k < DMR_SYNC_KINDS; k++) {
            int d = __builtin_popcountll(m ^ dmr_sync_word[k]);
            if (d < bd) { bd = d; best = k; }
        }
        uint8_t cc, dt, lc[12], pi, lcss;
        bool st = dmr_slot_type(e, &cc, &dt);
        bool em = dmr_emb(e, &cc, &pi, &lcss);
        printf("%2d sync %d ts %d mse %.2f | nearest sync %d at %d bits | slot type %d (cc %d dt %d) emb %d", n, e[33], e[34],
               e[35] / 16.0, best, bd, st, cc, dt, em);
        if (st && (dt == 1 || dt == 2) && dmr_bptc196(e, lc)) {
            printf(" bptc ok rs %d", dmr_full_lc_check(lc, dt == 1 ? DMR_MASK_VOICE_LC_HEADER : DMR_MASK_TERMINATOR_LC));
        }
        printf("\n");
    }
    return 0;
}
