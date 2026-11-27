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
// Yaesu System Fusion (YSF, C4FM) layer 2, from Yaesu's "Amateur Radio Digital Standards" rev 1.02 (ysf/facts.md):
// frames from the demodulated dibits and frames as dibits to send. No OS, no hardware; also built on the PC (ysf/c).
#ifndef YSF_CORE_H
#define YSF_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define YSF_FRAME_DIBITS 480 // 100 ms: sync 20, FICH 100, payload 360
#define YSF_SYNC_DIBITS  20
#define YSF_SYNC         0xD471C9634DULL // 20 dibits, the first sent the highest
#define YSF_CALL_LEN     10              // callsigns: 10 characters, space padded

enum { YSF_FI_HC = 0, YSF_FI_CC = 1, YSF_FI_TC = 2, YSF_FI_TEST = 3 };
enum { YSF_DT_VD1 = 0, YSF_DT_DFR = 1, YSF_DT_VD2 = 2, YSF_DT_VFR = 3 };

// the frame information channel's 32 bits
typedef struct {
    uint8_t fi, cs, cm, bn, bt, fn, ft, dev, mr, voip, dt, sql, sc; // sc: the squelch code (DG-ID)
} ysf_fich_t;

void ysf_fich_pack(const ysf_fich_t *f, uint8_t out[4]);
void ysf_fich_unpack(const uint8_t in[4], ysf_fich_t *f);

typedef enum {
    YSF_RX_NONE,
    YSF_RX_FICH,  // a frame's FICH in (decoded: fich_ok, or guessed within a call): fich, sync_wrong
    YSF_RX_VOICE, // a V/D frame's voice segment voice_j in: voice[voice_j], voice_err[voice_j]
    YSF_RX_FRAME, // the frame's end: its data channel(s) below (dch_n); a V/D frame's last voice segment (voice_j 4,
                  // voices 5) in with it
    YSF_RX_LOST,  // the frames' sync lost (frames in a row without sync and FICH)
} ysf_rx_event_t;

typedef struct {
    uint64_t sh;        // the last 20 dibits (2 bits each), as received
    bool locked;        // in step with the frames: the next one expected 480 dibits on
    uint16_t pos;       // dibits of the frame collected (0: waiting for a sync)
    uint16_t since;     // locked: dibits since the last frame's start
    uint8_t misses;     // locked: frames in a row without their sync and FICH
    bool discard;       // the frame being read has no FICH: read to its end (its place kept), dropped
    bool vd;            // the frame being read is V/D (mode 1 or 2): its voice segments decoded as they come
    uint8_t dib[YSF_FRAME_DIBITS];
    // the last frame's results
    bool fich_ok;
    ysf_fich_t fich;
    uint8_t fich_metric; // coded bits the Viterbi went against
    uint8_t sync_wrong;  // dibits of the sync wrong
    uint8_t dch_n;       // its data channels decoded: 0 (none / VW), 1 (V/D), 2 (HC, TC, data FR)
    bool dch_ok[2];      // their CRC
    uint8_t dch[2][20];  // V/D mode 2: 10 bytes, the others 20
    uint8_t voices;      // AMBE+2 frames of the frame decoded so far (V/D 1 and 2: 5 at its end), else 0
    uint8_t voice_j;     // the voice segment just decoded (YSF_RX_VOICE)
    uint8_t voice[5][9]; // as DMR's 72-bit frames (the vocoder's input)
    uint8_t voice_err[5]; // V/D 2: triplets not unanimous (of 27); V/D 1: Golay corrections
    // counters
    uint32_t frames, fich_bad, dch_bad, rep_err, rep_bits, lost;
} ysf_rx;

void ysf_rx_init(ysf_rx *r);
ysf_rx_event_t ysf_rx_dibit(ysf_rx *r, uint8_t dibit);
void ysf_rx_reset(ysf_rx *r); // hunting again

/* transmit: frames as dibits (480: 100 ms) */
// n dibits of the preamble (+3, -3 ...: 0x77)
void ysf_tx_preamble(uint8_t *out, int n);
// a header (YSF_FI_HC) or terminator (YSF_FI_TC) for a V/D mode 2 call: CSD1 = dst + src, CSD2 = spaces; the FICH's
// other fields from f (fn 0)
void ysf_tx_hc_tc(const ysf_fich_t *f, int fi, const char *dst, const char *src, uint8_t out[YSF_FRAME_DIBITS]);
// a V/D mode 2 communication frame: FICH f (fi CC, fn, ft), its DCH (10 bytes, by fn) and 5 voice frames (DMR's 72-bit
// AMBE+2 frames, 9 bytes each)
void ysf_tx_vd2(const ysf_fich_t *f, const uint8_t dch[10], const uint8_t voice[5][9], uint8_t out[YSF_FRAME_DIBITS]);
// the DCH of V/D mode 2 frame fn (FT 6): 0 dst, 1 src, 2 downlink, 3 uplink, 4 Rem1+2, 5 Rem3+Rem4 (radio_id, 5
// characters), 6 DT1 (zeros)
void ysf_vd2_dch(int fn, const char *dst, const char *src, const char *radio_id, uint8_t out[10]);
// 10 bytes of a callsign field as text: trailing spaces cut, NUL terminated (out 11 bytes)
void ysf_callsign(const uint8_t in[10], char out[11]);

#endif
