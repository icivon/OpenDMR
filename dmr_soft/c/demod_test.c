// src/services/dmr_demod.c + dmr_l2.c on raw 16-bit 8 kHz discriminator recordings (PC test), scored as
// dmr_soft/py/dmr_eval.py does: the voice superframes (a voice sync, then the 5 bursts of the same slot after it; taken
// when all of B - F have a good EMB: a signal there), their AMBE frames by position in the burst: bad (C0 + C1 corrections > 3), with no errors, and (--tone) right after the
// FEC against the transmitter's test frame (vocoder_tx_test).
//   demod_test [--tone] [-v] a.raw b.raw ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ambe.h"
#include "dmr_demod.h"
#include "dmr_l2.h"

static dmr_demod dm;
static int verbose, tone;
static long slot;               // bursts so far (both timeslots)
static long sf_slot = -100;     // the slot of the last voice sync (burst A)
static unsigned frames[3], bad[3], clean[3], right[3], bursts_sync, bursts, lcs, emb_ok, emb_bad;
static uint8_t ref49[49];
static struct {
    int e[6][3];  // C0 + C1 corrections of each frame, bursts A - F
    int right[6][3];
    int emb;      // B - F with a good EMB
    int have;     // bursts in
} sf;

static void sf_commit(void)
{
    if (sf.have && (sf.emb >= 5)) {
        for (int k = 0; k < sf.have; k++) {
            for (int f = 0; f < 3; f++) {
                frames[f]++;
                bad[f] += (sf.e[k][f] > 3);
                clean[f] += (sf.e[k][f] == 0);
                right[f] += sf.right[k][f];
            }
        }
    }
    memset(&sf, 0, sizeof(sf));
}
static dmr_emb_lc elc;

static void on_burst(void *ctx, const dmr_burst_t *b)
{
    (void)ctx;
    bursts++;
    if (b->sync != DMR_SYNC_NONE) {
        bursts_sync++;
    }
    if (DMR_SYNC_IS_VOICE(b->sync)) {
        sf_commit();
        sf_slot = slot;
    }
    long rel = slot - sf_slot;
    if (verbose) {
        uint8_t at, tc, ls, cc = 0, dt = 0;
        bool tact = dmr_tact(b->cach, &at, &tc, &ls);
        bool st = DMR_SYNC_IS_DATA(b->sync) && dmr_slot_type(b->bits, &cc, &dt);
        printf("slot %ld sync %d since %d mse %.2f g %d rel %ld tact %d tc %d at %d", slot, b->sync, b->since_sync,
               b->mse / 256.0, b->g >> 8, rel, tact, tc, at);
        if (st) {
            printf(" cc %d data type %d", cc, dt);
        }
        printf("\n");
    }
    if ((rel >= 0) && (rel <= 10) && !(rel & 1)) { // A - F of the superframe
        if (rel > 0) {
            uint8_t cc, pi, lcss, frag[4], lc[9];
            if (dmr_emb(b->bits, &cc, &pi, &lcss)) {
                emb_ok++;
                sf.emb++;
                dmr_embedded(b->bits, frag);
                if (dmr_emb_lc_add(&elc, lcss, frag, lc)) {
                    dmr_lc_t p;
                    dmr_lc_parse(lc, &p);
                    lcs++;
                    if (verbose) {
                        printf("  embedded LC flco %d dst %u src %u\n", p.flco, p.dst, p.src);
                    }
                }
            } else {
                emb_bad++;
            }
        }
        uint8_t v[3][9];
        dmr_voice_frames(b->bits, v);
        for (int f = 0; f < 3; f++) {
            uint8_t d49[49];
            sf.e[sf.have][f] = ambe_decode_frame(v[f], d49);
            sf.right[sf.have][f] = !memcmp(d49, ref49, 49);
        }
        if (verbose) {
            printf("  frames %d %d %d\n", sf.e[sf.have][0], sf.e[sf.have][1], sf.e[sf.have][2]);
        }
        sf.have++;
    }
    slot++;
}

int main(int argc, char **argv)
{
    static const uint8_t tone_frame[9] = {0xCE, 0xA8, 0xFE, 0x83, 0xAC, 0xC4, 0x58, 0x20, 0x0A};
    ambe_decode_frame(tone_frame, ref49);
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-v")) {
            verbose = 1;
            continue;
        }
        if (!strcmp(argv[a], "--tone")) {
            tone = 1;
            continue;
        }
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            continue;
        }
        dmr_demod_init(&dm, on_burst, NULL);
        dmr_emb_lc_reset(&elc);
        sf_slot = -100;
        slot = 0;
        int16_t x[256];
        size_t n;
        while ((n = fread(x, 2, 256, f)) > 0) {
            dmr_demod_samples(&dm, x, (int)n);
        }
        fclose(f);
        sf_commit();
        if (verbose) {
            printf("%s: syncs %u lost %u bursts %u\n", argv[a], dm.syncs, dm.lost, dm.bursts);
        }
    }
    printf("bursts %u (with a sync %u), EMB ok %u bad %u, embedded LCs %u\n", bursts, bursts_sync, emb_ok, emb_bad, lcs);
    unsigned fa = frames[1] + frames[2];
    printf("positions 2-3: %u frames, bad %.1f %%, no errors %.0f %%", fa, 100.0 * (bad[1] + bad[2]) / (fa ? fa : 1),
           100.0 * (clean[1] + clean[2]) / (fa ? fa : 1));
    if (tone) {
        printf(", right %.1f %%", 100.0 * (right[1] + right[2]) / (fa ? fa : 1));
    }
    printf(" | position 1: %u frames, bad %.1f %%\n", frames[0], 100.0 * bad[0] / (frames[0] ? frames[0] : 1));
    return 0;
}
