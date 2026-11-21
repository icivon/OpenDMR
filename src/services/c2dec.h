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

// A decoder for the Codec 2 3200 bit/s frames M17 voice streams carry: the decoding m17/codec2-3200.md describes, our
// own implementation, all integer (the CPU has no FPU); no OS, no hardware, also built on the PC (m17/c).
#ifndef C2DEC_H
#define C2DEC_H

#include <stdbool.h>
#include <stdint.h>

#define C2DEC_BYTES   8   // 64 bits a frame, 20 ms
#define C2DEC_SAMPLES 160 // 8 kHz
#define C2DEC_N       80  // a subframe (10 ms)
#define C2DEC_BINS    256 // the spectrum's points over 0 .. 4 kHz (a 512 point DFT's lower half)

typedef struct {
    bool voiced;
    uint32_t wo;      // the fundamental, phase a sample (2^32: 2 pi)
    int32_t e;        // the energy, log2 Q16
    uint32_t lsp[10]; // the LSP frequencies, phase a sample (2^32: 2 pi)
} c2dec_model_t;

typedef struct {
    c2dec_model_t prev; // the last frame's second subframe
    uint32_t ex_phase;  // the excitation's phase (2^32: 2 pi)
    int32_t bg;         // the background level, dB Q16
    uint32_t rand;      // the random phases' generator
    int32_t sn[2 * C2DEC_N];       // the overlap add, Q1
    int32_t pw[C2DEC_BINS];        // the power spectrum, log2 Q16 (c2dec.c)
    uint32_t arg_h[C2DEC_BINS + 1]; // the synthesis filter's phase where a harmonic samples it
} c2dec_t;

// shared with the encoder (c2enc.c)
extern const int16_t c2_sintab[1024];       // sin, Q15, 1024 a turn
int32_t c2_ilog2(uint64_t x);               // log2(x), x > 0, Q16
uint32_t c2_cordic_angle(int32_t x, int32_t y); // the angle of (x, y), 2^32: 2 pi

void c2dec_init(c2dec_t *d);
void c2dec_decode(c2dec_t *d, const uint8_t frame[C2DEC_BYTES], int16_t pcm[C2DEC_SAMPLES]);

#endif
