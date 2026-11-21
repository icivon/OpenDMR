// Decodes raw Codec 2 3200 frames (8 bytes each) with src/services/c2dec.c into raw 16-bit 8 kHz PCM (PC test).
#include <stdio.h>
#include "c2dec.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: c2dec_run in.c2 out.raw\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) {
        return 1;
    }
    static c2dec_t d;
    uint8_t f[C2DEC_BYTES];
    int16_t pcm[C2DEC_SAMPLES];
    c2dec_init(&d);
    while (fread(f, 1, sizeof(f), in) == sizeof(f)) {
        c2dec_decode(&d, f, pcm);
        fwrite(pcm, 2, C2DEC_SAMPLES, out);
    }
    fclose(out);
    return 0;
}
