// src/services/m17_demod.c and m17_core.c on raw 16-bit 8 kHz discriminator samples (PC test):
//   m17_demod_test in.raw [out.c2]
#include <stdio.h>
#include <string.h>

#include "m17_core.h"
#include "m17_demod.h"

static m17_rx rx;
static m17_demod dm;
static FILE *out, *metrics; // metrics: a byte a stream frame (argv[3])
static unsigned streams, metric_sum, metric_max, lsfs, eots, losts;
static long dibits;
static double gain_sum; // the demodulator's gain at each stream frame (per symbol level)

static void on_dibit(void *ctx, int d)
{
    (void)ctx;
    if (d == M17_DEMOD_LOST) {
        losts++;
        m17_rx_reset(&rx);
        return;
    }
    dibits++;
    bool had = rx.lsf_valid;
    switch (m17_rx_symbol(&rx, (int8_t)d)) {
    case M17_RX_LSF:
        lsfs++;
        break;
    case M17_RX_STREAM:
        streams++;
        gain_sum += dm.g / 256.0;
        metric_sum += rx.metric;
        if (rx.metric > metric_max) {
            metric_max = rx.metric;
        }
        if (out) {
            fwrite(rx.stream, 1, M17_STREAM_BYTES, out);
        }
        if (metrics) {
            fputc(rx.metric > 255 ? 255 : rx.metric, metrics);
            fputc(rx.soft_metric > 255 ? 255 : rx.soft_metric, metrics);
        }
        break;
    case M17_RX_EOT:
        eots++;
        break;
    default:
        break;
    }
    if (!had && rx.lsf_valid) {
        char src[10], dst[10];
        m17_address(rx.lsf + 6, src);
        m17_address(rx.lsf, dst);
        printf("LSF %s -> %s after %u stream frames\n", src, dst, streams);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    out = (argc > 2) ? fopen(argv[2], "wb") : NULL;
    metrics = (argc > 3) ? fopen(argv[3], "wb") : NULL;
    if (!in) {
        return 1;
    }
    m17_rx_init(&rx);
    m17_demod_init(&dm, on_dibit, NULL);
    int16_t buf[256];
    size_t n;
    while ((n = fread(buf, 2, 256, in)) > 0) {
        m17_demod_samples(&dm, buf, (int)n);
    }
    printf("syncs %u resyncs %u lost %u eots %u | frames: lsf %u, stream %u (metric mean %.2f, max %u), core eot %u, "
           "lich ok %u bad %u, dibits %ld, last sync err %.2f, gain %.1f a symbol level\n",
           dm.syncs, dm.resyncs, dm.lost, dm.eots, lsfs, streams, streams ? (double)metric_sum / streams : 0.0,
           metric_max, eots, rx.lich_ok, rx.lich_bad, dibits, dm.last_err / 256.0,
           streams ? gain_sum / streams : 0.0);
    if (out) {
        fclose(out);
    }
    return 0;
}
