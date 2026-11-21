// c2enc_run in.raw out.c2: 8 kHz 16 bit mono through src/services/c2enc.c (also timing)
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "c2enc.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: c2enc_run in.raw out.c2\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) {
        perror("open");
        return 1;
    }
    static c2enc_t e;
    c2enc_init(&e);
    int16_t pcm[C2DEC_SAMPLES];
    uint8_t frame[C2DEC_BYTES];
    unsigned n = 0;
    clock_t t = clock();
    while (fread(pcm, sizeof(int16_t), C2DEC_SAMPLES, in) == C2DEC_SAMPLES) {
        c2enc_encode(&e, pcm, frame);
        fwrite(frame, 1, C2DEC_BYTES, out);
        n++;
    }
    fprintf(stderr, "%u frames, %.1f us a frame (PC)\n", n, 1e6 * (double)(clock() - t) / CLOCKS_PER_SEC / (n ? n : 1));
    fclose(in);
    fclose(out);
    return 0;
}
