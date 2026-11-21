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
 * OpenDMR, service: the NXDN48 receiver (nxdn_radio.h, nxdn/README.md).
 *
 * The modem records its AF input (the FD6818's discriminator, the DMR settings kept) in two halves of 256 samples;
 * the task reads each half as the modem finishes it (every 32 ms) into nxdn_core.c, which finds the frames and decodes
 * them. Voice frames (VCH, AMBE+2 in DMR's bit order) go through our vocoder into the playback's queue of 20 ms frames
 * (voice_play.c: into the prompt FIFO, the path the beeps take, which plays while the modem stays in FM).
 * The caller and the group come from the SACCH superframe (VCALL) or a FACCH1; a TX_REL or no voice for CALL_TIMEOUT
 * ends the call. The demodulator's state and the voice queue live in the vocoder's overlay RAM after its own.
 *
 * Transmit (transmit(), the PTT): the recording stopped, the mic's 20 ms frames through our AMBE+2 encoder, 4 a frame
 * (nxdn_core.c: a lead, the preamble, a first frame with the VCALL, voice frames, a last with the TX_REL), each NXDN48
 * symbol sent twice through the P25 physical layer's transmit FIFO (4800 symbols/s), the modem's 4FSK level scaled
 * down to NXDN48's deviation meanwhile (modem_set_mod_scale()); then the recording again.
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "leds.h"
#include "modem.h"
#include "systime.h"
#include "vocoder.h"
#include "radio.h"
#include "radio_hooks.h"
#include "p25_phy.h"
#include "nxdn_core.h"
#include "nxdn_radio.h"
#include "ui.h"
#include "last_heard.h"
#include "dmr_ids.h"
#include "flash.h"
#include "dmr_compat.h"
#include "voice_play.h"
#include "afrx.h"

#define PCM_FRAMES     16   // 320 ms of decoded voice
#define PLAY_START     6    // frames queued before the playback starts: they come 4 at a time every 80 ms, a frame
                            // decoded once its last half (32 ms) is read
#define CALL_TIMEOUT   320  // ms without a voice frame: the call ended (a frame is 80 ms)
#define TAP_SAMPLES    2048 // the last samples as recorded, for tools over the cable (256 ms)
#define VCH_LOG        256  // the last voice frames received, for tools over the cable (5 s)
#define TX_LEAD_MAX    240  // symbols of lead at most (100 ms)

#define OVERLAY_BSS __attribute__((section(".nxdn_bss"))) // after the vocoder's RAM (ckcpu.ld), not zeroed

volatile nxdn_radio_stats_t nxdn_radio_stats;

static nxdn_rx rx OVERLAY_BSS;
static nxdn_sacch_super super OVERLAY_BSS;
static int16_t pcm[PCM_FRAMES][VOICE_PLAY_SAMPLES] OVERLAY_BSS; // the playback's queue
int16_t nxdn_tap[TAP_SAMPLES] OVERLAY_BSS; // the recording, nxdn_tap_n samples in so far (ring)
volatile uint32_t nxdn_tap_n;
volatile uint32_t nxdn_tap_hold; // not 0 (over the cable): the tap kept as it is, to be read out whole
// what the tap keeps (over the cable): 0 the recording, 1 the vocoder's output, 2 the samples into the FIFO
volatile uint32_t nxdn_tap_source;

static void tap_put(const int16_t *x, int n)
{
    if (nxdn_tap_hold) {
        return;
    }
    for (int i = 0; i < n; i++) {
        nxdn_tap[(nxdn_tap_n + (uint32_t)i) % TAP_SAMPLES] = x[i];
    }
    nxdn_tap_n += (uint32_t)n;
}
// A test of the vocoder and the playback without the air (over the cable, nxdn/py/flash_play.py): AMBE frames (9
// bytes each) in the flash window from nxdn_play_address, nxdn_play_frames of them played 4 every 80 ms as a call
// would bring them, around and around until nxdn_play_frames is 0 again; the received voice ignored meanwhile
volatile uint32_t nxdn_play_address, nxdn_play_frames;
static uint32_t play_index;
static TickType_t play_next;
uint8_t nxdn_vch_log[VCH_LOG][9] OVERLAY_BSS; // the voice frames as received (ring, nxdn_vch_log_n so far), for
volatile uint32_t nxdn_vch_log_n;              // nxdn/py/vch_check.py against the frames sent

/* transmit: the PTT (nxdn_radio_transmit()), where to (nxdn_radio_set_target()), settings and counters over the cable */
static volatile uint16_t tx_src, tx_dst = 1;
static volatile uint8_t tx_ran = 1;
static volatile bool tx_individual;
static nxdn_tx tx OVERLAY_BSS;
static uint8_t tx_ambe[4][9] OVERLAY_BSS;
static uint8_t tx_syms[TX_LEAD_MAX + NXDN_PREAMBLE_SYMS + NXDN_FRAME_SYMS] OVERLAY_BSS;
static uint8_t tx_dibits[2 * NXDN_FRAME_SYMS] OVERLAY_BSS;
// the deviation: the modem's 4FSK level (+3 = 1944 Hz, as DMR; nxdn/README.md) times this / 1024: NXDN48's 1050 Hz
volatile uint32_t nxdn_tx_dev = 553;
volatile uint32_t nxdn_tx_lead = 144;   // symbols (+3, -3 ...) before the preamble: the first voice frame's time
volatile uint32_t nxdn_tx_low_power;    // not 0: 50 mW (tests with the radios side by side)
volatile uint32_t nxdn_tx_source;       // 0 the mic; 1 the AMBE frames of the flash test (nxdn_play_address)
volatile uint32_t nxdn_tx_stats[4];     // calls, frames, encoder frames, underruns (the last call's)

static nxdn_call_t call;
static bool ignored; // the call on the air is not for us (another unit called, another RAN)
static TickType_t last_voice;
static volatile TickType_t heard_at;
static volatile bool heard;


/* ---- the call ---- */

static struct {
    bool active;
    uint16_t src;
    uint32_t tg_or_pc;
} published;

uint32_t nxdn_call_tg_or_pc(const nxdn_call_t *c)
{
    return (c->type == 4) ? (((uint32_t)LAST_HEARD_PC_FLAG << 24) | c->dst) : c->dst;
}

static void call_publish(void)
{
    uint32_t tg_or_pc = nxdn_call_tg_or_pc(&call);
    char text[LAST_HEARD_TEXT_SIZE];

    if ((call.active == published.active) && (call.src == published.src) && (tg_or_pc == published.tg_or_pc)) {
        return;
    }
    if (call.active && (call.src != 0) && ((call.src != published.src) || (tg_or_pc != published.tg_or_pc))) {
        dmr_ids_caller_text(call.src, text, sizeof(text));
        last_heard_add(call.src, tg_or_pc, LAST_HEARD_TS_NXDN, text[0] ? text : NULL);
    }
    published.active = call.active;
    published.src = call.src;
    published.tg_or_pc = tg_or_pc;

    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

static void call_begin(void)
{
    if (!call.active) {
        memset(&call, 0, sizeof(call));
        call.active = true;
        vocoderReset();
        nxdn_radio_stats.calls++;
    }
}

static void call_end(void)
{
    call.active = false;
    voice_play_end(); // what is queued played out, then the speaker off
    memset(&super, 0, sizeof(super));
    call_publish();
}

// a VCALL or TX_REL (SACCH superframe or FACCH1): who and to whom
static void message(const uint8_t *msg, int len, uint8_t ran)
{
    uint8_t type;
    uint16_t src, dst;

    if (!nxdn_msg_call(msg, len, &type, &src, &dst)) {
        return;
    }
    if ((msg[0] & 0x3F) == NXDN_MSG_TX_REL) {
        if (call.active) {
            call_end();
        }
        return;
    }
    if ((type == NXDN_CALL_INDIVIDUAL) && (dst != tx_src)) { // a unit called, not us
        ignored = true;
        return;
    }
    call_begin();
    call.type = type;
    call.src = src;
    call.dst = dst;
    call.ran = ran;
}

/* ---- the voice ---- */

static void voice(const uint8_t ambe[9])
{
    int16_t *out = voice_play_frame();
    if (!out) { // the playback fell behind: the newest frame dropped
        nxdn_radio_stats.pcm_drops++;
        return;
    }
    memcpy(nxdn_vch_log[nxdn_vch_log_n % VCH_LOG], ambe, 9);
    nxdn_vch_log_n++;
    uint32_t t0 = systime_get_us();
    vocoderDecodeFrame(ambe, out);
    uint32_t took = systime_get_us() - t0;
    if (nxdn_tap_source == 1) {
        tap_put(out, VOICE_PLAY_SAMPLES);
    }
    nxdn_radio_stats.dec_us_sum += took;
    if (took > nxdn_radio_stats.dec_us_max) {
        nxdn_radio_stats.dec_us_max = took;
    }
    voice_play_put();
    nxdn_radio_stats.vch++;
}

static void on_frame(void *ctx, const nxdn_frame_t *f)
{
    (void)ctx;
    nxdn_radio_stats.frames++;
    if (!f->lich_ok) {
        nxdn_radio_stats.lich_bad++;
        return;
    }
    heard = true;
    heard_at = xTaskGetTickCount();
    if (f->sacch_ok) {
        nxdn_radio_stats.sacch_ok++;
        // another RAN than the channel's (0: any): not ours, nothing of it taken
        uint8_t ran = (uint8_t)(f->sacch_sr & 0x3F);
        if ((tx_ran != 0) && (ran != tx_ran)) {
            nxdn_radio_stats.other_ran++;
            if (!call.active) {
                ignored = true;
            }
            return;
        }
    }
    if (ignored && !call.active) { // a call not for us: its frames ignored until a VCALL for us comes
        uint8_t msg[9];
        for (int h = 0; h < 2; h++) {
            if (f->facch_ok & (1u << h)) {
                ignored = false; // judged again from this one
                message(f->facch[h], 10, (uint8_t)(f->sacch_sr & 0x3F));
            }
        }
        if (nxdn_sacch_add(&super, f, msg)) {
            ignored = false;
            message(msg, 9, (uint8_t)(f->sacch_sr & 0x3F));
        }
        return;
    }
    uint8_t msg[9];
    if (nxdn_sacch_add(&super, f, msg)) {
        message(msg, 9, (uint8_t)(f->sacch_sr & 0x3F));
    }
    for (int h = 0; h < 2; h++) {
        if (f->facch_ok & (1u << h)) {
            nxdn_radio_stats.facch_ok++;
            message(f->facch[h], 10, (uint8_t)(f->sacch_sr & 0x3F));
        }
    }
    // a call begins with a frame whose SACCH or FACCH1 CRC holds (LICH parity alone: 1 in 2 by chance), noise
    // decoded as voice otherwise
    if (f->halves && !ignored && (call.active || f->sacch_ok || f->facch_ok) && !nxdn_play_frames) {
        call_begin();
        last_voice = xTaskGetTickCount();
        for (int k = 0; k < 4; k++) {
            if (f->halves & (1u << (k / 2))) {
                voice(f->vch[k]);
            }
        }
    }
    call_publish();
}

/* ---- the playback (voice_play.c) ---- */

// the samples into the FIFO (the tap: nxdn_tap_source 2)
static void play_tap(const int16_t *x, int n)
{
    if (nxdn_tap_source == 2) {
        tap_put(x, n);
    }
}

static const voice_play_cfg_t play_cfg = {
    .frames = pcm,
    .count = PCM_FRAMES,
    .start = PLAY_START,
    .underruns = &nxdn_radio_stats.underruns,
    .writes = &nxdn_radio_stats.feed_writes,
    .tap = play_tap,
};

/* ---- transmit ---- */

// NXDN48 symbols (dibits) into the physical layer's FIFO, each twice (it sends 4800 a second)
static void put(const uint8_t *syms, int n)
{
    while (n > 0) {
        int k = (n > NXDN_FRAME_SYMS) ? NXDN_FRAME_SYMS : n;
        for (int i = 0; i < k; i++) {
            tx_dibits[2 * i] = tx_dibits[2 * i + 1] = syms[i];
        }
        p25_phy_tx_put(tx_dibits, 2 * k);
        syms += k;
        n -= k;
    }
}

/* A transmission while the PTT stays held (afrx.c: the call ended, the playback and the recording off): the lead, the preamble and
   the first frame into the FIFO and the transmitter keyed (the first voice frame comes 4 mic frames and their
   encoding later, ~95 ms: the lead and the first frame cover that, the interrupt taking a block 30 ms ahead); then a
   frame for every 4 mic frames. The PTT released: the frame finished with silence, the last frame (TX_REL); once
   sent, back to receive. */
static void transmit(void)
{
    vocoderReset();
    afrx_fifo_prepare(nxdn_tx_dev);
    nxdn_tx_stats[0]++;

    uint32_t lead = (nxdn_tx_lead > TX_LEAD_MAX) ? TX_LEAD_MAX : nxdn_tx_lead;
    for (uint32_t i = 0; i < lead; i++) {
        tx_syms[i] = (i & 1) ? 3 : 1; // +3, -3
    }
    int n = (int)lead + nxdn_tx_begin(&tx, tx_src, tx_dst, tx_ran,
                                      tx_individual ? NXDN_CALL_INDIVIDUAL : NXDN_CALL_GROUP, &tx_syms[lead]);
    put(tx_syms, n);
    bool started = afrx_fifo_key(nxdn_tx_low_power != 0);
    int16_t pcm[160];
    int halves = 0, nf = 0;
    uint32_t play = 0;
    bool ending = false;
    while (started) {
        if (!afrx_ptt_held()) {
            ending = true;
        }
        if (ending && (nf == 0)) {
            break;
        }
        bool frame = false;
        if (ending) {
            memset(pcm, 0, sizeof(pcm)); // silence to the end of the frame of 4
            frame = true;
        } else if (sound_mic_take(&pcm[80 * halves])) {
            if (++halves == 2) {
                halves = 0;
                frame = true;
            }
        }
        if (!frame) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if ((nxdn_tx_source == 1) && nxdn_play_frames && !ending) {
            if (play >= nxdn_play_frames) {
                play = 0;
            }
            flash_read(nxdn_play_address + (9 * play++), tx_ambe[nf], 9);
        } else {
            vocoderEncodeFrame(pcm, tx_ambe[nf]);
        }
        nxdn_tx_stats[2]++;
        if (++nf < 4) {
            continue;
        }
        nf = 0;
        put(tx_syms, nxdn_tx_voice(&tx, (const uint8_t(*)[9])tx_ambe, tx_syms));
        nxdn_tx_stats[1]++;
    }
    if (started) {
        put(tx_syms, nxdn_tx_end(&tx, tx_syms));
    }
    uint32_t underruns = afrx_fifo_finish(started);
    if (started) {
        nxdn_tx_stats[3] = underruns;
    }
}

void nxdn_radio_set_target(uint8_t ran, uint16_t src, uint16_t dst, bool individual)
{
    tx_ran = ran & 0x3F;
    tx_src = src;
    tx_dst = dst;
    tx_individual = individual;
}

void nxdn_radio_transmit(bool on)
{
    afrx_ptt(on);
}

bool nxdn_radio_transmitting(void)
{
    return afrx_transmitting();
}

/* ---- receiving (afrx.c runs it) ---- */

// the receiver from scratch: starting, and after transmitting
static void rx_init(void)
{
    nxdn_rx_init(&rx, on_frame, NULL);
    memset(&super, 0, sizeof(super));
    memset(&call, 0, sizeof(call));
    vocoderReset();
    ignored = false;
}

static void samples(int16_t *x, int n)
{
    uint32_t t0 = systime_get_us();
    if (nxdn_tap_source == 0) {
        tap_put(x, n);
    }
    nxdn_rx_samples(&rx, x, n);
    nxdn_radio_stats.hunts = rx.hunts;
    nxdn_radio_stats.lost_sync = rx.lost_sync;
    nxdn_radio_stats.demod_us_sum += systime_get_us() - t0;
}

static void poll(void)
{
    if (nxdn_play_frames && ((int32_t)(xTaskGetTickCount() - play_next) >= 0)) { // the flash test
        if (!call.active) {
            call_begin();
            play_next = xTaskGetTickCount();
        }
        for (int k = 0; k < 4; k++) {
            uint8_t ambe[9];
            if (play_index >= nxdn_play_frames) {
                play_index = 0;
            }
            flash_read(nxdn_play_address + (9 * play_index++), ambe, 9);
            voice(ambe);
        }
        last_voice = xTaskGetTickCount();
        play_next += pdMS_TO_TICKS(80);
    }
    if (ignored && ((xTaskGetTickCount() - heard_at) >= pdMS_TO_TICKS(CALL_TIMEOUT))) {
        ignored = false; // the call not for us is over
    }
    if (call.active && ((xTaskGetTickCount() - last_voice) >= pdMS_TO_TICKS(CALL_TIMEOUT))) {
        call_end();
    }
}

static void end_call(void)
{
    if (call.active) {
        call_end();
    }
}

static const afrx_ops_t ops = {
    .bw = AFREC_BW_NXDN,
    .overlay = OVERLAY_AMBE,
    .play = &play_cfg,
    .rx_init = rx_init,
    .samples = samples,
    .poll = poll,
    .end_call = end_call,
    .transmit = transmit,
    .stats = {
        .halves = &nxdn_radio_stats.halves,
        .missed = &nxdn_radio_stats.missed_halves,
        .busy_us_max = &nxdn_radio_stats.busy_max_us,
    },
};

void nxdn_radio_start(void)
{
    if (afrx_start(&ops)) {
        heard = false;
        memset(&published, 0, sizeof(published));
    }
}

void nxdn_radio_stop(void)
{
    if (!afrx_running(&ops)) {
        return;
    }
    afrx_stop();
    call.active = false;
    call_publish();
}

bool nxdn_radio_call(nxdn_call_t *out)
{
    *out = call;
    return call.active;
}

bool nxdn_radio_heard_within(uint32_t ms)
{
    return heard && ((xTaskGetTickCount() - heard_at) < pdMS_TO_TICKS(ms));
}
