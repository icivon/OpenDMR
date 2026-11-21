// m17_tx_test in.c2 out.txt [SRC]: Codec 2 frames into M17 frames (src/services/m17_core.c's transmit) as
// m17/py/m17_frames.py writes them (hex, the 48 bytes of each frame), then through the receiver: all decoded?
#include <stdio.h>
#include <string.h>

#include "m17_core.h"

static void emit(FILE *f, const char *kind, const uint8_t *d)
{
    fprintf(f, "%s 48 ", kind);
    for (int i = 0; i < 48; i++) {
        fprintf(f, "%02x", (d[4 * i] << 6) | (d[4 * i + 1] << 4) | (d[4 * i + 2] << 2) | d[4 * i + 3]);
    }
    fprintf(f, "\n");
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: m17_tx_test in.c2 out.txt [SRC]\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "w");
    static uint8_t c2[1 << 20];
    size_t n = fread(c2, 1, sizeof(c2), in) / 16;
    uint8_t lsf[M17_LSF_BYTES], d[M17_FRAME_DIBITS];
    m17_lsf_make(lsf, NULL, argc > 3 ? argv[3] : "VA3BPF", 0);
    static m17_rx rx;
    m17_rx_init(&rx);
    unsigned streams = 0, lsfs = 0, eots = 0, bad = 0;
    m17_tx_preamble(d);
    for (int i = 0; i < M17_FRAME_DIBITS; i++) {
        m17_rx_dibit(&rx, d[i]);
    }
    m17_tx_lsf(lsf, d);
    emit(out, "M17LSF", d);
    for (int i = 0; i < M17_FRAME_DIBITS; i++) {
        lsfs += m17_rx_dibit(&rx, d[i]) == M17_RX_LSF;
    }
    for (size_t k = 0; k < n; k++) {
        m17_tx_stream(lsf, (uint16_t)k, k == n - 1, &c2[16 * k], d);
        emit(out, "M17STREAM", d);
        for (int i = 0; i < M17_FRAME_DIBITS; i++) {
            if (m17_rx_dibit(&rx, d[i]) == M17_RX_STREAM) {
                streams++;
                bad += (rx.fn != (k & 0x7FFF)) || memcmp(rx.stream, &c2[16 * k], 16) || (rx.eos != (k == n - 1)) || rx.metric;
            }
        }
    }
    m17_tx_eot(d);
    emit(out, "M17EOT", d);
    for (int i = 0; i < M17_FRAME_DIBITS; i++) {
        eots += m17_rx_dibit(&rx, d[i]) == M17_RX_EOT;
    }
    char src[10], dst[10];
    m17_address(rx.lsf + 6, src);
    m17_address(rx.lsf, dst);
    printf("%zu frames: LSF %u (%s -> %s), streams %u (bad %u), EoT %u\n", n, lsfs, src, dst, streams, bad, eots);
    fclose(out);
    return 0;
}
