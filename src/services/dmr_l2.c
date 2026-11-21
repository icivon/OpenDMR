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
 * OpenDMR, service: DMR layer 2 receive (dmr_l2.h), from ETSI TS 102 361-1:
 *
 * - a burst (6.1, 6.2): payload bits 0 - 107 and 156 - 263 around the middle 48 (108 - 155): a SYNC, or the EMB's first
 *   8 bits, 32 embedded signalling bits and the EMB's last 8; a data burst's Slot Type is bits 98 - 107 and 156 - 165,
 *   its 196 information bits 0 - 97 and 166 - 263.
 * - the block codes from their generator matrices (B.3): each data bit's parity bits as a constant (the matrix's row);
 *   the small codes (Golay (20,8), QR (16,7,6), Hamming (7,4)) decoded by the nearest codeword, the Hamming row and
 *   column codes by their syndromes.
 * - BPTC (196,96) (B.1.1): the bits deinterleaved (index x 181 mod 196), the 13 x 15 matrix corrected by columns
 *   (Hamming (13,9,3)) and rows (Hamming (15,11,3)) in turns; RS (12,9) over GF(2^8) (x^8 + x^4 + x^3 + x^2 + 1, roots
 *   a, a^2, a^3) after the data type's CRC mask.
 * - the embedded LC (B.2.1): 4 x 32 bits read back into the 8 x 16 matrix (column-wise), each row Hamming (16,11,4),
 *   each column's parity, the 5-bit checksum (B.3.11).
 */

#include <string.h>

#include "dmr_l2.h"

const uint64_t dmr_sync_word[DMR_SYNC_KINDS] = {
    0,
    0x755FD7DF75F7ULL, 0xDFF57D75DF5DULL, // BS voice, data
    0x7F7D5DD57DFDULL, 0xD5D7F77FD757ULL, // MS
    0x5D577F7757FFULL, 0xF7FDD5DDFD55ULL, // TDMA direct mode TS1
    0x7DFFD5F55D5FULL, 0xD7557F5FF7F5ULL, // TS2
};

static int bit(const uint8_t *b, int i)
{
    return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

static void set_bit(uint8_t *b, int i, int v)
{
    if (v) {
        b[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    } else {
        b[i >> 3] &= (uint8_t)~(0x80 >> (i & 7));
    }
}

static int popcount32(uint32_t v)
{
    return __builtin_popcount(v);
}

uint64_t dmr_burst_middle(const uint8_t b[DMR_BURST_BYTES])
{
    uint64_t w = 0;
    for (int i = 108; i < 156; i++) {
        w = (w << 1) | (uint64_t)bit(b, i);
    }
    return w;
}

int dmr_sync_kind(uint64_t w, int max_errors)
{
    int best = DMR_SYNC_NONE, best_d = max_errors + 1;
    for (int k = 1; k < DMR_SYNC_KINDS; k++) {
        int d = __builtin_popcountll(w ^ dmr_sync_word[k]);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

/* ---- the block codes: each data bit's parity bits (the generator matrix's rows, B.11 - B.17) ---- */

static const uint16_t golay2087[8] = {0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB};
static const uint16_t qr1676[7] = {0x04F, 0x11E, 0x1B7, 0x1E2, 0x1C9, 0x0E5, 0x073};
static const uint8_t hamming74[4] = {0x5, 0x7, 0x6, 0x3};
static const uint8_t hamming1393[9] = {0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3};
static const uint8_t hamming15113[11] = {0x9, 0xD, 0xF, 0xE, 0x7, 0xA, 0x5, 0xB, 0xC, 0x6, 0x3};
static const uint8_t hamming16114[11] = {0x13, 0x1A, 0x1F, 0x1C, 0x0E, 0x15, 0x0B, 0x16, 0x19, 0x0D, 0x07};

// the parity of k data bits (the first the MSB of data), from their rows
static uint32_t parity_of(uint32_t data, int k, const void *rows, int wide)
{
    uint32_t p = 0;
    for (int i = 0; i < k; i++) {
        if ((data >> (k - 1 - i)) & 1) {
            p ^= wide ? ((const uint16_t *)rows)[i] : ((const uint8_t *)rows)[i];
        }
    }
    return p;
}

// the nearest codeword of a short code (k data bits, p parity bits) to the word w (data then parity): its data, the
// distance into *dist
static uint32_t nearest(uint32_t w, int k, int p, const void *rows, int wide, int *dist)
{
    uint32_t best = 0;
    int best_d = 99;
    for (uint32_t d = 0; d < (1U << k); d++) {
        uint32_t cw = (d << p) | parity_of(d, k, rows, wide);
        int e = popcount32(cw ^ w);
        if (e < best_d) {
            best_d = e;
            best = d;
        }
    }
    *dist = best_d;
    return best;
}

// A Hamming code over bits v[0 .. k + p - 1] (data, then parity): one error corrected. 0 none, 1 corrected, -1 more
// (the syndrome matches no bit)
static int hamming_fix(uint8_t *v, int k, int p, const uint8_t *rows)
{
    uint32_t data = 0, par = 0;
    for (int i = 0; i < k; i++) {
        data = (data << 1) | v[i];
    }
    for (int i = 0; i < p; i++) {
        par = (par << 1) | v[k + i];
    }
    uint32_t s = parity_of(data, k, rows, 0) ^ par;
    if (s == 0) {
        return 0;
    }
    for (int i = 0; i < k; i++) {
        if (rows[i] == s) {
            v[i] ^= 1;
            return 1;
        }
    }
    for (int i = 0; i < p; i++) {
        if (s == (1U << (p - 1 - i))) {
            v[k + i] ^= 1;
            return 1;
        }
    }
    return -1;
}

/* ---- Slot Type, EMB, TACT ---- */

bool dmr_slot_type(const uint8_t b[DMR_BURST_BYTES], uint8_t *cc, uint8_t *data_type)
{
    uint32_t w = 0;
    for (int i = 98; i < 108; i++) {
        w = (w << 1) | (uint32_t)bit(b, i);
    }
    for (int i = 156; i < 166; i++) {
        w = (w << 1) | (uint32_t)bit(b, i);
    }
    int d;
    uint32_t v = nearest(w, 8, 12, golay2087, 1, &d);
    *cc = (uint8_t)(v >> 4);
    *data_type = (uint8_t)(v & 0x0F);
    return d <= 3;
}

bool dmr_emb(const uint8_t b[DMR_BURST_BYTES], uint8_t *cc, uint8_t *pi, uint8_t *lcss)
{
    uint32_t w = 0;
    for (int i = 108; i < 116; i++) {
        w = (w << 1) | (uint32_t)bit(b, i);
    }
    for (int i = 148; i < 156; i++) {
        w = (w << 1) | (uint32_t)bit(b, i);
    }
    int d;
    uint32_t v = nearest(w, 7, 9, qr1676, 1, &d);
    *cc = (uint8_t)(v >> 3);
    *pi = (uint8_t)((v >> 2) & 1);
    *lcss = (uint8_t)(v & 3);
    return d <= 2;
}

bool dmr_tact(uint32_t cach, uint8_t *at, uint8_t *tc, uint8_t *lcss)
{
    // B.9: AT, TC, LS(1), LS(0), H(2), H(1), H(0) at TX(23), TX(19), TX(15), TX(11), TX(9), TX(5), TX(1)
    static const uint8_t pos[7] = {23, 19, 15, 11, 9, 5, 1};
    uint32_t w = 0;
    for (int i = 0; i < 7; i++) {
        w = (w << 1) | ((cach >> pos[i]) & 1);
    }
    int d;
    uint32_t v = nearest(w, 4, 3, hamming74, 0, &d);
    *at = (uint8_t)(v >> 3);
    *tc = (uint8_t)((v >> 2) & 1);
    *lcss = (uint8_t)(v & 3);
    return d <= 1;
}

void dmr_embedded(const uint8_t b[DMR_BURST_BYTES], uint8_t out[4])
{
    memset(out, 0, 4);
    for (int i = 0; i < 32; i++) {
        set_bit(out, i, bit(b, 116 + i));
    }
}

void dmr_voice_frames(const uint8_t b[DMR_BURST_BYTES], uint8_t out[3][DMR_AMBE_BYTES])
{
    memset(out, 0, 3 * DMR_AMBE_BYTES);
    for (int i = 0; i < 216; i++) {
        int src = (i < 108) ? i : (i + 48);
        set_bit(out[i / 72], i % 72, bit(b, src));
    }
}

/* ---- BPTC (196,96) ---- */

bool dmr_bptc196(const uint8_t b[DMR_BURST_BYTES], uint8_t out[12])
{
    uint8_t raw[196], m[196]; // m: the encoded matrix, index 0 (R(3)) unused, then 13 rows of 15
    for (int i = 0; i < 196; i++) {
        raw[i] = (uint8_t)bit(b, (i < 98) ? i : (i + 68));
    }
    for (int i = 0; i < 196; i++) {
        m[i] = raw[(i * 181) % 196];
    }
    bool clean = false;
    for (int round = 0; (round < 5) && !clean; round++) {
        clean = true;
        for (int c = 0; c < 15; c++) { // the columns: Hamming (13,9,3)
            uint8_t v[13];
            for (int r = 0; r < 13; r++) {
                v[r] = m[1 + 15 * r + c];
            }
            int f = hamming_fix(v, 9, 4, hamming1393);
            if (f != 0) {
                clean = false;
                for (int r = 0; r < 13; r++) {
                    m[1 + 15 * r + c] = v[r];
                }
            }
        }
        for (int r = 0; r < 9; r++) { // the rows: Hamming (15,11,3)
            int f = hamming_fix(&m[1 + 15 * r], 11, 4, hamming15113);
            if (f != 0) {
                clean = false;
            }
        }
    }
    if (!clean) { // still not: one more look only
        for (int r = 0; r < 9; r++) {
            uint8_t v[15];
            memcpy(v, &m[1 + 15 * r], 15);
            if (hamming_fix(v, 11, 4, hamming15113) != 0) {
                return false;
            }
        }
        for (int c = 0; c < 15; c++) {
            uint8_t v[13];
            for (int r = 0; r < 13; r++) {
                v[r] = m[1 + 15 * r + c];
            }
            if (hamming_fix(v, 9, 4, hamming1393) != 0) {
                return false;
            }
        }
    }
    // the information: row 0 columns 3 - 10 (I(95) - I(88)), rows 1 - 8 columns 0 - 10
    memset(out, 0, 12);
    int k = 0;
    for (int r = 0; r < 9; r++) {
        for (int c = (r == 0) ? 3 : 0; c < 11; c++) {
            set_bit(out, k++, m[1 + 15 * r + c]);
        }
    }
    return true;
}

/* ---- RS (12,9) ---- */

static uint8_t gf_exp[512], gf_log[256];
static bool gf_ready;

static void gf_init(void)
{
    if (gf_ready) {
        return;
    }
    uint32_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) {
            x ^= 0x11D;
        }
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
    gf_ready = true;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return (a && b) ? gf_exp[gf_log[a] + gf_log[b]] : 0;
}

bool dmr_full_lc_check(uint8_t lc[12], uint32_t mask)
{
    gf_init();
    uint8_t c[12];
    memcpy(c, lc, 12);
    c[9] ^= (uint8_t)(mask >> 16);
    c[10] ^= (uint8_t)(mask >> 8);
    c[11] ^= (uint8_t)mask;
    // the syndromes: the codeword (c[0] the x^11 term) at a, a^2, a^3
    uint8_t s[3];
    for (int j = 0; j < 3; j++) {
        uint8_t a = gf_exp[j + 1], v = 0;
        for (int i = 0; i < 12; i++) {
            v = (uint8_t)(gf_mul(v, a) ^ c[i]);
        }
        s[j] = v;
    }
    if (!s[0] && !s[1] && !s[2]) {
        return true;
    }
    // one wrong byte e at degree L: s1 = e a^L, s2 = e a^2L, s3 = e a^3L
    if (!s[0] || !s[1] || !s[2]) {
        return false;
    }
    int l1 = (gf_log[s[1]] - gf_log[s[0]] + 255) % 255; // a^L = s2 / s1
    int l2 = (gf_log[s[2]] - gf_log[s[1]] + 255) % 255; // = s3 / s2
    if ((l1 != l2) || (l1 > 11)) {
        return false;
    }
    uint8_t e = gf_exp[(gf_log[s[0]] - l1 + 255) % 255];
    lc[11 - l1] ^= e;
    return true;
}

/* ---- the embedded LC ---- */

void dmr_emb_lc_reset(dmr_emb_lc *e)
{
    memset(e, 0, sizeof(*e));
    e->n = -1;
}

bool dmr_emb_lc_add(dmr_emb_lc *e, uint8_t lcss, const uint8_t frag[4], uint8_t lc[9])
{
    if (lcss == DMR_LCSS_FIRST) {
        e->n = 0;
    } else if ((e->n <= 0) || (lcss == DMR_LCSS_SINGLE)) {
        e->n = -1; // not part of an LC we follow
        return false;
    }
    if (((e->n < 3) && (lcss == DMR_LCSS_LAST)) || ((e->n == 3) && (lcss != DMR_LCSS_LAST))) {
        e->n = -1; // a fragment missing
        return false;
    }
    memcpy(&e->bits[4 * e->n], frag, 4);
    if (++e->n < 4) {
        return false;
    }
    e->n = -1;
    // the 8 x 16 matrix: transmitted bit k is row k % 8, column k / 8
    uint8_t m[8][16];
    for (int k = 0; k < 128; k++) {
        m[k % 8][k / 8] = (uint8_t)bit(e->bits, k);
    }
    for (int r = 0; r < 7; r++) {
        if (hamming_fix(m[r], 11, 5, hamming16114) < 0) {
            return false;
        }
    }
    for (int c = 0; c < 16; c++) {
        int p = 0;
        for (int r = 0; r < 8; r++) {
            p ^= m[r][c];
        }
        if (p) {
            return false;
        }
    }
    // LC(71) - LC(0): rows 0, 1 columns 0 - 10, rows 2 - 6 columns 0 - 9; the checksum rows 2 - 6 column 10
    memset(lc, 0, 9);
    int k = 0, cs = 0;
    for (int r = 0; r < 7; r++) {
        for (int c = 0; c < ((r < 2) ? 11 : 10); c++) {
            set_bit(lc, k++, m[r][c]);
        }
        if (r >= 2) {
            cs = (cs << 1) | m[r][10];
        }
    }
    int sum = 0;
    for (int i = 0; i < 9; i++) {
        sum += lc[i];
    }
    return (sum % 31) == cs;
}

void dmr_lc_parse(const uint8_t lc[9], dmr_lc_t *out)
{
    out->flco = lc[0] & 0x3F;
    out->fid = lc[1];
    out->options = lc[2];
    out->dst = ((uint32_t)lc[3] << 16) | ((uint32_t)lc[4] << 8) | lc[5];
    out->src = ((uint32_t)lc[6] << 16) | ((uint32_t)lc[7] << 8) | lc[8];
}

/* ---- transmit ---- */

static void put_bits(uint8_t *b, int from, uint32_t v, int n)
{
    for (int i = 0; i < n; i++) {
        set_bit(b, from + i, (int)((v >> (n - 1 - i)) & 1));
    }
}

static void set_middle(uint8_t b[DMR_BURST_BYTES], uint64_t w)
{
    for (int i = 0; i < 48; i++) {
        set_bit(b, 108 + i, (int)((w >> (47 - i)) & 1));
    }
}

void dmr_lc_make(uint8_t lc[9], uint8_t flco, uint8_t options, uint32_t dst, uint32_t src)
{
    lc[0] = flco & 0x3F;
    lc[1] = 0;
    lc[2] = options;
    lc[3] = (uint8_t)(dst >> 16);
    lc[4] = (uint8_t)(dst >> 8);
    lc[5] = (uint8_t)dst;
    lc[6] = (uint8_t)(src >> 16);
    lc[7] = (uint8_t)(src >> 8);
    lc[8] = (uint8_t)src;
}

void dmr_full_lc_make(const uint8_t lc[9], uint32_t mask, uint8_t out[12])
{
    // the remainder of m(x) x^3 by g(x) = x^3 + 0x0E x^2 + 0x38 x + 0x40 (B.10)
    gf_init();
    uint8_t r[3] = {0, 0, 0};
    for (int i = 0; i < 9; i++) {
        uint8_t fb = lc[i] ^ r[0];
        r[0] = r[1] ^ gf_mul(fb, 0x0E);
        r[1] = r[2] ^ gf_mul(fb, 0x38);
        r[2] = gf_mul(fb, 0x40);
    }
    memcpy(out, lc, 9);
    out[9] = r[0] ^ (uint8_t)(mask >> 16);
    out[10] = r[1] ^ (uint8_t)(mask >> 8);
    out[11] = r[2] ^ (uint8_t)mask;
}

void dmr_burst_data(uint8_t b[DMR_BURST_BYTES], const uint8_t info[12], uint8_t cc, uint8_t data_type, int sync)
{
    uint8_t m[196]; // the encoded matrix: index 0 R(3), then 13 rows of 15
    memset(m, 0, sizeof(m));
    int k = 0;
    for (int r = 0; r < 9; r++) {
        for (int c = (r == 0) ? 3 : 0; c < 11; c++) {
            m[1 + 15 * r + c] = (uint8_t)bit(info, k++);
        }
        uint32_t d = 0;
        for (int c = 0; c < 11; c++) {
            d = (d << 1) | m[1 + 15 * r + c];
        }
        uint32_t p = parity_of(d, 11, hamming15113, 0);
        for (int i = 0; i < 4; i++) {
            m[1 + 15 * r + 11 + i] = (uint8_t)((p >> (3 - i)) & 1);
        }
    }
    for (int c = 0; c < 15; c++) {
        uint32_t d = 0;
        for (int r = 0; r < 9; r++) {
            d = (d << 1) | m[1 + 15 * r + c];
        }
        uint32_t p = parity_of(d, 9, hamming1393, 0);
        for (int i = 0; i < 4; i++) {
            m[1 + 15 * (9 + i) + c] = (uint8_t)((p >> (3 - i)) & 1);
        }
    }
    memset(b, 0, DMR_BURST_BYTES);
    for (int i = 0; i < 196; i++) {
        int t = (i * 181) % 196; // where encoded bit i goes
        set_bit(b, (t < 98) ? t : (t + 68), m[i]);
    }
    uint32_t st = ((uint32_t)(cc & 0x0F) << 4) | (data_type & 0x0F);
    uint32_t w = (st << 12) | parity_of(st, 8, golay2087, 1);
    put_bits(b, 98, w >> 10, 10);
    put_bits(b, 156, w & 0x3FF, 10);
    set_middle(b, dmr_sync_word[sync]);
}

static void voice_payload(uint8_t b[DMR_BURST_BYTES], const uint8_t ambe[3][DMR_AMBE_BYTES])
{
    memset(b, 0, DMR_BURST_BYTES);
    for (int i = 0; i < 216; i++) {
        set_bit(b, (i < 108) ? i : (i + 48), bit(ambe[i / 72], i % 72));
    }
}

void dmr_burst_voice_sync(uint8_t b[DMR_BURST_BYTES], const uint8_t ambe[3][DMR_AMBE_BYTES], int sync)
{
    voice_payload(b, ambe);
    set_middle(b, dmr_sync_word[sync]);
}

void dmr_burst_voice_emb(uint8_t b[DMR_BURST_BYTES], const uint8_t ambe[3][DMR_AMBE_BYTES], uint8_t cc, uint8_t pi,
                         uint8_t lcss, const uint8_t frag[4])
{
    voice_payload(b, ambe);
    uint32_t e = ((uint32_t)(cc & 0x0F) << 3) | ((uint32_t)(pi & 1) << 2) | (lcss & 3);
    uint32_t w = (e << 9) | parity_of(e, 7, qr1676, 1);
    put_bits(b, 108, w >> 8, 8);
    for (int i = 0; i < 32; i++) {
        set_bit(b, 116 + i, bit(frag, i));
    }
    put_bits(b, 148, w & 0xFF, 8);
}

void dmr_emb_lc_make(const uint8_t lc[9], uint8_t frags[4][4])
{
    uint8_t m[8][16];
    memset(m, 0, sizeof(m));
    int sum = 0;
    for (int i = 0; i < 9; i++) {
        sum += lc[i];
    }
    int cs = sum % 31, k = 0;
    for (int r = 0; r < 7; r++) {
        for (int c = 0; c < ((r < 2) ? 11 : 10); c++) {
            m[r][c] = (uint8_t)bit(lc, k++);
        }
        if (r >= 2) {
            m[r][10] = (uint8_t)((cs >> (6 - r)) & 1); // CS(4) in row 2 .. CS(0) in row 6
        }
        uint32_t d = 0;
        for (int c = 0; c < 11; c++) {
            d = (d << 1) | m[r][c];
        }
        uint32_t p = parity_of(d, 11, hamming16114, 0);
        for (int i = 0; i < 5; i++) {
            m[r][11 + i] = (uint8_t)((p >> (4 - i)) & 1);
        }
    }
    for (int c = 0; c < 16; c++) {
        int p = 0;
        for (int r = 0; r < 7; r++) {
            p ^= m[r][c];
        }
        m[7][c] = (uint8_t)p;
    }
    memset(frags, 0, 16);
    for (int t = 0; t < 128; t++) { // transmitted bit t: row t % 8, column t / 8
        set_bit(frags[t / 32], t % 32, m[t % 8][t / 8]);
    }
}
