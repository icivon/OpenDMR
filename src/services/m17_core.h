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

// M17 receive from demodulated dibits and transmit as dibits (the M17 specification: frames, FEC, LSF, LICH), our own
// implementation; no OS,
// no hardware, also built on the PC (m17/c). Dibits as on the air: +3 1, +1 0, -1 2, -3 3; M17_DIBIT_ERASED: not
// received (the HR_C7000's gaps).
#ifndef M17_CORE_H
#define M17_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define M17_DIBIT_ERASED 4
// soft symbols (m17_rx_symbol()): the level x M17_SOFT_ONE (+3: 48, -1: -16 ...), -127 .. 127; M17_SOFT_ERASED: none
#define M17_SOFT_ONE     16
#define M17_SOFT_ERASED  (-128)
#define M17_LSF_BYTES    30 // DST 6, SRC 6, TYPE 2, META 14, CRC 2
#define M17_STREAM_BYTES 16 // two Codec 2 3200 frames

typedef enum {
    M17_RX_NONE,
    M17_RX_LSF,    // rx->lsf: a link setup frame with a good CRC (from the LSF itself or the LICH of 6 stream frames)
    M17_RX_STREAM, // rx->fn, rx->eos, rx->stream: a stream frame
    M17_RX_EOT,    // the end of transmission marker
} m17_rx_event_t;

typedef struct {
    // the frame sync and timing
    uint32_t sync;      // the last 16 dibits (2 bits each), as received
    uint8_t sync_valid; // dibits in it without an erasure
    bool locked;        // in step with the stream frames: the next frame expected 192 dibits on
    uint16_t pos;       // dibits of the frame collected (0: hunting / waiting for its sync)
    uint16_t since;     // locked: dibits since the last frame's start
    uint8_t misses;     // locked: frames in a row without their sync
    uint8_t kind;       // the frame being collected: 1 LSF, 2 stream
    int8_t payload[184];  // its soft symbols
    // the results
    uint8_t lsf[M17_LSF_BYTES];
    bool lsf_valid;
    uint16_t fn;        // the frame number (15 bits)
    bool eos;           // the last stream frame (its frame number following the one before: a frame decoded wrong
                        // with that bit set ended calls in the middle)
    int32_t next_fn;    // the frame number expected next (-1: none yet)
    uint8_t stream[M17_STREAM_BYTES];
    uint16_t metric;    // coded bits of the last frame that differ from the decoded data re-encoded (hard decisions)
    uint16_t soft_metric; // the Viterbi's path cost (the soft values it went against, / M17_SOFT_ONE)
    // the LSF from the LICH: 6 chunks of 40 bits
    uint8_t lich_lsf[M17_LSF_BYTES];
    uint8_t lich_got;   // a bit per chunk
    // counters
    uint32_t lsfs, lsf_bad, streams, stream_bad, eots, lich_ok, lich_bad, syncs_lost;
} m17_rx;

void m17_rx_init(m17_rx *r);
m17_rx_event_t m17_rx_dibit(m17_rx *r, uint8_t dibit);
// the same with a soft symbol (the demodulator's): the Viterbi weighs each coded bit by how sure it is
m17_rx_event_t m17_rx_symbol(m17_rx *r, int8_t z);
// the receiver hunting again (the stream given up)
void m17_rx_reset(m17_rx *r);

// A 6 byte address as text (callsign, "ALL" for the broadcast, "#..." other special ones), out 10 bytes
void m17_address(const uint8_t addr[6], char out[10]);
// The LSF's TYPE: stream mode, voice (3200 bps), not encrypted
bool m17_lsf_voice(const uint8_t lsf[M17_LSF_BYTES]);

/* transmit: frames as dibits (192: 40 ms) */
#define M17_FRAME_DIBITS 192

// an LSF: to dst ("" or NULL: ALL, the broadcast) from src (callsigns, up to 9 characters), voice stream (3200),
// the channel access number can (0 - 15)
void m17_lsf_make(uint8_t lsf[M17_LSF_BYTES], const char *dst, const char *src, uint8_t can);
// the preamble before an LSF (+3, -3 ...), the LSF, a stream frame (its LICH: chunk fn % 6 of lsf), the EoT
void m17_tx_preamble(uint8_t out[M17_FRAME_DIBITS]);
void m17_tx_lsf(const uint8_t lsf[M17_LSF_BYTES], uint8_t out[M17_FRAME_DIBITS]);
void m17_tx_stream(const uint8_t lsf[M17_LSF_BYTES], uint16_t fn, bool last, const uint8_t data[M17_STREAM_BYTES],
                   uint8_t out[M17_FRAME_DIBITS]);
void m17_tx_eot(uint8_t out[M17_FRAME_DIBITS]);

#endif
