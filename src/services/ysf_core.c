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
 * OpenDMR, service: YSF layer 2 (ysf_core.h), written from Yaesu's "Amateur Radio Digital Standards" rev 1.02
 * (ysf/facts.md; the Python model ysf/py/ysf.py checked against its test vectors and real recordings):
 *
 * - a frame: sync 0xD471C9634D (40 bits), the FICH (200 bits), the payload (720 bits), 100 ms
 * - FICH: 32 bits + CRC-16 (0x1021, from 0, inverted) -> 4 x Golay(24,12) -> 4 zero tail bits -> rate 1/2 K=5
 *   convolutional code (G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4) -> dibit i at 20 (i mod 5) + i / 5
 * - DCH: data whitened (PN9 x^9 + x^5 + 1 from 0x1C9) -> CRC-16 over the whitened bytes -> tail -> the same code ->
 *   20 rows of 5 (10 bytes, V/D mode 2) or 9 (20 bytes) dibits, cut in 5 pieces over the frame
 * - V/D mode 2 voice: the 49 AMBE+2 bits, the first 27 each sent three times, the other 22, a 0 -> whitened -> bit i at
 *   4 (i mod 26) + i / 26; V/D mode 1: DMR's 72-bit AMBE+2 frame as it is
 *
 * The receiver: hunting, a frame starts at a sync with 3 dibits wrong at most; a frame with a good FICH locks it, the
 * next frame then expected 480 dibits on whatever its sync. A FICH that fails while locked is guessed from the last
 * (the frame number one on), as long as the sync is near right; MAX_MISSES frames in a row without both: lost.
 * A frame is decoded as it comes: its FICH once in (YSF_RX_FICH), each voice segment once in (YSF_RX_VOICE: the first
 * 60 ms before the frame's end), the data channel(s) at the end (YSF_RX_FRAME).
 */

#include <string.h>

#include "ysf_core.h"
#include "ambe.h"
#include "p25_fec.h"

#ifdef __CSKYABI__
#define WORK __attribute__((section(".ysf_bss"))) // the scratch in the receiver's RAM (the AMBE overlay's, ckcpu.ld)
#else
#define WORK
#endif

#define SYNC_HUNT  3 // dibits of the sync wrong at most, hunting
#define SYNC_GUESS 6 // ... locked, for a frame whose FICH is guessed
#define MAX_MISSES 4 // frames in a row without sync and FICH: lost (the standard's N2)
#define MASK40     0xFFFFFFFFFFULL

// the whitening sequence's first 160 bits (PN9 x^9 + x^5 + 1, 0x1C9)
static const uint8_t WHITE[20] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F,
                                  0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8};

static int white(int i)
{
    return (WHITE[i >> 3] >> (7 - (i & 7))) & 1;
}

static int get_bit(const uint8_t *dib, int pos)
{
    return (dib[pos >> 1] >> (1 - (pos & 1))) & 1;
}

static void put_bit(uint8_t *dib, int pos, int b)
{
    uint8_t m = (uint8_t)(1U << (1 - (pos & 1)));
    dib[pos >> 1] = (uint8_t)(b ? (dib[pos >> 1] | m) : (dib[pos >> 1] & ~m));
}

static uint16_t crc16(const uint8_t *d, int n)
{
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)(d[i] << 8);
        for (int k = 0; k < 8; k++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
    }
    return (uint16_t)~c;
}

/* ---- the convolutional code with the interleave: message bits (one a byte) <-> coded bits on the air ---- */

#define MAX_MSG 176 // the 20-byte DCH: 160 + 16 CRC (+ 4 tail)

static struct {
    uint8_t msg[MAX_MSG + 4];
    uint8_t air[2 * (MAX_MSG + 4)];
    uint8_t coded[2 * (MAX_MSG + 4)];
    uint16_t trace[MAX_MSG + 4];
} w WORK;

// position on the air of coded dibit i of a block of 20 rows of cols
static int il(int i, int cols)
{
    return 20 * (i % cols) + i / cols;
}

// n message bits (w.msg) + 4 tail -> w.air
static void conv_il(int n, int cols)
{
    unsigned s = 0;
    for (int i = 0; i < n + 4; i++) {
        unsigned d = (i < n) ? w.msg[i] : 0;
        s = ((s << 1) | d) & 31; // d in bit 0, d1 .. d4 above
        int p = il(i, cols);
        w.air[2 * p] = (uint8_t)__builtin_parity(s & 0x19);     // d + d3 + d4
        w.air[2 * p + 1] = (uint8_t)__builtin_parity(s & 0x17); // d + d1 + d2 + d4
    }
}

// w.air -> n message bits (w.msg), hard decision Viterbi ending in state 0; returns the coded bits it went against
static int unconv_il(int n, int cols)
{
    int steps = n + 4;
    for (int i = 0; i < steps; i++) {
        int p = il(i, cols);
        w.coded[2 * i] = w.air[2 * p];
        w.coded[2 * i + 1] = w.air[2 * p + 1];
    }
    uint16_t cost[16], next[16];
    cost[0] = 0;
    for (int s = 1; s < 16; s++) {
        cost[s] = 0x4000;
    }
    for (int k = 0; k < steps; k++) {
        unsigned r1 = w.coded[2 * k], r2 = w.coded[2 * k + 1];
        uint16_t t = 0;
        for (unsigned s = 0; s < 16; s++) { // s: the 4 newest bits, the newest in bit 0
            unsigned lo = s >> 1, hi = lo | 8; // the state before: its oldest bit 0 or 1
            unsigned reg_lo = (lo << 1) | (s & 1), reg_hi = (hi << 1) | (s & 1);
            uint16_t clo = (uint16_t)(cost[lo] + ((unsigned)__builtin_parity(reg_lo & 0x19) != r1) +
                                      ((unsigned)__builtin_parity(reg_lo & 0x17) != r2));
            uint16_t chi = (uint16_t)(cost[hi] + ((unsigned)__builtin_parity(reg_hi & 0x19) != r1) +
                                      ((unsigned)__builtin_parity(reg_hi & 0x17) != r2));
            if (chi < clo) {
                t |= (uint16_t)(1U << s);
                next[s] = chi;
            } else {
                next[s] = clo;
            }
        }
        w.trace[k] = t;
        memcpy(cost, next, sizeof(cost));
    }
    unsigned s = 0;
    for (int k = steps; k-- > 0;) {
        if (k < n) {
            w.msg[k] = (uint8_t)(s & 1);
        }
        s = (s >> 1) | (((w.trace[k] >> s) & 1) << 3);
    }
    return cost[0];
}

static void bytes_to_msg(const uint8_t *d, int nbytes)
{
    for (int i = 0; i < 8 * nbytes; i++) {
        w.msg[i] = (d[i >> 3] >> (7 - (i & 7))) & 1;
    }
}

static void msg_to_bytes(uint8_t *d, int nbytes)
{
    for (int i = 0; i < nbytes; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) {
            b = (uint8_t)((b << 1) | w.msg[8 * i + k]);
        }
        d[i] = b;
    }
}

/* ---- FICH ---- */

void ysf_fich_pack(const ysf_fich_t *f, uint8_t out[4])
{
    out[0] = (uint8_t)((f->fi << 6) | ((f->cs & 3) << 4) | ((f->cm & 3) << 2) | (f->bn & 3));
    out[1] = (uint8_t)((f->bt << 6) | ((f->fn & 7) << 3) | (f->ft & 7));
    out[2] = (uint8_t)(((f->dev & 1) << 6) | ((f->mr & 7) << 3) | ((f->voip & 1) << 2) | (f->dt & 3));
    out[3] = (uint8_t)(((f->sql & 1) << 7) | (f->sc & 0x7F));
}

void ysf_fich_unpack(const uint8_t in[4], ysf_fich_t *f)
{
    f->fi = in[0] >> 6;
    f->cs = (in[0] >> 4) & 3;
    f->cm = (in[0] >> 2) & 3;
    f->bn = in[0] & 3;
    f->bt = in[1] >> 6;
    f->fn = (in[1] >> 3) & 7;
    f->ft = in[1] & 7;
    f->dev = (in[2] >> 6) & 1;
    f->mr = (in[2] >> 3) & 7;
    f->voip = (in[2] >> 2) & 1;
    f->dt = in[2] & 3;
    f->sql = in[3] >> 7;
    f->sc = in[3] & 0x7F;
}

// the FICH into frame bits 40 .. 239
static void fich_put(const ysf_fich_t *f, uint8_t *frame)
{
    uint8_t d[6];
    ysf_fich_pack(f, d);
    uint16_t c = crc16(d, 4);
    d[4] = (uint8_t)(c >> 8);
    d[5] = (uint8_t)c;
    for (int k = 0; k < 4; k++) { // 12 bits each: byte 0 + high nibble of 1, low nibble of 1 + byte 2, ...
        const uint8_t *p = &d[3 * (k / 2)];
        uint32_t d12 = (k & 1) ? (((uint32_t)(p[1] & 0x0F) << 8) | p[2]) : (((uint32_t)p[0] << 4) | (p[1] >> 4));
        uint32_t cw = p25_golay24_encode(d12);
        for (int i = 0; i < 24; i++) {
            w.msg[24 * k + i] = (uint8_t)((cw >> (23 - i)) & 1);
        }
    }
    conv_il(96, 5);
    for (int i = 0; i < 200; i++) {
        put_bit(frame, 40 + i, w.air[i]);
    }
}

// the FICH from frame bits 40 .. 239; false: not decoded
static bool fich_get(const uint8_t *frame, ysf_fich_t *f, uint8_t *metric)
{
    for (int i = 0; i < 200; i++) {
        w.air[i] = (uint8_t)get_bit(frame, 40 + i);
    }
    int m = unconv_il(96, 5);
    *metric = (uint8_t)((m > 255) ? 255 : m);
    uint32_t v[4];
    for (int k = 0; k < 4; k++) {
        uint32_t cw = 0;
        for (int i = 0; i < 24; i++) {
            cw = (cw << 1) | w.msg[24 * k + i];
        }
        if (p25_golay24_decode(cw, &v[k]) < 0) {
            return false;
        }
    }
    uint8_t d[6] = {(uint8_t)(v[0] >> 4), (uint8_t)((v[0] << 4) | (v[1] >> 8)), (uint8_t)v[1],
                    (uint8_t)(v[2] >> 4), (uint8_t)((v[2] << 4) | (v[3] >> 8)), (uint8_t)v[3]};
    if (crc16(d, 4) != (uint16_t)((d[4] << 8) | d[5])) {
        return false;
    }
    ysf_fich_unpack(d, f);
    return true;
}

/* ---- DCH ---- */

// frame bit of coded bit k of a DCH: V/D mode 2's (n 10) in 40-bit pieces at payload bit 144 j; the 20-byte ones in
// 72-bit pieces at 144 j (DCH-1, V/D mode 1's) or 144 j + 72 (second: DCH-2)
static int dch_bit_pos(int n, int second, int k)
{
    int len = (n == 10) ? 40 : 72;
    return 240 + 144 * (k / len) + (second ? 72 : 0) + (k % len);
}

static void dch_put(const uint8_t *data, int n, int second, uint8_t *frame)
{
    uint8_t wd[22];
    for (int i = 0; i < n; i++) {
        wd[i] = data[i] ^ WHITE[i];
    }
    uint16_t c = crc16(wd, n);
    wd[n] = (uint8_t)(c >> 8);
    wd[n + 1] = (uint8_t)c;
    bytes_to_msg(wd, n + 2);
    conv_il(8 * n + 16, (n == 10) ? 5 : 9);
    for (int k = 0; k < 2 * (8 * n + 20); k++) {
        put_bit(frame, dch_bit_pos(n, second, k), w.air[k]);
    }
}

static bool dch_get(const uint8_t *frame, int n, int second, uint8_t *data)
{
    for (int k = 0; k < 2 * (8 * n + 20); k++) {
        w.air[k] = (uint8_t)get_bit(frame, dch_bit_pos(n, second, k));
    }
    unconv_il(8 * n + 16, (n == 10) ? 5 : 9);
    uint8_t wd[22];
    msg_to_bytes(wd, n + 2);
    if (crc16(wd, n) != (uint16_t)((wd[n] << 8) | wd[n + 1])) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        data[i] = wd[i] ^ WHITE[i];
    }
    return true;
}

/* ---- V/D mode 2 voice ---- */

static void vd2_voice_put(const uint8_t d49[49], uint8_t *frame, int j)
{
    for (int i = 0; i < 104; i++) {
        int b = (i < 81) ? d49[i / 3] : (i < 103) ? d49[i - 54] : 0;
        put_bit(frame, 240 + 144 * j + 40 + 4 * (i % 26) + i / 26, b ^ white(i));
    }
}

// -> the triplets not unanimous
static int vd2_voice_get(const uint8_t *frame, int j, uint8_t d49[49])
{
    int errs = 0;
    uint8_t v[104];
    for (int i = 0; i < 104; i++) {
        v[i] = (uint8_t)(get_bit(frame, 240 + 144 * j + 40 + 4 * (i % 26) + i / 26) ^ white(i));
    }
    for (int k = 0; k < 27; k++) {
        int s = v[3 * k] + v[3 * k + 1] + v[3 * k + 2];
        errs += (s == 1) || (s == 2);
        d49[k] = (uint8_t)(s >= 2);
    }
    for (int k = 27; k < 49; k++) {
        d49[k] = v[k + 54];
    }
    return errs;
}

/* ---- receive ---- */

static int sync_wrong(uint64_t sh)
{
    uint64_t diff = (sh ^ YSF_SYNC) & MASK40;
    int wrong = 0;
    for (int i = 0; i < 20; i++) {
        wrong += ((diff >> (2 * i)) & 3) != 0;
    }
    return wrong;
}

void ysf_rx_init(ysf_rx *r)
{
    memset(r, 0, sizeof(*r));
}

void ysf_rx_reset(ysf_rx *r)
{
    r->locked = false;
    r->discard = false;
    r->vd = false;
    r->pos = 0;
    r->since = 0;
    r->misses = 0;
}

static void start_frame(ysf_rx *r)
{
    for (int i = 0; i < YSF_SYNC_DIBITS; i++) {
        r->dib[i] = (uint8_t)((r->sh >> (2 * (19 - i))) & 3);
    }
    r->pos = YSF_SYNC_DIBITS;
    r->vd = false;
    r->discard = false;
}

// FICH_END: the FICH is in (dibits 0 .. 119); voice segment j of a V/D frame is in at VOICE_END(j) (its bits end at
// 240 + 144 j + 143, in V/D mode 1 and 2 alike)
#define FICH_END     120
#define VOICE_END(j) (192 + 72 * (j))

// the sync and FICH in: the frame's FICH decoded (or guessed), the lock judged
static ysf_rx_event_t fich_in(ysf_rx *r)
{
    const uint8_t *f = r->dib;
    r->dch_n = 0;
    r->voices = 0;
    r->sync_wrong = 0;
    for (int i = 0; i < YSF_SYNC_DIBITS; i++) {
        r->sync_wrong += (uint8_t)(f[i] != ((YSF_SYNC >> (2 * (19 - i))) & 3));
    }
    ysf_fich_t fich;
    r->fich_ok = fich_get(f, &fich, &r->fich_metric);
    if (r->fich_ok) {
        r->fich = fich;
    } else {
        r->fich_bad++;
        if (!r->locked || (r->sync_wrong > SYNC_GUESS)) {
            r->discard = true; // the rest of it read and dropped: the next frame's place kept
            if (r->locked && (++r->misses >= MAX_MISSES)) {
                ysf_rx_reset(r);
                r->lost++;
                return YSF_RX_LOST;
            }
            if (!r->locked) {
                r->pos = 0; // hunting: a false sync
            }
            return YSF_RX_NONE;
        }
        // locked, the sync near right: the FICH guessed from the last one (V/D: the frame number on)
        if (r->fich.fi == YSF_FI_HC) {
            r->fich.fi = YSF_FI_CC;
            r->fich.fn = 0;
        } else {
            r->fich.fn = (uint8_t)((r->fich.fn >= r->fich.ft) ? 0 : r->fich.fn + 1);
        }
    }
    r->locked = (r->fich.fi != YSF_FI_TC); // after a terminator: hunting (the next transmission's timing)
    r->misses = 0;
    r->frames++;
    const ysf_fich_t *h = &r->fich;
    r->vd = (h->fi == YSF_FI_CC) && ((h->dt == YSF_DT_VD2) || (h->dt == YSF_DT_VD1));
    return YSF_RX_FICH;
}

// voice segment j in (a V/D frame): decoded into voice[j] at once (not at the frame's end: 60 ms sooner for the first)
static ysf_rx_event_t voice_in(ysf_rx *r, int j)
{
    const uint8_t *f = r->dib;
    uint8_t d49[49];
    if (r->fich.dt == YSF_DT_VD2) {
        r->voice_err[j] = (uint8_t)vd2_voice_get(f, j, d49);
        r->rep_err += r->voice_err[j];
        r->rep_bits += 27;
        ambe_encode_frame(d49, r->voice[j]);
    } else {
        uint8_t *v = r->voice[j];
        memset(v, 0, 9);
        for (int i = 0; i < 72; i++) {
            v[i >> 3] |= (uint8_t)(get_bit(f, 240 + 144 * j + 72 + i) << (7 - (i & 7)));
        }
        int e = ambe_decode_frame(v, d49);
        r->voice_err[j] = (uint8_t)((e < 0) ? 255 : e);
    }
    r->voice_j = (uint8_t)j;
    r->voices = (uint8_t)(j + 1);
    return YSF_RX_VOICE;
}

// the whole frame in: its data channel(s)
static ysf_rx_event_t frame_in(ysf_rx *r)
{
    const uint8_t *f = r->dib;
    const ysf_fich_t *h = &r->fich;
    r->pos = 0;
    r->since = 0;
    if (r->discard) {
        r->discard = false;
        return YSF_RX_NONE;
    }
    if (r->vd) { // its last voice segment ends with the frame: decoded here, given with YSF_RX_FRAME
        voice_in(r, 4);
    }
    if ((h->fi == YSF_FI_HC) || (h->fi == YSF_FI_TC) || (h->dt == YSF_DT_DFR)) {
        r->dch_n = 2;
        for (int k = 0; k < 2; k++) {
            r->dch_ok[k] = dch_get(f, 20, k, r->dch[k]);
            r->dch_bad += !r->dch_ok[k];
        }
    } else if ((h->dt == YSF_DT_VD2) || (h->dt == YSF_DT_VD1)) {
        int n = (h->dt == YSF_DT_VD2) ? 10 : 20;
        r->dch_n = 1;
        r->dch_ok[0] = dch_get(f, n, 0, r->dch[0]);
        r->dch_bad += !r->dch_ok[0];
    }
    return YSF_RX_FRAME;
}

ysf_rx_event_t ysf_rx_dibit(ysf_rx *r, uint8_t dibit)
{
    r->sh = ((r->sh << 2) | (dibit & 3)) & MASK40;
    if (r->pos) {
        r->dib[r->pos++] = dibit & 3;
        if (r->pos == YSF_FRAME_DIBITS) {
            return frame_in(r);
        }
        if (r->discard) {
            return YSF_RX_NONE;
        }
        if (r->pos == FICH_END) {
            return fich_in(r);
        }
        if (r->vd && (r->pos >= VOICE_END(0)) && (((r->pos - VOICE_END(0)) % 72) == 0)) { // segments 0 .. 3
            return voice_in(r, (r->pos - VOICE_END(0)) / 72);
        }
        return YSF_RX_NONE;
    }
    if (r->locked) {
        // the next frame's sync ends 20 dibits after the last frame; an exact sync elsewhere: a new time line
        if ((++r->since == YSF_SYNC_DIBITS) || (sync_wrong(r->sh) == 0)) {
            start_frame(r);
        }
        return YSF_RX_NONE;
    }
    if (sync_wrong(r->sh) <= SYNC_HUNT) {
        start_frame(r);
    }
    return YSF_RX_NONE;
}

/* ---- transmit ---- */

void ysf_tx_preamble(uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        out[i] = (i & 1) ? 3 : 1; // +3 -3: 0x77
    }
}

static void put_sync(uint8_t *out)
{
    for (int i = 0; i < YSF_SYNC_DIBITS; i++) {
        out[i] = (uint8_t)((YSF_SYNC >> (2 * (19 - i))) & 3);
    }
}

static void pad(uint8_t *out, const char *s, int n)
{
    int i = 0;
    for (; s && s[i] && (i < n); i++) {
        out[i] = (uint8_t)s[i];
    }
    for (; i < n; i++) {
        out[i] = ' ';
    }
}

void ysf_tx_hc_tc(const ysf_fich_t *f, int fi, const char *dst, const char *src, uint8_t out[YSF_FRAME_DIBITS])
{
    ysf_fich_t h = *f;
    h.fi = (uint8_t)fi;
    h.fn = 0;
    memset(out, 0, YSF_FRAME_DIBITS);
    put_sync(out);
    fich_put(&h, out);
    uint8_t csd[20];
    pad(csd, dst, YSF_CALL_LEN);
    pad(csd + YSF_CALL_LEN, src, YSF_CALL_LEN);
    dch_put(csd, 20, 0, out);
    pad(csd, NULL, 20);
    dch_put(csd, 20, 1, out);
}

void ysf_tx_vd2(const ysf_fich_t *f, const uint8_t dch[10], const uint8_t voice[5][9], uint8_t out[YSF_FRAME_DIBITS])
{
    ysf_fich_t h = *f;
    h.fi = YSF_FI_CC;
    h.dt = YSF_DT_VD2;
    memset(out, 0, YSF_FRAME_DIBITS);
    put_sync(out);
    fich_put(&h, out);
    dch_put(dch, 10, 0, out);
    for (int j = 0; j < 5; j++) {
        uint8_t d49[49];
        ambe_decode_frame(voice[j], d49);
        vd2_voice_put(d49, out, j);
    }
}

void ysf_vd2_dch(int fn, const char *dst, const char *src, const char *radio_id, uint8_t out[10])
{
    switch (fn) {
    case 0: pad(out, dst, 10); break;
    case 1: pad(out, src, 10); break;
    case 5:
        pad(out, NULL, 5);
        pad(out + 5, radio_id, 5);
        break;
    case 6: memset(out, 0, 10); break;
    default: pad(out, NULL, 10); break;
    }
}

void ysf_callsign(const uint8_t in[10], char out[11])
{
    int n = 10;
    while ((n > 0) && ((in[n - 1] == ' ') || (in[n - 1] == 0))) {
        n--;
    }
    for (int i = 0; i < n; i++) {
        out[i] = ((in[i] >= 0x20) && (in[i] < 0x7F)) ? (char)in[i] : '?';
    }
    out[n] = 0;
}
