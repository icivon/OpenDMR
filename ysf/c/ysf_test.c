// PC tests of the firmware's YSF code (src/services/ysf_core.c, p25_demod.c with YSF's sync):
//   ysf_test rx in.raw [out.bit [out.dibits]] 8 kHz S16LE discriminator recording -> frames (one line each), the voice as
//                                       DMR's 72-bit AMBE frames (9 bytes) into out.bit
//   ysf_test dibits in.dibits [out.bit] the same from dibits (one a byte, as on the air)
//   ysf_test tx in.bit out.dibits SRC  a V/D mode 2 call from SRC to ALL with the AMBE frames of in.bit (5 a frame):
//                                       preamble, HC, CC (FN 0..6, FT 6), TC; one dibit a byte
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p25_demod.h"
#include "ysf_core.h"

static const p25_demod_sync YSF_DEMOD_SYNC = {YSF_SYNC, YSF_SYNC_DIBITS, 17};
static ysf_rx rx;
static p25_demod dm;
static FILE *out_bit, *out_dibits;
static long dibits_in;
static int lost_events;

static void frame_line(void)
{
    const ysf_fich_t *f = &rx.fich;
    printf("%7.3f s sync %d FICH %s FI %d FN %d FT %d DT %d MR %d SC %d", dibits_in / 4800.0, rx.sync_wrong,
           rx.fich_ok ? "ok" : "guess", f->fi, f->fn, f->ft, f->dt, f->mr, f->sc);
    for (int k = 0; k < rx.dch_n; k++) {
        if (!rx.dch_ok[k]) {
            printf(" CRC!");
            continue;
        }
        int n = (f->dt == YSF_DT_VD2 && f->fi == YSF_FI_CC) ? 10 : 20;
        printf(" '");
        for (int i = 0; i < n; i++) {
            putchar((rx.dch[k][i] >= 32 && rx.dch[k][i] < 127) ? rx.dch[k][i] : '.');
        }
        printf("'");
    }
    if (rx.voices) {
        printf(" voice errs");
        for (int j = 0; j < rx.voices; j++) {
            printf(" %d", rx.voice_err[j]);
            if (out_bit) {
                fwrite(rx.voice[j], 1, 9, out_bit);
            }
        }
    }
    printf("\n");
}

static void on_dibit(void *ctx, int d)
{
    (void)ctx;
    if (d == P25_DEMOD_LOST) {
        ysf_rx_reset(&rx);
        lost_events++;
        return;
    }
    dibits_in++;
    if (out_dibits) {
        fputc(d, out_dibits);
    }
    if (ysf_rx_dibit(&rx, (uint8_t)d) == YSF_RX_FRAME) {
        frame_line();
    }
}

static void summary(void)
{
    printf("frames %u, FICH bad %u, DCH bad %u, repetition BER %.2f %% (%u/%u), lost %u, demod syncs %u lost %d\n",
           rx.frames, rx.fich_bad, rx.dch_bad, rx.rep_bits ? 100.0 * rx.rep_err / rx.rep_bits : 0.0, rx.rep_err,
           rx.rep_bits, rx.lost, dm.syncs, lost_events);
}

static int run_tx(const char *bit, const char *out, const char *src)
{
    FILE *in = fopen(bit, "rb"), *o = fopen(out, "wb");
    if (!in || !o) {
        return 1;
    }
    static uint8_t frames[4096][9];
    int n = (int)fread(frames, 9, 4096, in);
    uint8_t d[YSF_FRAME_DIBITS];
    ysf_fich_t f = {.fi = YSF_FI_HC, .cs = 2, .ft = 6, .dt = YSF_DT_VD2};
    ysf_tx_preamble(d, 200);
    fwrite(d, 1, 200, o);
    ysf_tx_hc_tc(&f, YSF_FI_HC, "**********", src, d);
    fwrite(d, 1, YSF_FRAME_DIBITS, o);
    int fn = 0;
    for (int k = 0; k + 5 <= n; k += 5) {
        uint8_t dch[10];
        f.fn = (uint8_t)fn;
        ysf_vd2_dch(fn, "**********", src, "OPDMR", dch);
        ysf_tx_vd2(&f, dch, (const uint8_t(*)[9])frames[k], d);
        fwrite(d, 1, YSF_FRAME_DIBITS, o);
        fn = (fn + 1) % 7;
    }
    ysf_tx_hc_tc(&f, YSF_FI_TC, "**********", src, d);
    fwrite(d, 1, YSF_FRAME_DIBITS, o);
    fclose(o);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ysf_test rx in.raw [out.bit] | dibits in.dibits [out.bit] | tx in.bit out.dibits SRC\n");
        return 2;
    }
    if (!strcmp(argv[1], "tx")) {
        return run_tx(argv[2], argv[3], (argc > 4) ? argv[4] : "N0CALL");
    }
    FILE *in = fopen(argv[2], "rb");
    if (!in) {
        return 1;
    }
    out_bit = (argc > 3) ? fopen(argv[3], "wb") : NULL;
    out_dibits = (argc > 4) ? fopen(argv[4], "wb") : NULL;
    ysf_rx_init(&rx);
    if (!strcmp(argv[1], "dibits")) {
        int c;
        while ((c = fgetc(in)) != EOF) {
            dibits_in++;
            if (ysf_rx_dibit(&rx, (uint8_t)c) == YSF_RX_FRAME) {
                frame_line();
            }
        }
    } else {
        p25_demod_init_sync(&dm, on_dibit, NULL, &YSF_DEMOD_SYNC);
        int16_t x[256];
        size_t n;
        while ((n = fread(x, 2, 256, in)) > 0) {
            p25_demod_samples(&dm, x, (int)n);
        }
    }
    summary();
    return 0;
}
