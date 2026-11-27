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
#ifndef YSF_RADIO_H
#define YSF_RADIO_H

#include <stdbool.h>
#include <stdint.h>

// The YSF (System Fusion) receiver and transmitter (ysf/README.md): the HR_C7000's recording of the discriminator
// (8 kHz) demodulated in software (p25_demod.c with YSF's sync: the same C4FM at 4800 symbols/s), the dibits into
// ysf_core.c, the V/D mode 2 ("DN") AMBE+2 frames through our vocoder to the speaker through the prompt FIFO.
// Transmits V/D mode 2 from the radio's callsign to ALL. Voice FR ("VW", IMBE) is received as a call (shown) but not
// played. Started by radio.c on a YSF channel, once the RF and the modem are set up as for DMR.

typedef struct {
    bool active;   // a call is being received
    bool muted;    // not V/D mode 1 or 2 (VW, data): not played
    char src[11];  // the caller ("" not known yet: late entry before its FN 1 frame)
    char dst[11];
    uint8_t dgid;  // its DG-ID (the FICH's squelch code)
} ysf_call_t;

void ysf_radio_start(void);
void ysf_radio_stop(void);
bool ysf_radio_call(ysf_call_t *out);     // the call now (false: none)
bool ysf_radio_heard_within(uint32_t ms); // a frame decoded in the last ms (the scan)
// The channel's DG-ID (tx_target_apply()): transmitted; any received (the call's shown)
void ysf_radio_set_dgid(uint8_t dgid);
void ysf_radio_transmit(bool on);
bool ysf_radio_transmitting(void);

// counters, read over the cable
typedef struct {
    uint32_t halves, missed_halves, busy_us_max;
    uint32_t frames, fich_bad, dch_bad, voice_frames, rep_err, rep_bits, lost, calls, other_dgid;
    uint32_t pcm_drops, underruns, dec_us_max, demod_syncs;
} ysf_radio_stats_t;
extern volatile ysf_radio_stats_t ysf_radio_stats;

#endif
