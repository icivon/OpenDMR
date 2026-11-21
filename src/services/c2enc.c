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
 * OpenDMR, service: the Codec 2 3200 encoder (c2enc.h), the analysis m17/codec2-3200.md section 9 describes;
 * m17/py/c2_3200_enc_model.py is the same in floating point.
 *
 * Integer where it counts (the CPU has no FPU): the spectrum a 512 point real FFT through a 256 point complex one in
 * block floating point; the pitch estimator's low pass only at the samples it keeps, its spectrum a DFT over the bins
 * it searches; the harmonic sums and the voicing with 64 bit sums; the LSP root search in Q15 and CORDIC. Floats only
 * for the LPC recursion and the energy's scale (a few dozen operations a frame).
 */

#include <string.h>

#include "c2enc.h"

#ifdef C2DEC_PROFILE // the time each step takes (the firmware's debug build: c2enc_prof over the cable), us
uint32_t systime_get_us(void);
uint32_t c2enc_prof[6]; // the pitch estimator's filter, its spectrum and peaks, the spectrum, refinement, voicing, LPC
static uint32_t prof_t;
#define PROF(n) do { uint32_t t_ = systime_get_us(); if ((n) >= 0) c2enc_prof[(n) < 0 ? 0 : (n)] += t_ - prof_t; prof_t = t_; } while (0)
#else
#define PROF(n)
#endif

#define N      C2DEC_N  // 80: a subframe
#define M      C2ENC_M
#define NW     C2ENC_NW
#define HALF   139      // the window's half
#define W0     21       // the window's first sample in sn (M / 2 - HALF)
#define ONE    65536
#define DB_PER_L2 197283     // 10 log10(2), Q16
#define L2_C2     (-1029171) // 2 log2(1 / sqrt(512 sum w^2)): the reference's window normalisation, Q16
#define WT_MAX    17         // the window's spectrum kept for |bin| <= 17
#ifndef SPECTRUM_HP
#define SPECTRUM_HP 0        // the speech spectrum's FFT with two products a multiplication (1) or one (as good here)
#endif

// the pitch estimator's 600 Hz low pass (48 taps, symmetric: the first half), Q16
static const int16_t nlp_fir[24] = {
    -71, -72, -61, -28, 36, 131, 243, 337, 367, 282, 53, -316,
    -767, -1193, -1446, -1371, -839, 211, 1749, 3639, 5656, 7524, 8962, 9744,
};

// 0.994^i: the LPC's 15 Hz bandwidth expansion
static const float bw_exp[11] = {
    1.0f, 0.994f, 0.988036f, 0.982107784f, 0.976215137f, 0.970357846f, 0.964535699f, 0.958748485f, 0.952995994f,
    0.947278018f, 0.94159435f,
};

/* ---- arithmetic ---- */

// cos of a phase (2^32: 2 pi), Q15, interpolated
static int32_t icos(uint32_t p)
{
    p += 0x40000000U;
    uint32_t i = p >> 22;
    int32_t a = c2_sintab[i], b = c2_sintab[(i + 1) & 1023];
    return a + (((b - a) * (int32_t)((p >> 6) & 0xFFFF)) >> 16);
}

static uint32_t iabs(int32_t v)
{
    return (uint32_t)(v < 0 ? -v : v);
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1U << 30;
    while (b > v) {
        b >>= 2;
    }
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

// the shift that brings the largest magnitude mx into [2^(bits - 1), 2^bits): > 0 right, < 0 left
static int norm_shift(uint32_t mx, int bits)
{
    if (!mx) {
        return 0;
    }
    return (32 - __builtin_clz(mx)) - bits;
}

// v / 20 (truncated) for |v| < 2^39 by two 32 bit divisions (a 64 bit one is a long library call)
static int64_t div20(int64_t v)
{
    uint64_t a = (uint64_t)(v < 0 ? -v : v);
    uint32_t hi = (uint32_t)(a >> 20), lo = (uint32_t)a & 0xFFFFF;
    uint64_t q = ((uint64_t)(hi / 20) << 20) + ((((hi % 20) << 20) | lo) / 20);
    return (v < 0) ? -(int64_t)q : (int64_t)q;
}

/* ---- the spectrum (9.2) ---- */

// a c 2^-15, |a| < 2^30, |c| <= 2^15: two 32 bit products (the CPU has no wider multiply)
static int32_t mulq15(int32_t a, int32_t c)
{
    return (a >> 15) * c + (((a & 0x7FFF) * c) >> 15);
}

// in place, radix 2, forward (e^-j), from the stage of half size h0 (1: all; 8: the first three left to the caller,
// whose input had its nonzero values only every 8th in the bit reversed order: each block of 8 that value); block
// floating point: every stage's input below 2^28 (hp, two products a multiplication) or 2^14 (one)
static void fft256(int32_t *re, int32_t *im, unsigned h0, bool hp)
{
    if (h0 == 1) {
        for (unsigned i = 1, j = 0; i < 256; i++) {
            unsigned bit = 128;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j |= bit;
            if (i < j) {
                int32_t t = re[i];
                re[i] = re[j];
                re[j] = t;
                t = im[i];
                im[i] = im[j];
                im[j] = t;
            }
        }
    }
    uint32_t limit = hp ? (1U << 28) : (1U << 14), mx = 0;
    for (unsigned h = h0; h < 256; h <<= 1) {
        if (mx >= limit) {
            for (unsigned i = 0; i < 256; i++) {
                re[i] >>= 1;
                im[i] >>= 1;
            }
        }
        mx = 0;
        for (unsigned i = 0; i < 256; i += 2 * h) { // k = 0: the twiddle 1
            unsigned j = i + h;
            int32_t tr = re[j], ti = im[j];
            re[j] = re[i] - tr;
            im[j] = im[i] - ti;
            re[i] += tr;
            im[i] += ti;
            mx |= iabs(re[i]) | iabs(im[i]) | iabs(re[j]) | iabs(im[j]);
        }
        unsigned step = 512 / h; // the twiddle e^(-j pi k / h), 1024 a turn
        for (unsigned k = 1; k < h; k++) {
            int32_t c = c2_sintab[(k * step + 256) & 1023], s = c2_sintab[(k * step) & 1023];
            for (unsigned i = k; i < 256; i += 2 * h) {
                unsigned j = i + h;
                int32_t tr, ti;
                if (hp) {
                    tr = mulq15(re[j], c) + mulq15(im[j], s);
                    ti = mulq15(im[j], c) - mulq15(re[j], s);
                } else {
                    tr = (re[j] * c + im[j] * s) >> 15;
                    ti = (im[j] * c - re[j] * s) >> 15;
                }
                re[j] = re[i] - tr;
                im[j] = im[i] - ti;
                re[i] += tr;
                im[i] += ti;
                mx |= iabs(re[i]) | iabs(im[i]) | iabs(re[j]) | iabs(im[j]);
            }
        }
    }
}

// X[k] of the 512 point real transform from A = Z[k], B = conj(Z[256 - k]) of the 256 point complex one
// (hp: two products a multiplication, else one: |A|, |B| < 2^15.5)
static void split(int32_t ar, int32_t ai, int32_t br, int32_t bi, unsigned k, int32_t *xr, int32_t *xi, bool hp)
{
    int32_t er = (ar + br) >> 1, ei = (ai + bi) >> 1, or_ = (ar - br) >> 1, oi = (ai - bi) >> 1;
    int32_t s = c2_sintab[(2 * k) & 1023], c = c2_sintab[(2 * k + 256) & 1023];
    if (hp) {
        *xr = er + mulq15(oi, c) - mulq15(or_, s); // + O (-sin - j cos)
        *xi = ei - mulq15(or_, c) - mulq15(oi, s);
    } else {
        *xr = er + ((oi * c - or_ * s) >> 15);
        *xi = ei - ((or_ * c + oi * s) >> 15);
    }
}

// the 512 point real transform of x, loaded as z[m] = x[2m] + j x[2m + 1] into re, im (|x| < 2^28 hp, else 2^14):
// X[0 .. 256] into re, im (|X| < 2^31)
static void rfft512(int32_t *re, int32_t *im, unsigned h0, bool hp)
{
    fft256(re, im, h0, hp);
    int32_t zr0 = re[0], zi0 = im[0];
    for (unsigned k = 1; k < 128; k++) {
        int32_t ar = re[k], ai = im[k], br = re[256 - k], bi = im[256 - k];
        split(ar, ai, br, -bi, k, &re[k], &im[k], hp);
        split(br, bi, ar, -ai, 256 - k, &re[256 - k], &im[256 - k], hp);
    }
    split(re[128], im[128], re[128], -im[128], 128, &re[128], &im[128], hp);
    re[0] = (zr0 >> 1) + (zi0 >> 1); // like the other bins: (A + B) / 2
    im[0] = 0;
    re[256] = (zr0 >> 1) - (zi0 >> 1);
    im[256] = 0;
}

// the shift that brings re, im [lo, hi] below 2^15
static int shift15(const int32_t *re, const int32_t *im, unsigned lo, unsigned hi)
{
    uint32_t mx = 0;
    for (unsigned k = lo; k <= hi; k++) {
        mx |= iabs(re[k]) | iabs(im[k]);
    }
    int sh = norm_shift(mx, 15);
    return sh > 0 ? sh : 0;
}

// the windowed input's spectrum: re, im (bins 0 .. 256, below 2^15) and pw
static void spectrum(c2enc_t *e)
{
    uint32_t mx = 0;
    for (unsigned j = 0; j < NW; j++) {
        mx |= iabs(e->sn[W0 + j] * e->hann[j]);
    }
    int sh = norm_shift(mx, SPECTRUM_HP ? 28 : 14);
    // x[n]: the window's second half at n = 0 .. 138, its first half at n = 373 .. 511 (centred on n = 0)
    for (unsigned n = 0; n < 512; n++) {
        int32_t v = 0;
        if (n < HALF) {
            v = e->sn[M / 2 + n] * e->hann[HALF + n];
        } else if (n >= 512 - HALF) {
            v = e->sn[W0 + n - (512 - HALF)] * e->hann[n - (512 - HALF)];
        }
        v = (sh >= 0) ? (v >> sh) : (v << -sh);
        if (n & 1) {
            e->im[n >> 1] = v;
        } else {
            e->re[n >> 1] = v;
        }
    }
    rfft512(e->re, e->im, 1, SPECTRUM_HP);
    sh = shift15(e->re, e->im, 0, 256);
    for (unsigned k = 0; k <= 256; k++) {
        e->re[k] >>= sh;
        e->im[k] >>= sh;
        e->pw[k] = (uint32_t)(e->re[k] * e->re[k]) + (uint32_t)(e->im[k] * e->im[k]);
    }
}

// |X[b]|^2 for b = 0 .. 511 (the upper half mirrors the lower: a real input)
static uint32_t pw_at(const c2enc_t *e, unsigned b)
{
    return e->pw[(b <= 256) ? b : 512 - b];
}

/* ---- the pitch estimator (9.3) ---- */

// the 80 new samples through the square, the DC notch and the low pass; the DFT of the 64 kept samples; the
// global peak and its sub-multiples; returns the f0 bin (8000 / 2560 Hz a bin)
static unsigned nlp(c2enc_t *e)
{
    memmove(e->dec, &e->dec[16], 48 * sizeof(e->dec[0]));
    for (unsigned j = 0; j < N; j++) {
        int32_t s = e->sn[M - N + j];
        int64_t x = (int64_t)(s * s) << 8; // Q8: the notch's rounding well below its input
        int64_t y = x - e->nlp_x1 + e->nlp_y1 - div20(e->nlp_y1); // y = x - x1 + 0.95 y1
        e->nlp_x1 = x;
        e->nlp_y1 = y;
        e->fir[e->fir_pos] = y;
        e->fir_pos = (e->fir_pos + 1 == 48) ? 0 : e->fir_pos + 1;
        if (j % 5 == 0) {
            int64_t acc = 65540LL << 8; // the reference's + 1 after the notch, through the filter
            unsigned p = e->fir_pos, q = (p == 0) ? 47 : p - 1; // the oldest, the newest
            for (unsigned k = 0; k < 24; k++) { // symmetric: tap k and 47 - k together
                acc += (e->fir[p] + e->fir[q]) * nlp_fir[k];
                p = (p + 1 == 48) ? 0 : p + 1;
                q = (q == 0) ? 47 : q - 1;
            }
            e->dec[48 + j / 5] = acc;
        }
    }
    PROF(0);
    uint64_t mx = 0;
    for (unsigned i = 0; i < C2ENC_DEC; i++) {
        mx |= (uint64_t)(e->dec[i] < 0 ? -e->dec[i] : e->dec[i]);
    }
    int sh = (int)((mx >> 32) ? 32 + norm_shift((uint32_t)(mx >> 32), 0) : norm_shift((uint32_t)mx, 0)) - 28;
    // the spectrum of the 64 windowed samples (zero padded to 512) in re, im (spectrum() fills them again after):
    // z[m] = x[2m] + j x[2m + 1], m < 32, lands at 8 bitrev5(m) in the bit reversed order, and the FFT's first three
    // stages only copy it over its block of 8 (the rest zeros)
    for (unsigned m = 0; m < C2ENC_DEC / 2; m++) {
        int32_t v[2];
        for (unsigned h = 0; h < 2; h++) {
            int64_t d = e->dec[2 * m + h];
            v[h] = mulq15((int32_t)((sh >= 0) ? (d >> sh) : (d << -sh)), e->nlp_w[2 * m + h]);
        }
        unsigned r = ((m & 1) << 4) | ((m & 2) << 2) | (m & 4) | ((m & 8) >> 2) | ((m & 16) >> 4);
        for (unsigned i = 8 * r; i < 8 * r + 8; i++) {
            e->re[i] = v[0];
            e->im[i] = v[1];
        }
    }
    fft256(e->re, e->im, 8, true);
    int32_t fr[129], fi[129]; // the bins searched only (15 .. 128)
    uint32_t fm = 0;
    for (unsigned k = 15; k <= 128; k++) {
        split(e->re[k], e->im[k], e->re[256 - k], -e->im[256 - k], k, &fr[k], &fi[k], true);
        fm |= iabs(fr[k]) | iabs(fi[k]);
    }
    sh = norm_shift(fm, 15); // these far below the squared signal's low frequencies
    if (sh < 0) {
        sh = 0;
    }
    uint32_t f[129]; // |F|^2
    for (unsigned k = 15; k <= 128; k++) {
        int32_t r = fr[k] >> sh, i = fi[k] >> sh;
        f[k] = (uint32_t)(r * r) + (uint32_t)(i * i);
    }
    unsigned gbin = 16;
    uint32_t gmax = 0;
    for (unsigned k = 16; k <= 128; k++) {
        if (f[k] > gmax) {
            gmax = f[k];
            gbin = k;
        }
    }
    unsigned cbin = gbin;
    for (unsigned mult = 2; gbin / mult >= 16; mult++) {
        unsigned b = gbin / mult, bmin = b * 4 / 5, bmax = b * 6 / 5;
        if (bmin < 16) {
            bmin = 16;
        }
        // a lower threshold around the last estimate: pitch tracking
        uint64_t thresh = (uint64_t)gmax * ((e->prev_bin > bmin && e->prev_bin < bmax) ? 15 : 30);
        uint32_t lmax = 0;
        unsigned lbin = bmin;
        for (b = bmin; b <= bmax; b++) {
            if (f[b] > lmax) {
                lmax = f[b];
                lbin = b;
            }
        }
        if ((uint64_t)lmax * 100 > thresh && lmax > f[lbin - 1] && lmax > f[lbin + 1]) {
            cbin = lbin;
        }
    }
    e->prev_bin = cbin;
    return cbin;
}

/* ---- the pitch refinement, the voicing (9.4, 9.5) ---- */

// the period (Q16 samples) in [p0 + lo, p0 + hi] whose harmonics (as many as p0 has) sum the most energy
static uint32_t refine(const c2enc_t *e, uint32_t p0, int32_t lo, int32_t hi, int32_t step)
{
    unsigned L = p0 >> 17; // pi / Wo = p / 2
    uint32_t best = p0;
    uint64_t best_e = 0;
    for (int32_t p = (int32_t)p0 + lo; p <= (int32_t)p0 + hi; p += step) {
        uint32_t bf = (uint32_t)((1ULL << 41) / (uint32_t)p); // Wo / r = 512 / p, Q16
        uint32_t cur = bf;
        uint64_t en = 0;
        for (unsigned m = 1; m <= L; m++) {
            en += pw_at(e, (cur + 0x8000) >> 16);
            cur += bf;
        }
        if (en > best_e) {
            best_e = en;
            best = (uint32_t)p;
        }
    }
    return best;
}

// 1: voiced (the MBE fit of the harmonics below 1 kHz, the low / high energy checks); p the period, Q16
static int voicing(const c2enc_t *e, uint32_t p)
{
    unsigned L = p >> 17;
    if ((uint64_t)200 * L * ONE >= (uint64_t)95 * p) { // Wo L >= 0.95 pi
        L--;
    }
    uint32_t bf = (uint32_t)((1ULL << 41) / p); // Wo / r, Q16
    unsigned l1k = L / 4, l2k = L / 2;
    uint64_t sig = 0, elow = 0, ehigh = 0, err = 0;
    for (unsigned m = 1; m <= L; m++) {
        unsigned am = ((((2 * m - 1) * bf) >> 1) + 0x8000) >> 16, bm = ((((2 * m + 1) * bf) >> 1) + 0x8000) >> 16;
        uint64_t a2 = 0;
        for (unsigned i = am; i < bm; i++) {
            a2 += pw_at(e, i);
        }
        if (m <= l1k) {
            sig += a2;
        }
        if (m <= l2k) {
            elow += a2;
        }
        if (m >= l2k) {
            ehigh += a2;
        }
    }
    for (unsigned l = 1; l <= l1k; l++) {
        unsigned al = ((((2 * l - 1) * bf) >> 1) + 0xFFFF) >> 16, bl = ((((2 * l + 1) * bf) >> 1) + 0xFFFF) >> 16;
        int32_t off = (int32_t)((256 * ONE + ONE / 2 - l * bf) >> 16); // the window's centre on harmonic l
        int64_t nr = 0, ni = 0;
        uint32_t den = 0;
        for (unsigned m = al; m < bl; m++) {
            int32_t d = off + (int32_t)m - 256;
            int32_t w = (d >= -WT_MAX && d <= WT_MAX) ? e->wt[d < 0 ? -d : d] : 0;
            nr += (int64_t)e->re[m] * w;
            ni += (int64_t)e->im[m] * w;
            den += (uint32_t)(w * w);
        }
        int64_t ar = den ? (nr << 12) / den : 0, ai = den ? (ni << 12) / den : 0; // the harmonic, Q12 of the window
        for (unsigned m = al; m < bl; m++) {
            int32_t d = off + (int32_t)m - 256;
            int32_t w = (d >= -WT_MAX && d <= WT_MAX) ? e->wt[d < 0 ? -d : d] : 0;
            int64_t er = e->re[m] - ((ar * w) >> 12), ei = e->im[m] - ((ai * w) >> 12);
            err += (uint64_t)(er * er + ei * ei);
        }
    }
    int voiced = sig * 1000 > err * 3981; // snr > 6 dB
    if (!voiced && elow > ehigh * 10) { // the low frequencies 10 dB above the high: voiced after all
        voiced = 1;
    }
    if (voiced) {
        if (ehigh > elow * 10) { // 10 dB below
            voiced = 0;
        }
        if (elow * 10000 < ehigh * 3981 && (uint64_t)p * 60 >= (8000ULL << 16)) { // 4 dB below, f0 <= 60 Hz
            voiced = 0;
        }
    }
    return voiced;
}

// a 10 ms subframe: 1 voiced; *p the period, Q16
static int analyse(c2enc_t *e, const int16_t *pcm, uint32_t *p)
{
    PROF(-1);
    memmove(e->sn, &e->sn[N], (M - N) * sizeof(e->sn[0]));
    memcpy(&e->sn[M - N], pcm, N * sizeof(e->sn[0]));
    unsigned bin = nlp(e);
    PROF(1);
    spectrum(e);
    PROF(2);
    uint32_t p0 = (2560U << 16) / bin;
    p0 = refine(e, p0, -5 * ONE, 5 * ONE, ONE);
    p0 = refine(e, p0, -ONE, ONE, ONE / 4);
    PROF(3);
    if (p0 < 20 * ONE) {
        p0 = 20 * ONE;
    }
    if (p0 > 160 * ONE) {
        p0 = 160 * ONE;
    }
    *p = p0;
    int voiced = voicing(e, p0);
    PROF(4);
    return voiced;
}

/* ---- the LPC, the LSPs (9.6) ---- */

// a b 2^-29, |a|, |b| < 2^29.5: 15 bit halves, 32 bit products
static int32_t mul29(int32_t a, int32_t b)
{
    int32_t ah = a >> 15, al = a & 0x7FFF, bh = b >> 15, bl = b & 0x7FFF;
    return 2 * ah * bh + ((ah * bl) >> 14) + ((al * bh) >> 14) + ((al * bl) >> 29);
}

// sum c[5 - i] T_i(x), x Q29 (|x| <= 1.01), |c| < 2^28: Q28 (the sign decides the root search near a root, where
// the value is tiny)
static int32_t cheb(const int32_t c[6], int32_t x)
{
    int32_t t0 = 1 << 29, t1 = x, sum = c[5] + mul29(c[4], t1);
    for (int i = 2; i <= 5; i++) {
        int32_t t2 = 2 * mul29(x, t1) - t0;
        sum += mul29(c[5 - i], t2);
        t0 = t1;
        t1 = t2;
    }
    return sum;
}

// the same with c >> 15 (|c| < 2^13) at x Q15: one product a step, its error below 2^21
static int32_t cheb15(const int32_t c[6], int32_t x)
{
    int32_t t0 = 1 << 15, t1 = x, sum = c[5] * t0 + c[4] * t1;
    for (int i = 2; i <= 5; i++) {
        int32_t t2 = ((x * t1) >> 14) - t0;
        sum += c[5 - i] * t2;
        t0 = t1;
        t1 = t2;
    }
    return sum;
}

// at x Q30 on the search grid: Q15, Q29 where the value is too close to 0 for its sign
static int32_t cheb_grid(const int32_t c[6], const int32_t c15[6], int32_t x)
{
    int32_t v = cheb15(c15, x >> 15);
    return (v < (1 << 21) && v > -(1 << 21)) ? cheb(c, x >> 1) : v;
}

static void cheb_coefs(const float *f, int32_t c[6])
{
    float mx = 0;
    for (int i = 0; i < 6; i++) {
        float a = f[i] < 0 ? -f[i] : f[i];
        if (a > mx) {
            mx = a;
        }
    }
    for (int i = 0; i < 6; i++) {
        c[i] = mx > 0 ? (int32_t)(f[i] * (268435455.0f / mx)) : 0; // 2^28 - 1
    }
}

static bool opposite(int32_t a, int32_t b)
{
    return a && b && ((a < 0) != (b < 0));
}

// the energy (its index) and the LSPs (Hz, Q4) of the window over sn
static unsigned lpc(const c2enc_t *e, int32_t hz[10])
{
    uint32_t mx = 0;
    for (unsigned j = 0; j < NW; j++) {
        mx |= iabs(e->sn[W0 + j] * e->hann[j]);
    }
    for (int i = 0; i < 10; i++) {
        hz[i] = 400 * 16 * i; // pi i / 10: the reference's fallback
    }
    if (!mx) {
        return 0;
    }
    int sh = norm_shift(mx, 14);
    if (sh < 0) {
        sh = 0;
    }
    int16_t x[NW];
    for (unsigned j = 0; j < NW; j++) {
        x[j] = (int16_t)((e->sn[W0 + j] * e->hann[j]) >> sh);
    }
    int64_t r[11];
    for (unsigned k = 0; k <= 10; k++) {
        int64_t acc = 0;
        unsigned n = NW - k, j = 0;
        while (j < n) { // 32 bit sums of 7 products (each below 2^28), then 64
            int32_t part = 0;
            unsigned end = (j + 7 < n) ? j + 7 : n;
            for (; j < end; j++) {
                part += x[j] * x[j + k];
            }
            acc += part;
        }
        r[k] = acc;
    }
    if (r[0] <= 0) {
        return 0;
    }
    // Levinson-Durbin on the normalised autocorrelation
    float rn[11], a[11], t[11];
    for (int k = 0; k <= 10; k++) {
        rn[k] = (float)r[k] / (float)r[0];
        a[k] = 0;
    }
    a[0] = 1;
    float err = 1;
    for (int i = 1; i <= 10; i++) {
        float s = 0;
        for (int j = 1; j < i; j++) {
            s += a[j] * rn[i - j];
        }
        float k = -(rn[i] + s) / err;
        if (k > 1 || k < -1) {
            k = 0;
        }
        for (int j = 1; j < i; j++) {
            t[j] = a[j] + k * a[i - j];
        }
        for (int j = 1; j < i; j++) {
            a[j] = t[j];
        }
        a[i] = k;
        err *= 1 - k * k;
    }
    float g = 0;
    for (int k = 0; k <= 10; k++) {
        g += a[k] * rn[k];
    }
    // the energy: r[0] g scaled back to the reference's window (x = s w 2^(15 - sh) / c)
    uint32_t gq = (g > 0) ? (uint32_t)(g * 1073741824.0f) : 1;
    if (!gq) {
        gq = 1;
    }
    int32_t l2 = c2_ilog2((uint64_t)r[0]) + c2_ilog2(gq) - 30 * ONE + (2 * sh - 30) * ONE + L2_C2;
    int32_t db = (int32_t)(((int64_t)l2 * DB_PER_L2) >> 16);
    int32_t ei = (int32_t)((((int64_t)db * 41943) >> 16) + 452198) >> 16; // 32 (dB + 10) / 50 + 1/2
    ei = (ei < 0) ? 0 : (ei > 31) ? 31 : ei;
    // the LSPs: the roots of P'(x), Q'(x) (x = cos w) after a 15 Hz bandwidth expansion
    for (int k = 0; k <= 10; k++) {
        a[k] *= bw_exp[k];
    }
    float pf[6], qf[6];
    pf[0] = qf[0] = 1;
    for (int i = 1; i <= 5; i++) {
        pf[i] = a[i] + a[11 - i] - pf[i - 1];
        qf[i] = a[i] - a[11 - i] + qf[i - 1];
    }
    for (int i = 0; i < 5; i++) {
        pf[i] *= 2;
        qf[i] *= 2;
    }
    int32_t cp[6], cq[6], root[10];
    cheb_coefs(pf, cp);
    cheb_coefs(qf, cq);
    int32_t cp15[6], cq15[6];
    for (int i = 0; i < 6; i++) {
        cp15[i] = cp[i] >> 15;
        cq15[i] = cq[i] >> 15;
    }
    int roots = 0;
    // the reference's search: a 0.01 grid from x = 1 down, 6 bisections at a sign change; positions Q30, the
    // polynomial at Q29 (on the grid Q15 where that is sure of the sign)
    int32_t xl = 1 << 30, xr = 0, xm = 0;
    for (int j = 0; j < 10; j++) {
        const int32_t *c = (j & 1) ? cq : cp, *c15 = (j & 1) ? cq15 : cp15;
        int32_t psuml = cheb_grid(c, c15, xl);
        bool found = false;
        while (!found && xr >= -(1 << 30)) {
            xr = xl - 10737418; // 0.01
            int32_t psumr = cheb_grid(c, c15, xr);
            if (opposite(psumr, psuml) || !psumr) {
                roots++;
                for (int b = 0; b <= 5; b++) {
                    xm = (xl >> 1) + (xr >> 1);
                    int32_t psumm = cheb(c, xm >> 1);
                    if (psumm && psuml && ((psumm < 0) == (psuml < 0))) {
                        psuml = psumm;
                        xl = xm;
                    } else {
                        xr = xm;
                    }
                }
                root[j] = xm;
                xl = xm;
                found = true;
            } else {
                psuml = psumr;
                xl = xr;
            }
        }
    }
    if (roots == 10) {
        for (int i = 0; i < 10; i++) {
            int32_t xv = root[i] > (1 << 30) ? (1 << 30) : root[i] < -(1 << 30) ? -(1 << 30) : root[i];
            // sin = sqrt((1 - x)(1 + x)), Q15: exact for the small angles where 1 - x^2 loses its bits
            uint64_t prod = ((uint64_t)(uint32_t)((1 << 30) - xv) * (uint32_t)((1 << 30) + xv)) >> 30;
            uint32_t y = isqrt((uint32_t)(prod > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : prod));
            uint32_t ang = c2_cordic_angle((xv >> 15) << 13, (int32_t)y << 13); // acos x, 2^32: 2 pi
            hz[i] = (int32_t)(((uint64_t)ang * 128000) >> 32);                 // 8000 Hz a turn, Q4
        }
    }
    return (unsigned)ei;
}

/* ---- the frame (9.1, 9.7) ---- */

static unsigned lspd_hz(int i, unsigned k)
{
    if (i >= 3 && i <= 5 && k >= 8) {
        return 250 + 50 * (k - 8);
    }
    return 25 * (k + 1);
}

static void put(uint8_t *frame, unsigned *bit, unsigned v, unsigned width)
{
    v ^= v >> 1; // Gray
    for (unsigned i = 0; i < width; i++, (*bit)++) {
        if ((v >> (width - 1 - i)) & 1) {
            frame[*bit >> 3] |= 0x80 >> (*bit & 7);
        }
    }
}

void c2enc_init(c2enc_t *e)
{
    memset(e, 0, sizeof(*e));
    for (unsigned i = 0; i < M; i++) {
        e->sn[i] = 1;
    }
    e->prev_bin = 16; // 50 Hz
    for (unsigned j = 0; j < NW; j++) {
        e->hann[j] = (int16_t)((32768 - icos((uint32_t)(((uint64_t)j << 32) / NW))) >> 1);
    }
    for (unsigned i = 0; i < C2ENC_DEC; i++) {
        e->nlp_w[i] = (int16_t)((32768 - icos((uint32_t)(((uint64_t)i << 32) / (C2ENC_DEC - 1)))) >> 1);
    }
    // the window's spectrum (real, even) at bins d = 0 .. WT_MAX, the window centred on sample 139
    int64_t w[WT_MAX + 1];
    for (unsigned d = 0; d <= WT_MAX; d++) {
        int64_t acc = 0;
        for (unsigned j = 0; j < NW; j++) {
            acc += (int64_t)e->hann[j] * icos((uint32_t)(((int32_t)j - HALF) * (int32_t)d) << 23);
        }
        w[d] = acc;
    }
    for (unsigned d = 0; d <= WT_MAX; d++) {
        e->wt[d] = (int16_t)((w[d] * 4096) / w[0]);
    }
}

void c2enc_encode(c2enc_t *e, const int16_t pcm[C2DEC_SAMPLES], uint8_t frame[C2DEC_BYTES])
{
    uint32_t p;
    unsigned v0 = (unsigned)analyse(e, pcm, &p);
    unsigned v1 = (unsigned)analyse(e, &pcm[N], &p);
    // Wo = 2 pi / p: its index 128 (Wo - Wo_min) / (Wo_max - Wo_min) + 1/2 = 128 (160 / p - 1) / 7 + 1/2
    uint64_t x = (20480ULL << 32) / p; // 128 * 160 / p, Q16
    int64_t wi = (((int64_t)x - (128 << 16)) / 7 + 0x8000) >> 16;
    wi = (wi < 0) ? 0 : (wi > 127) ? 127 : wi;
    int32_t hz[10];
    PROF(-1);
    unsigned ei = lpc(e, hz);
    PROF(5);
    memset(frame, 0, C2DEC_BYTES);
    unsigned bit = 0;
    put(frame, &bit, v0, 1);
    put(frame, &bit, v1, 1);
    put(frame, &bit, (unsigned)wi, 7);
    put(frame, &bit, ei, 5);
    int32_t q = 0; // the quantised LSP so far, Hz Q4
    for (int i = 0; i < 10; i++) {
        int32_t d = hz[i] - q;
        unsigned best = 0;
        int32_t best_err = 0x7FFFFFFF;
        for (unsigned k = 0; k < 32; k++) {
            int32_t err = d - (int32_t)(lspd_hz(i, k) * 16);
            err = err < 0 ? -err : err;
            if (err < best_err) {
                best_err = err;
                best = k;
            }
        }
        q += (int32_t)(lspd_hz(i, best) * 16);
        put(frame, &bit, best, 5);
    }
}
