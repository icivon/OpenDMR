/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// An encoder for the Codec 2 3200 bit/s frames M17 voice streams carry: the analysis m17/codec2-3200.md section 9
// describes, our own implementation, integer but for a few float steps a frame (the CPU has no FPU); no OS, no
// hardware, also built on the PC (m17/c).
#ifndef C2ENC_H
#define C2ENC_H

#include <stdint.h>

#include "c2dec.h"

#define C2ENC_M     320 // the pitch analysis window, 40 ms
#define C2ENC_NW    278 // the analysis window's nonzero samples
#define C2ENC_DEC   64  // the pitch estimator's decimated samples (every 5th of the window)

typedef struct {
    int16_t sn[C2ENC_M];       // the input, the newest last
    int64_t nlp_x1, nlp_y1;    // the pitch estimator's DC notch, Q8
    int64_t fir[48];           // its low pass's input, a ring
    uint32_t fir_pos;
    int64_t dec[C2ENC_DEC];    // its low pass's output, every 5th sample, Q24
    uint32_t prev_bin;         // the last pitch estimate (its DFT bin)
    int16_t hann[C2ENC_NW];    // the analysis window, Q15
    int16_t nlp_w[C2ENC_DEC];  // the pitch estimator's window, Q15
    int16_t wt[18];            // the analysis window's spectrum around 0, Q12
    int32_t re[257], im[257];  // the spectrum (bins 0 .. 256)
    uint32_t pw[257];          // |the spectrum|^2
} c2enc_t;

void c2enc_init(c2enc_t *e);
void c2enc_encode(c2enc_t *e, const int16_t pcm[C2DEC_SAMPLES], uint8_t frame[C2DEC_BYTES]);

#endif
