// src/services/m17_core.c on a dibit file (one dibit a byte, as on the air), PC test:
//   m17_core_test in.dibits [out.c2] [--preamble] [--gap FROM N] [--flip EVERY]
// --preamble: 192 dibits of +3 -3 first (as a transmitter sends); --gap: N dibits from FROM (counted after the
// preamble's last dibit... as the HR_C7000 loses them, from the sync on) erased; --flip: every EVERY-th dibit wrong
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m17_core.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: m17_core_test in.dibits [out.c2] [--preamble] [--gap FROM N] [--flip EVERY]\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = NULL;
    int preamble = 0, gap_from = -1, gap_n = 0, flip = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--preamble")) {
            preamble = 1;
        } else if (!strcmp(argv[i], "--gap") && i + 2 < argc) {
            gap_from = atoi(argv[++i]);
            gap_n = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--flip") && i + 1 < argc) {
            flip = atoi(argv[++i]);
        } else {
            out = fopen(argv[i], "wb");
        }
    }
    if (!in) {
        return 1;
    }
    static uint8_t buf[1 << 20];
    size_t n = 0;
    if (preamble) {
        for (int i = 0; i < 192; i++) {
            buf[n++] = (i & 1) ? 3 : 1; // +3, -3
        }
    }
    int c;
    while ((c = fgetc(in)) != EOF && n < sizeof(buf)) {
        buf[n++] = (uint8_t)c;
    }
    size_t base = preamble ? 192 : 0; // the LSF sync's first dibit
    static m17_rx rx;
    m17_rx_init(&rx);
    unsigned streams = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t d = buf[i];
        long rel = (long)i - (long)base;
        if (gap_from >= 0 && rel >= gap_from && rel < gap_from + gap_n) {
            d = M17_DIBIT_ERASED;
        }
        if (flip && (i % (size_t)flip) == 0 && d < 4) {
            d ^= 1;
        }
        bool had = rx.lsf_valid;
        m17_rx_event_t ev = m17_rx_dibit(&rx, d);
        if (ev == M17_RX_LSF || (!had && rx.lsf_valid)) {
            char src[10], dst[10];
            m17_address(rx.lsf + 6, src);
            m17_address(rx.lsf, dst);
            printf("%zu: LSF (%s) %s -> %s type %02x%02x voice %d metric %u\n", i, ev == M17_RX_LSF ? "frame" : "LICH",
                   src, dst, rx.lsf[12], rx.lsf[13], m17_lsf_voice(rx.lsf), rx.metric);
        }
        if (ev == M17_RX_STREAM) {
            if (streams < 3 || rx.eos) {
                printf("%zu: stream fn %u eos %d metric %u\n", i, rx.fn, rx.eos, rx.metric);
            }
            streams++;
            if (out) {
                fwrite(rx.stream, 1, M17_STREAM_BYTES, out);

            }
        }
        if (ev == M17_RX_EOT) {
            printf("%zu: EOT\n", i);
        }
    }
    printf("streams %u (bad %u), lsf %u (bad %u), lich ok %u bad %u, eot %u, syncs lost %u\n", rx.streams,
           rx.stream_bad, rx.lsfs, rx.lsf_bad, rx.lich_ok, rx.lich_bad, rx.eots, rx.syncs_lost);
    if (out) {
        fclose(out);
    }
    return 0;
}
