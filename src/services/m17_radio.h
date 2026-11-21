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

#ifndef M17_RADIO_H
#define M17_RADIO_H

#include <stdbool.h>
#include <stdint.h>

// The M17 receiver and transmitter (m17/README.md): the HR_C7000's recording of the discriminator (8 kHz, as NXDN's) demodulated in
// software (m17_demod.c), the dibits into m17_core.c, the Codec 2 3200 frames through c2dec.c (all in the M17 overlay
// RAM) to the speaker through the prompt FIFO. Started by radio.c on an M17 channel, once the RF and the modem are set
// up as for DMR.

typedef struct {
    bool active;   // a stream is being received
    bool muted;    // not voice (3200), or encrypted: not played
    char src[10];  // the caller ("" not known yet: the LSF lost, its LICH not complete)
    char dst[10];
} m17_call_t;

void m17_radio_start(void);
void m17_radio_stop(void);
bool m17_radio_call(m17_call_t *out); // the call now (false: none)
bool m17_radio_heard_within(uint32_t ms); // a stream frame decoded in the last ms (the scan)
// The PTT: a voice stream from the radio's callsign (the codeplug's) to ALL while on; transmitting until the EoT is
// sent. The channel access number it carries (the channel's, tx_target_apply()); received: any
void m17_radio_set_can(uint8_t can);
void m17_radio_transmit(bool on);
bool m17_radio_transmitting(void);

// counters, read over the cable
typedef struct {
    uint32_t halves, syncs, lost, streams, bad_frames, lsfs, eots, c2_frames, pcm_drops, underruns, calls;
    uint32_t decode_us_max; // the longest two Codec 2 frames took
    uint32_t busy_us_max;   // the longest a half took (demodulation and all)
    uint32_t gain_sum;      // the demodulator's gain at each stream frame (Q8, per symbol level): over streams, the
                            // deviation as received (tests: m17/py/m17_tx_check.py)
} m17_radio_stats_t;
extern volatile m17_radio_stats_t m17_radio_stats;

#endif
