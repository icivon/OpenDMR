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

// M17 4FSK demodulation from FM discriminator audio at 8 kHz (the HR_C7000's recording, as NXDN's): no OS, no
// hardware, all integer; also built on the PC (m17/c). Soft symbols out (the level x M17_SOFT_ONE, m17_core.h) for
// m17_core.c.
#ifndef M17_DEMOD_H
#define M17_DEMOD_H

#include <stdbool.h>
#include <stdint.h>

#include "fsk4.h"

#define M17_DEMOD_RING 2048 // samples kept (256 ms; a frame is 320)
#define M17_DEMOD_LOST 1000 // given instead of a symbol: the signal lost, the frame receiver to start over

typedef void (*m17_demod_cb)(void *ctx, int z); // z: a soft symbol (-127 .. 127) or M17_DEMOD_LOST

// Times in samples Q12, from the start (a symbol is 5/3 samples)
// the receive chain's choices (m17_demod.c; over the cable, A / B): decision directed levels per frame, the 3-tap
// equaliser (both on). No receive filter: RRC 0.2 or 0.5 doubled the bit errors (our transmitter's shaping is the P25
// physical layer's, the recording band-limited already)
extern volatile uint32_t m17_demod_dd, m17_demod_eq;

typedef struct {
    int32_t fv[192];             // the frame's symbols as read, until all are in
    int32_t buf[M17_DEMOD_RING]; // the samples (fsk4.h)
    fsk4_ring x;
    int32_t dc_slow;           // the input's long-term mean, Q8 (hunting: the signs of a sync)
    int32_t hunt_t;            // hunting: the next start time to try (Q12)
    // hunting: the best sync within a few candidates
    bool cand;
    int32_t cand_t;
    int32_t cand_err, cand_g, cand_dc; // its error (Q8, level^2), gain (Q8, per symbol level), DC
    // locked
    bool locked;
    int32_t t0;                // the frame's start (its sync's first symbol, Q12)
    int i;                     // the next symbol of the frame (0 .. 191)
    int32_t g, dc;             // gain (Q8, per symbol level: +3 is 3 g), DC
    int misses;                // frames in a row without their sync
    bool confirmed;            // a sync found where the lock said it would be (else a miss ends the lock at once)
    m17_demod_cb cb;
    void *ctx;
    uint32_t syncs, resyncs, lost, eots; // counters
    int32_t last_err;          // the last sync's error (Q8)
} m17_demod;

void m17_demod_init(m17_demod *d, m17_demod_cb cb, void *ctx);
void m17_demod_samples(m17_demod *d, const int16_t *x, int count);

#endif
