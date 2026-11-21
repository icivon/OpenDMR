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

/*
 * OpenDMR, service: M17 demodulation (m17_demod.h) from the FM discriminator at 8 kHz: 4800 symbols/s, a symbol every
 * 5/3 samples, each taken by cubic (Catmull-Rom) interpolation at its time (the recording is already band-limited:
 * measured, no receive filter does better, m17/README.md). The ring, the interpolation, the fit and the slicing are
 * fsk4.c's (shared with NXDN and DMR).
 *
 * Hunting: every third of a sample a start is tried: the 8 symbols of the LSF or the stream sync burst (all outer:
 * +-3) by their signs at the nearest samples first, then a least squares fit of gain and DC to the interpolated values
 * and its error; the best of the candidates close together, refined to a sixth of a sample, is the frame's start.
 * Locked: the frame's 192 symbols read; once all are in, scaled to levels with that gain and DC, the levels decision
 * directed over the frame (fsk4_relevel(): the four levels' means, the thresholds between them) and the 3-tap
 * equaliser (fsk4_eq3(), as DMR's), then out as soft symbols: on our recordings at ~-116 dBm the frames' bit errors a
 * third fewer (12.5 -> 8.4 a frame), Codec 2 frames right 96.7 -> 98.4 % (2026-11-20). The next sync looked for
 * within 2 samples of where it should be (a sixth of a sample apart), the timing, gain and DC taken from it; up to
 * MAX_MISSES frames without it are read where they should be. An EoT (at the levels followed) ends the lock.
 */

#include <string.h>

#include "m17_demod.h"

#define Q           12
#define ONE         (1 << Q)
#define FRAME       192
#define FRAME_T     1310720U            // 192 symbols, Q12 (192 * 5/3 samples)
#define HUNT_STEP   1365                // a third of a sample, Q12
#define FINE_STEP   683                 // a sixth
#define TRACK       12                  // the next sync looked for within +-TRACK sixths of a sample (2 samples)
#define MAX_MISSES  3
#define G_MIN       (100 << 8)          // gain at least (Q8, per symbol level; samples halved: an outer +-3 ~ 3800)
#define ERR_MAX     128                 // the fit's mean squared error at most, Q8 level^2 (0.5)
#define HUNT_ERR    48                  // the same hunting (0.19: noise found syncs at 0.5; a signal's are 0.03 - 0.15)
#define REBASE_AT   (1U << 18)          // samples: the Q12 times kept within int32
#define REBASE_BY   (1U << 17)          // a multiple of the ring

#ifndef M17_DEMOD_DD
#define M17_DEMOD_DD 1
#endif
#ifndef M17_DEMOD_EQ
#define M17_DEMOD_EQ 1
#endif
volatile uint32_t m17_demod_dd = M17_DEMOD_DD, m17_demod_eq = M17_DEMOD_EQ;

static const int8_t LSF[8] = {3, 3, 3, 3, -3, -3, 3, -3};    // 0x55F7; the stream sync 0xFF5D is its negative
static const int8_t STREAM[8] = {-3, -3, -3, -3, 3, 3, -3, 3};
static const int8_t EOT[8] = {3, 3, 3, 3, 3, 3, -3, 3};      // 0x555D

// the time of symbol i of a frame starting at t (Q12)
static int32_t sym_t(int32_t t, int i)
{
    return fsk4_sym_t(t, i, FSK4_SPS_4800_NUM, FSK4_SPS_DEN);
}

static int32_t sample(const m17_demod *d, int32_t t)
{
    return fsk4_at(&d->x, t);
}

static bool available(const m17_demod *d, int32_t t)
{
    return fsk4_have(&d->x, t);
}

// Gain (Q8) and DC by least squares of the 8 symbols at t to the pattern, the error (Q8, mean squared, in levels^2);
// false: no fit (too weak, or the error too large)
#ifdef M17_DEMOD_COUNT
unsigned long m17_demod_fits; // the PC tests: how much hunting costs
#endif

static bool fit(const m17_demod *d, int32_t t, const int8_t *p, int32_t *g, int32_t *dc, int32_t *err, int32_t max)
{
#ifdef M17_DEMOD_COUNT
    m17_demod_fits++;
#endif
    int32_t v[8];
    fsk4_fit_t f;
    for (int k = 0; k < 8; k++) {
        v[k] = sample(d, sym_t(t, k));
    }
    if (!fsk4_fit(v, p, 8, G_MIN, max, &f)) {
        return false;
    }
    *g = f.g;
    *dc = f.dc;
    *err = f.err;
    return true;
}

// the EOT at t against the levels followed (gain, DC): the mean squared error in levels^2, Q8
static int32_t eot_err(const m17_demod *d, int32_t t)
{
    int32_t gs = d->g >> 8;
    if (gs <= 0) {
        return INT32_MAX;
    }
    int64_t e2 = 0;
    for (int k = 0; k < 8; k++) {
        int32_t e = (sample(d, sym_t(t, k)) - d->dc) * 256 / gs - EOT[k] * 256; // Q8 levels
        e2 += (int64_t)e * e;
    }
    return (int32_t)((e2 / 8) >> 8);
}

// a sample as a soft symbol: (v - dc) / gain x M17_SOFT_ONE (gain Q8 a level), -127 .. 127
static int slice(const m17_demod *d, int32_t v)
{
    return fsk4_soft(v, d->g, d->dc, 16);
}

void m17_demod_init(m17_demod *d, m17_demod_cb cb, void *ctx)
{
    memset(d, 0, sizeof(*d));
    fsk4_ring_init(&d->x, d->buf, M17_DEMOD_RING);
    d->cb = cb;
    d->ctx = ctx;
    d->hunt_t = 4U << Q;
}

static void lock(m17_demod *d, int32_t t, int32_t g, int32_t dc)
{
    d->locked = true;
    d->t0 = t;
    d->i = 0;
    d->g = g;
    d->dc = dc;
    d->misses = 0;
    d->confirmed = false;
    d->cand = false;
    d->syncs++;
}

static void unlock(m17_demod *d, int32_t from)
{
    d->locked = false;
    d->cand = false;
    d->hunt_t = from;
}

// hunting: the candidates whose symbols are all in
static void hunt(m17_demod *d)
{
    while (!d->locked && available(d, sym_t(d->hunt_t, 7))) {
        int32_t t = d->hunt_t;
        d->hunt_t += HUNT_STEP;
        // the signs at the nearest samples against the LSF pattern (the stream sync's are the opposite)
        int32_t ref = d->dc_slow >> 8;
        int same = 0;
        for (int k = 0; k < 8; k++) {
            uint32_t s = (uint32_t)((sym_t(t, k) + (ONE / 2)) >> Q);
            same += ((d->x.x[s & d->x.mask] > ref) == (LSF[k] > 0));
        }
        const int8_t *p = (same >= 7) ? LSF : (same <= 1) ? STREAM : NULL;
        int32_t g, dc, err;
        if (p && fit(d, t, p, &g, &dc, &err, HUNT_ERR)) {
            if (!d->cand || err < d->cand_err) {
                d->cand = true;
                d->cand_t = t;
                d->cand_err = err;
                d->cand_g = g;
                d->cand_dc = dc;
            }
        }
        if (d->cand && ((int32_t)(t - d->cand_t) > 2 * ONE)) { // the best of those close together: refined
            const int8_t *best_p = NULL;
            for (int s = -1; s <= 1; s += 2) {
                int32_t tt = d->cand_t + s * FINE_STEP;
                for (int q = 0; q < 2; q++) {
                    const int8_t *pp = q ? STREAM : LSF;
                    if (fit(d, tt, pp, &g, &dc, &err, HUNT_ERR) && (err < d->cand_err)) {
                        d->cand_t = tt;
                        d->cand_err = err;
                        d->cand_g = g;
                        d->cand_dc = dc;
                        best_p = pp;
                    }
                }
            }
            (void)best_p;
            d->last_err = d->cand_err;
            lock(d, d->cand_t, d->cand_g, d->cand_dc);
        }
    }
}

// a frame's symbols out, once all are in: scaled to levels, their levels decision directed, equalised (as set)
static void frame_out(m17_demod *d)
{
    int32_t z[FRAME];
    int32_t gs = (d->g >> 8) ? (d->g >> 8) : 1;
    for (int k = 0; k < FRAME; k++) {
        z[k] = (d->fv[k] - d->dc) * 256 / gs;
    }
    if (m17_demod_dd) {
        fsk4_relevel(z, FRAME);
    }
    if (m17_demod_eq) {
        fsk4_eq3(z, FRAME);
    }
    for (int k = 0; k < FRAME; k++) {
        int32_t v = z[k] >> 4; // M17_SOFT_ONE (16) a level
        d->cb(d->ctx, (v > 127) ? 127 : (v < -127) ? -127 : (int)v);
    }
}

// locked: the symbols in; at a frame's end the next one's sync
static void track(m17_demod *d)
{
    while (d->locked) {
        if (d->i < FRAME) {
            int32_t t = sym_t(d->t0, d->i);
            if (!available(d, t)) {
                return;
            }
            d->fv[d->i++] = sample(d, t);
            if (d->i == FRAME) {
                frame_out(d);
            }
            continue;
        }
        int32_t t1 = d->t0 + FRAME_T;
        if (!available(d, sym_t(t1 + TRACK * FINE_STEP, 7))) {
            return;
        }
        int32_t best_err = ERR_MAX, bg = 0, bdc = 0;
        int32_t bt = 0;
        const int8_t *bp = NULL;
        for (int s = -TRACK; s <= TRACK; s++) {
            int32_t tt = t1 + s * FINE_STEP;
            int32_t g, dc, err;
            if (fit(d, tt, STREAM, &g, &dc, &err, ERR_MAX) && (err < best_err)) {
                best_err = err;
                bt = tt;
                bg = g;
                bdc = dc;
                bp = STREAM;
            }
            // the EOT (7 of its 8 symbols +3: nearly a constant) at the levels followed, not fitted: with its own gain
            // and DC free it fitted pieces of stream frames, the stream ended in the middle (a dozen times in 40 s at
            // -116 dBm, 2026-11-20)
            err = eot_err(d, tt);
            if ((err < HUNT_ERR) && (err < best_err)) {
                best_err = err;
                bt = tt;
                bg = d->g;
                bdc = d->dc;
                bp = EOT;
            }
        }
        if (bp) {
            d->t0 = bt;
            d->g = (d->g + bg) / 2;
            d->dc = (d->dc + bdc) / 2;
            d->misses = 0;
            d->confirmed = true;
            d->resyncs++;
            d->last_err = best_err;
            if (bp == EOT) { // its 8 symbols out (the frame receiver ends the stream), then hunting
                for (int k = 0; k < 8; k++) {
                    d->cb(d->ctx, slice(d, sample(d, sym_t(bt, k))));
                }
                d->eots++;
                unlock(d, sym_t(bt, 8));
                return;
            }
        } else {
            d->t0 = t1;
            if (!d->confirmed || (++d->misses > MAX_MISSES)) {
                d->lost++;
                d->cb(d->ctx, M17_DEMOD_LOST);
                unlock(d, t1);
                return;
            }
        }
        d->i = 0;
    }
}

void m17_demod_samples(m17_demod *d, const int16_t *x, int count)
{
    for (int k = 0; k < count; k++) {
        int32_t s = x[k] >> 1; // halved
        fsk4_put(&d->x, s);
        d->dc_slow += ((s << 8) - d->dc_slow) >> 10;
    }
    // the hunt never looks further back than the ring holds
    int32_t oldest = (d->x.n > M17_DEMOD_RING - 8) ? (int32_t)((d->x.n - M17_DEMOD_RING + 8) << Q) : 0;
    if (!d->locked && (d->hunt_t < oldest)) {
        d->hunt_t = oldest;
        d->cand = false;
    }
    hunt(d);
    track(d);
    hunt(d); // after an unlock
    if (d->x.n >= REBASE_AT) {
        fsk4_rebase(&d->x, REBASE_BY);
        d->hunt_t -= (int32_t)(REBASE_BY << Q);
        d->t0 -= (int32_t)(REBASE_BY << Q);
        d->cand_t -= (int32_t)(REBASE_BY << Q);
    }
}
