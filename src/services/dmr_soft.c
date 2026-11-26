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
 * OpenDMR, service: the DMR receiver in software (dmr_soft.h, dmr_soft/README.md).
 *
 * As the NXDN and M17 receivers (afrx.c runs it): the modem records the FD6818's discriminator, dmr_demod.c finds
 * the TDMA time line and gives each 30 ms burst; dmr_l2.c decodes its fields. Each burst's timeslot: on a BS signal
 * from the CACH before it (TC: the channel of the burst that follows), in TDMA direct mode from its SYNC, an MS
 * signal (direct mode, one transmitter) taken as the channel's timeslot. Per timeslot:
 *
 * - a data burst (its Slot Type, colour code): a voice LC header starts the call (its LC through the DMR filters,
 *   dmr_tg_filter()), a terminator with LC ends it.
 * - a voice burst: A has the voice SYNC; B - F follow it 60 ms apart (also an A whose SYNC was missed), each with its
 *   EMB (colour code; two bad in a row: the superframe lost) and the embedded LC's fragments: a whole LC starts the
 *   call (late entry), or brings its talker alias and GPS position. The call's voice: the 3 AMBE+2 frames of each burst
 *   through our vocoder into the playback (voice_play.c), one call played at a time.
 * - no voice for CALL_TIMEOUT: the call over.
 *
 * The calls are shown as the engine's (dmr.c: the last heard list, the talker alias).
 *
 * Transmit, direct mode (Rx == Tx: soft_transmit()): the recording off, the bursts made here (dmr_l2.c) and sent
 * through the P25 physical layer's FIFO, whose 30 ms blocks of 144 symbols are DMR's timeslots: ours the burst's 132
 * symbols centred in it (10.2.3.1: 6 symbols of guard each side), the other one empty. The voice LC header (one: the
 * transmitter keyed once the first burst's first two AMBE frames are encoded, so the burst follows it at once), the
 * voice superframes (A the voice SYNC; B - E the LC embedded; F the null message), the last one finished with silence
 * after the PTT, then the terminator with LC. The FIFO run dry (zeros sent) an odd number of blocks: one more put in,
 * the bursts kept in their timeslot.
 *
 * Transmit, repeater mode (Rx != Tx, ETSI TS 102 361-1 4.4.1, 5.2.2.2): the MS's timing is the repeater's. Its bursts
 * (a BS sync, the timeslot from the CACH) are followed as received; once DL_RUN of them in a row, the last fresh and one
 * of them with our colour code, the transmission is planned on their time line: the physical layer's reset (its send
 * timer, p25_phy_tx_begin_at()) at the time that puts our burst's symbol 0 on the air with the repeater's burst of our
 * timeslot (4.2: the centres of the inbound and outbound bursts aligned; the inbound TS1 burst at the outbound TS1's,
 * as the HR_C7000 and MMDVM do: measured, tools/debug/dmr_rmo_timing.py), dmr_soft_rmo_lead_us before the repeater's
 * burst start as received. Then the same bursts as in direct mode, the PA on in our timeslot only, the FD6818 back to
 * the receive frequency in between (radio_rf.c). No repeater: the wake-up (5.2.2.2.4: a CSBK BS_Dwn_Act, ETSI TS
 * 102 361-2), receiving meanwhile (tx_ready()), again every WAKE_RETRY_MS up to the codeplug's attempts, then failed
 * (dmr_repeater_failed(): the TX screen's error). The demodulator's state and the voice queue live after the vocoder's
 * RAM (ckcpu.ld .dmrs_rx, where NXDN's is when NXDN runs).
 */

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "systime.h"
#include "modem.h"
#include "vocoder.h"
#include "p25_phy.h"
#include "p25_radio.h"
#include "dmr_compat.h"
#include "hr_c7000.h"
#include "dmr.h"
#include "afrx.h"
#include "radio_hooks.h"
#include "voice_play.h"
#include "dmr_demod.h"
#include "dmr_l2.h"
#include "dmr_soft.h"
#include "ambe.h"
#include "dmr_data.h"
#include "sms.h"

#define PCM_FRAMES    16  // 320 ms of decoded voice
#define PLAY_START    3   // frames queued before the playback starts (they come 3 at a time every 60 ms, with a
#define PLAY_START_MS 25  // jitter of the recording's 16 ms quarters): the first burst's frames, kept this long
#define CALL_TIMEOUT  400 // ms without a voice burst on the call's timeslot: the call over
#define LOST_TIMEOUT  150 // ... its voice superframe no longer followed (no terminator came)
#define TAP_SAMPLES   2048 // the last samples recorded, for tools over the cable (256 ms)
#define BURST_LOG     32   // the last bursts, for tools over the cable
#define A_MSE_MAX     77   // an A without its SYNC taken (the sync missed) when its symbols fit under 0.30 level^2
                           // (Q8): noise is 0.31 - 0.45, a signal at -116 dBm 0.12 - 0.2
#define TX_HEADERS    1    // voice LC headers before the voice: one (ETSI TS 102 361-1 5.1.2.2, conventional: "a voice
                           // LC header shall be sent", burst A immediately after it); three cost ~120 ms more delay
#define TX_KEY_AFTER  2    // the transmitter keyed once this many AMBE frames of the first burst are encoded: the
                           // header goes out while the third is, the burst ready ~50 ms before it is due. After the
                           // first only, 1 call in 12 ran the FIFO dry between the header and the burst (2026-11-20)
#define TX_GUARD      6    // symbols each side of a burst in its 144
#define TX_END_MS     3000 // the engine's transmission ending (a repeater's hang time) at most, before receiving again
#define DL_RUN        4    // repeater mode: its bursts in a row before transmitting on them (the engine: 4 timecodes)
#define DL_FRESH_MS   100  // ... the last at most this old
#define DL_CC_MS      1000 // ... one with our colour code at most this old
#define WAKE_RETRY_MS 600  // the wake-up again after this long without the repeater (the engine's, the stock firmware's)
#define RMO_SETUP_US  40000 // our first burst at least this far ahead (the transmitter's 30 ms set up, the first voice)

#define OVERLAY_BSS __attribute__((section(".dmrs_bss"))) // after the vocoder's RAM (ckcpu.ld), not zeroed

volatile uint32_t dmr_soft_enabled = 1;
volatile uint32_t dmr_soft_tx = 1, dmr_soft_tx_low_power, dmr_soft_tx_stats[4];
volatile uint32_t dmr_soft_tx_gate = 1; // 0: the PA on in both timeslots (tests)
volatile dmr_soft_stats_t dmr_soft_stats;
// tests against known content (over the cable, the transmitter's vocoder_tx_test): not 0, each AMBE+2 frame played
// judged: by its place in the burst, frames, bad (C0 + C1 corrections > 3), the tone frame after the FEC
volatile uint32_t dmr_soft_tone_check;
volatile uint32_t dmr_soft_tone_stats[3][3];

static dmr_demod dm OVERLAY_BSS;
static int16_t pcm[PCM_FRAMES][VOICE_PLAY_SAMPLES] OVERLAY_BSS;
// the recording (ring, dmr_soft_tap_n samples so far; dmr_soft_tap_hold 1: kept as it is, more: samples still to take
// before that)
int16_t dmr_soft_tap[TAP_SAMPLES] OVERLAY_BSS;
volatile uint32_t dmr_soft_tap_n, dmr_soft_tap_hold;
// the last bursts: the 33 bytes, the sync kind, the timeslot, the mse (Q4 level^2, at most 255), slots since a sync,
// the time its symbol 0 was received (systime_get_us(), little endian; 0: not known yet). Not 0: the log kept as it is
uint8_t dmr_soft_burst_log[BURST_LOG][41] OVERLAY_BSS;
volatile uint32_t dmr_soft_burst_log_n, dmr_soft_burst_log_hold;

typedef struct {
    bool active;      // a call followed: its LC taken
    int pos;          // the last voice burst's place in the superframe: 0 A .. 5 F; -1 none followed
    int bad_emb;      // bursts in a row without a good EMB
    int cc;           // the colour code of the voice followed (its first good EMB; -1 not yet)
    dmr_emb_lc emb;
    TickType_t last;  // the last voice burst or header of the call
} slot_t;

static slot_t slots[2];

/* The data received (ETSI TS 102 361-1 8.2.1.1, 9.2.6 - 9.2.8): per timeslot an unconfirmed packet's header (its CRC
   with the data header mask), then its Rate 1/2 blocks (12 octets each, the last ending with the message CRC-32: B.3.9),
   the packet once all are in. Kept in dmr_soft_data_log (over the cable; the messages' layer later). Confirmed
   packets, Rate 3/4 and 1 blocks: counted only, for now. */
#define DATA_RX_BLOCKS 32 // a packet of at most this many blocks taken (384 octets)
#define DATA_LOG       4
typedef struct {
    bool active;
    uint8_t head[12]; // its header (U_HEAD)
    int blocks, got;
    TickType_t last;
    uint8_t data[DATA_RX_BLOCKS * 12];
} data_rx_t;
static data_rx_t data_rx[2] OVERLAY_BSS;
// the last packets: timeslot, CRC-32 right, SAP, group, source, destination, user data length, the first 64 octets
dmr_soft_data_rx_t dmr_soft_data_log[DATA_LOG];
volatile uint32_t dmr_soft_data_log_n;

// repeater mode: its last burst with a BS sync (dmr_burst_t.at, the timeslot), its run of them, our colour code seen
static struct {
    bool any, cc_seen;
    uint32_t burst, at;
    int ts, run;
    TickType_t last, cc_ok;
} dl;

// repeater mode's wake-ups for the PTT held
static struct {
    int sent;
    TickType_t at;
} wake;
static volatile bool wake_reset; // a new PTT
volatile bool dmr_soft_wake_failed;
// The physical layer's reset for our burst: the repeater's burst's start as received (afrx_sample_us()) less this (the
// receive delay, and the physical layer's from its reset to symbol 0 on the air). Measured with another radio on both
// frequencies (tools/debug/dmr_rmo_timing.py, 2026-11-24, MMDVM_HS): our TS1 bursts +71, -39, +71 us from the
// repeater's (the HR_C7000 engine's +206 us); the hotspot's BER 0.7 - 1.3 % (the engine's 1.3 - 1.9 %) at -60 dBm
volatile int32_t dmr_soft_rmo_lead_us = 8770;
// wake-ups sent, transmissions on the repeater's time line, wakes failed, the last reset's lateness (us), its plan's
// time ahead (us), the repeater's burst it went by: its age (us)
volatile uint32_t dmr_soft_rmo_stats[6];
volatile uint32_t dmr_soft_rmo_plan_clock[2]; // the last plan's recording clock: its halves, their spread (us)
// The PA on for the first burst this long after the physical layer's reset (0: on from the set up, 30 ms before, an
// unmodulated carrier over the other timeslot): the first transmit interrupt comes 3.5 - 3.6 ms after the reset, then
// every 30 ms (measured 2026-11-24); the burst's data starts 28.2 ms after the interrupt before it (radio_rf.c), so the
// first burst's at ~1.8 ms; the PA on 1.3 ms before it, as the gate does for the others. The repeater (MMDVM_HS) logged
// the voice header of every transmission so
volatile int32_t dmr_soft_rmo_pa_us = 500;
static uint32_t burst_n;
static int phase;      // the timeslot of burst n: (n + phase) & 1
static int playing;    // the timeslot played (-1 none)

/* ---- the calls ---- */

volatile uint32_t dmr_soft_tx_realigned;  // the FIFO's timeslot pairs put back in step (put_burst())
volatile uint32_t dmr_soft_log_all_voice; // tests over the cable: every voice burst received into the delay log
static uint32_t rx_voice_logged; // voice bursts of the call in the delay log (p25_radio.c)

static void call_end(int ts)
{
    slot_t *s = &slots[ts];
    s->active = false;
    s->pos = -1;
    if (playing == ts) {
        playing = -1;
        voice_play_end(); // what is queued played out, then the speaker off
        dmr_call_over();
        dmr_soft_call_beeps(false);
    }
}

// a voice call's LC (a header's, an embedded one): the call on ts followed if the filters take it
static void call_lc(int ts, const uint8_t lc[9])
{
    dmr_lc_t p;
    dmr_lc_parse(lc, &p);
    slot_t *s = &slots[ts];
    if ((p.flco != DMR_FLCO_GROUP) && (p.flco != DMR_FLCO_UNIT)) {
        if (s->active && (playing == ts) && (p.flco >= 4) && (p.flco <= 8)) {
            dmr_lc_heard(lc, (uint8_t)ts); // a talker alias block, a GPS position
        }
        return;
    }
    uint32_t tg_or_pc = (p.flco == DMR_FLCO_UNIT) ? (((uint32_t)PC_CALL_FLAG << 24) | p.dst) : p.dst;
    if (!dmr_tg_filter(tg_or_pc, p.src)) {
        dmr_soft_stats.filtered++;
        if (s->active) {
            call_end(ts);
        }
        return;
    }
    s->last = xTaskGetTickCount();
    if (!s->active) {
        s->active = true;
        rx_voice_logged = 0;
        dmr_soft_stats.calls++;
        if (playing < 0) {
            playing = ts;
            vocoderReset();
            voice_play_prepare(); // the speaker on while the voice comes (the header is before it)
            dmr_soft_call_beeps(true);
        }
    }
    if (playing == ts) {
        dmr_lc_heard(lc, (uint8_t)ts);
    }
}

static void voice(const uint8_t b[DMR_BURST_BYTES])
{
    uint8_t ambe[3][DMR_AMBE_BYTES];
    dmr_voice_frames(b, ambe);
    uint32_t t0 = systime_get_us();
    for (int f = 0; f < 3; f++) {
        int16_t *out = voice_play_frame();
        if (!out) { // the playback fell behind: the newest frame dropped
            dmr_soft_stats.pcm_drops++;
            continue;
        }
        vocoderDecodeFrame(ambe[f], out);
        voice_play_put();
        if (dmr_soft_tone_check) {
            static const uint8_t tone[DMR_AMBE_BYTES] = {0xCE, 0xA8, 0xFE, 0x83, 0xAC, 0xC4, 0x58, 0x20, 0x0A};
            uint8_t d[49], ref[49];
            int e = ambe_decode_frame(ambe[f], d);
            ambe_decode_frame(tone, ref);
            dmr_soft_tone_stats[f][0]++;
            dmr_soft_tone_stats[f][1] += (e > 3);
            dmr_soft_tone_stats[f][2] += !memcmp(d, ref, sizeof(d));
        }
    }
    uint32_t took = systime_get_us() - t0;
    if (took > dmr_soft_stats.dec_us_max) {
        dmr_soft_stats.dec_us_max = took;
    }
}

/* ---- data received ---- */

static void data_packet(int ts, data_rx_t *d)
{
    int len = d->blocks * 12 - 4;          // user data and pad octets, then the CRC-32
    uint8_t dpf = d->head[0] & 0x0F;
    int pad = (dpf == DMR_DPF_DEFINED) ? (d->head[9] / 8) : (((d->head[0] & 0x10) ? 16 : 0) | (d->head[1] & 0x0F));
    const uint8_t *c = &d->data[len];
    uint32_t got = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) | ((uint32_t)c[3] << 24);
    bool ok = (dmr_crc32(d->data, len) == got) && (pad <= len);
    if (ok) {
        dmr_soft_stats.data_packets++;
    } else {
        dmr_soft_stats.data_crc_bad++;
    }
    dmr_soft_data_rx_t *e = &dmr_soft_data_log[dmr_soft_data_log_n % DATA_LOG];
    memset(e, 0, sizeof(*e));
    e->ts = (uint8_t)ts;
    e->crc_ok = ok;
    e->sap = d->head[1] >> 4;
    e->group = (d->head[0] & 0x80) != 0;
    e->dst = ((uint32_t)d->head[2] << 16) | ((uint32_t)d->head[3] << 8) | d->head[4];
    e->src = ((uint32_t)d->head[5] << 16) | ((uint32_t)d->head[6] << 8) | d->head[7];
    e->length = (uint16_t)(ok ? (len - pad) : len);
    memcpy(e->data, d->data, (size_t)((e->length < (int)sizeof(e->data)) ? e->length : (int)sizeof(e->data)));
    dmr_soft_data_log_n++;
    if (ok) { // to the text messages (sms.c: decoded and queued, stored later by the input task)
        dmr_data_rx_t p = {.dpf = dpf, .sap = d->head[1] >> 4, .group = e->group, .dst = e->dst, .src = e->src,
                           .data = d->data, .length = len - pad};
        memcpy(p.head, d->head, sizeof(p.head));
        sms_dmr_packet(ts, &p);
    }
}

// a data burst of timeslot ts (its data type dt): a CSBK, a data header, a data block
static void data_burst(int ts, uint8_t dt, const dmr_burst_t *b)
{
    data_rx_t *d = &data_rx[ts];
    uint8_t info[12];
    if (d->active && ((xTaskGetTickCount() - d->last) > pdMS_TO_TICKS(LOST_TIMEOUT))) {
        d->active = false; // its blocks stopped coming
        dmr_soft_stats.data_lost++;
    }
    if ((dt == DMR_DT_RATE_34) || (dt == DMR_DT_RATE_1)) {
        dmr_soft_stats.data_other++;
        d->active = false;
        return;
    }
    if (!dmr_bptc196(b->bits, info)) {
        dmr_soft_stats.data_fec_bad++;
        return;
    }
    if (dt == DMR_DT_CSBK) {
        if (dmr_crc_ccitt_check(info, DMR_MASK_CSBK)) {
            dmr_soft_stats.csbks++;
        }
        return;
    }
    if (dt == DMR_DT_DATA_HEADER) {
        d->active = false;
        if (!dmr_crc_ccitt_check(info, DMR_MASK_DATA_HEADER)) {
            dmr_soft_stats.data_fec_bad++;
            return;
        }
        dmr_soft_stats.data_headers++;
        uint8_t dpf = info[0] & 0x0F;
        int blocks = (dpf == DMR_DPF_UNCONFIRMED) ? (info[8] & 0x7F)
                     : (dpf == DMR_DPF_DEFINED)   ? ((((info[0] >> 4) & 0x03) << 4) | (info[1] & 0x0F)) // appended
                                                  : 0;
        if ((blocks == 0) || (blocks > DATA_RX_BLOCKS)) {
            dmr_soft_stats.data_other++; // not an unconfirmed or defined short data packet (yet)
            return;
        }
        memcpy(d->head, info, sizeof(d->head));
        d->blocks = blocks;
        d->got = 0;
        d->active = true;
        d->last = xTaskGetTickCount();
        return;
    }
    // a Rate 1/2 block
    if (!d->active) {
        return;
    }
    memcpy(&d->data[d->got * 12], info, 12);
    d->last = xTaskGetTickCount();
    if (++d->got == d->blocks) {
        d->active = false;
        data_packet(ts, d);
    }
}

/* ---- the bursts ---- */

static bool cc_ok(uint8_t cc)
{
    if ((nonVolatileSettings.dmrCcTsFilter & DMR_CC_FILTER_PATTERN) && (cc != trxGetDMRColourCode())) {
        dmr_soft_stats.other_cc++;
        return false;
    }
    return true;
}

static bool ts_ok(int ts)
{
    return !(nonVolatileSettings.dmrCcTsFilter & DMR_TS_FILTER_PATTERN) || (ts == trxGetDMRTimeSlot());
}

// A RAS system computes the LC CRC with a secret, so it fails the standard check on purpose. The "DMR crc" option
// (inverted: Off = ignore) must work here too, not only in the HR_C7000 engine (hrc7000CrcIsValid()). The FEC still
// runs: only this last CRC check is skipped.
static bool lc_crc_ok(uint8_t lc[12], uint32_t mask)
{
    if ((settingsUsbMode != USB_MODE_HOTSPOT) && settingsIsOptionBitSet(BIT_DMR_CRC_IGNORED)) {
        return true;
    }
    return dmr_full_lc_check(lc, mask);
}

static void log_burst(const dmr_burst_t *b, int ts)
{
    if (dmr_soft_burst_log_hold) {
        return;
    }
    uint8_t *e = dmr_soft_burst_log[dmr_soft_burst_log_n % BURST_LOG];
    memcpy(e, b->bits, DMR_BURST_BYTES);
    e[33] = (uint8_t)b->sync;
    e[34] = (uint8_t)ts;
    int32_t m = b->mse >> 4;
    e[35] = (uint8_t)((m > 255) ? 255 : m);
    e[36] = (uint8_t)((b->since_sync > 255) ? 255 : b->since_sync);
    uint32_t us = 0;
    afrx_sample_us(b->at, &us);
    memcpy(&e[37], &us, 4);
    dmr_soft_burst_log_n++;
}

static void on_burst(void *ctx, const dmr_burst_t *b)
{
    (void)ctx;
    uint32_t n = burst_n++;
    // the timeslot
    if (DMR_SYNC_IS_BS(b->sync)) {
        uint8_t at, tc, lcss;
        dmr_tact(b->cach, &at, &tc, &lcss);
        phase = (int)((tc - n) & 1);
    } else if ((b->sync == DMR_SYNC_TS1_VOICE) || (b->sync == DMR_SYNC_TS1_DATA)) {
        phase = (int)((0 - n) & 1);
    } else if ((b->sync == DMR_SYNC_TS2_VOICE) || (b->sync == DMR_SYNC_TS2_DATA)) {
        phase = (int)((1 - n) & 1);
    } else if (b->sync != DMR_SYNC_NONE) { // MS: one transmitter, on the channel's timeslot
        phase = (int)(((uint32_t)trxGetDMRTimeSlot() - n) & 1);
    }
    int ts = (int)((n + (uint32_t)phase) & 1);
    slot_t *s = &slots[ts];
    log_burst(b, ts);
    if (DMR_SYNC_IS_BS(b->sync)) { // a repeater's: its time line for repeater mode's transmitting
        TickType_t now = xTaskGetTickCount();
        dl.run = (dl.any && ((n - dl.burst) <= 2)) ? dl.run + 1 : 1;
        dl.any = true;
        dl.burst = n;
        dl.at = b->at;
        dl.ts = ts;
        dl.last = now;
        uint8_t cc, dt;
        if (DMR_SYNC_IS_DATA(b->sync) && dmr_slot_type(b->bits, &cc, &dt) && (cc == trxGetDMRColourCode())) {
            dl.cc_ok = now;
            dl.cc_seen = true;
        }
    }
    if (!ts_ok(ts)) {
        return;
    }
    if (DMR_SYNC_IS_DATA(b->sync)) {
        uint8_t cc, dt, lc[12];
        s->pos = -1;
        if (!dmr_slot_type(b->bits, &cc, &dt) || !cc_ok(cc)) {
            return;
        }
        if ((dt == DMR_DT_CSBK) || (dt == DMR_DT_DATA_HEADER) || (dt == DMR_DT_RATE_12) || (dt == DMR_DT_RATE_34) ||
            (dt == DMR_DT_RATE_1)) {
            data_burst(ts, dt, b);
        } else if ((dt == DMR_DT_VOICE_LC_HEADER) && dmr_bptc196(b->bits, lc) &&
                   lc_crc_ok(lc, DMR_MASK_VOICE_LC_HEADER)) {
            dmr_soft_stats.headers++;
            p25_ev_log(42, (uint16_t)ts); // the delay log (p25_radio.c): a voice LC header received
            call_lc(ts, lc);
        } else if ((dt == DMR_DT_TERMINATOR_LC) && dmr_bptc196(b->bits, lc) &&
                   lc_crc_ok(lc, DMR_MASK_TERMINATOR_LC)) {
            dmr_soft_stats.terminators++;
            if (s->active) {
                call_end(ts);
            }
        }
        return;
    }
    // a voice burst: A (its SYNC), or the next of the superframe followed
    if (DMR_SYNC_IS_VOICE(b->sync)) {
        if (s->pos < 0) {
            s->cc = -1;
        }
        s->pos = 0;
        s->bad_emb = 0;
    } else if (s->pos >= 0) {
        s->pos = (s->pos + 1) % 6;
    } else {
        return;
    }
    if (s->pos > 0) {
        uint8_t cc, pi, lcss, frag[4], lc[9];
        // a good EMB of the voice's colour code (a random word passes the QR (16,7,6) one time in 4)
        bool good = dmr_emb(b->bits, &cc, &pi, &lcss) && ((s->cc < 0) || (cc == s->cc));
        if (good) {
            dmr_soft_stats.emb_ok++;
            s->bad_emb = 0;
            if (!cc_ok(cc)) {
                s->pos = -1;
                return;
            }
            s->cc = cc;
            dmr_embedded(b->bits, frag);
            if (dmr_emb_lc_add(&s->emb, lcss, frag, lc)) {
                dmr_soft_stats.emb_lcs++;
                call_lc(ts, lc);
            }
        } else {
            dmr_soft_stats.emb_bad++;
            if (++s->bad_emb >= 2) { // the voice is over (or too weak to follow)
                s->pos = -1;
                return;
            }
        }
    } else if ((b->sync == DMR_SYNC_NONE) && (s->bad_emb || (b->mse > A_MSE_MAX))) {
        s->pos = -1; // an A without its SYNC: not a voice burst seen (the voice over)
        return;
    }
    dmr_soft_stats.voice_bursts++;
    if (s->active) {
        s->last = xTaskGetTickCount();
        if (playing == ts) {
            if ((rx_voice_logged < 3) || dmr_soft_log_all_voice) {
                p25_ev_log(43, (uint16_t)rx_voice_logged++); // a voice burst received, before its decoding
            }
            voice(b->bits);
        }
    }
}

/* ---- the playback (voice_play.c) ---- */

static const voice_play_cfg_t play_cfg = {
    .frames = pcm,
    .count = PCM_FRAMES,
    .start = PLAY_START,
    .start_ms = PLAY_START_MS,
    .underruns = &dmr_soft_stats.underruns,
};

/* ---- receiving (afrx.c runs it) ---- */

static void rx_init(void)
{
    dmr_demod_init(&dm, on_burst, NULL);
    memset(slots, 0, sizeof(slots));
    for (int ts = 0; ts < 2; ts++) {
        slots[ts].pos = -1;
        slots[ts].cc = -1;
        dmr_emb_lc_reset(&slots[ts].emb);
    }
    playing = -1;
    burst_n = 0;
    phase = 0;
    memset(&dl, 0, sizeof(dl));
    memset(data_rx, 0, sizeof(data_rx));
    vocoderReset();
}

static void samples(int16_t *x, int n)
{
    for (int i = 0; (i < n) && (dmr_soft_tap_hold != 1); i++) {
        dmr_soft_tap[dmr_soft_tap_n % TAP_SAMPLES] = x[i];
        dmr_soft_tap_n++;
        if (dmr_soft_tap_hold > 1) {
            dmr_soft_tap_hold--;
        }
    }
    dmr_demod_samples(&dm, x, n);
    dmr_soft_stats.bursts = dm.bursts;
    dmr_soft_stats.syncs = dm.syncs;
    dmr_soft_stats.lost = dm.lost;
    dmr_soft_stats.relocks = dm.relocks;
}

static void poll(void)
{
    TickType_t now = xTaskGetTickCount();
    for (int ts = 0; ts < 2; ts++) {
        uint32_t timeout = (slots[ts].pos < 0) ? LOST_TIMEOUT : CALL_TIMEOUT;
        if (slots[ts].active && ((now - slots[ts].last) >= pdMS_TO_TICKS(timeout))) {
            call_end(ts);
        }
    }
}

static void end_call(void)
{
    for (int ts = 0; ts < 2; ts++) {
        if (slots[ts].active) {
            call_end(ts);
        }
    }
}

/* ---- transmit in software (direct mode) ---- */

// the transmission's buffers off the stack: the AMBE encoder takes most of it
static struct {
    uint8_t lc[9], full[12], frags[4][4], b[DMR_BURST_BYTES], ambe[3][DMR_AMBE_BYTES], d[144];
    int16_t pcm[160];
} txm OVERLAY_BSS;

// a timeslot pair into the FIFO: ours (the guard, the burst, the guard), then the other one empty. The FIFO run dry
// sends a block of zeros in its place: an odd number of them would move every burst after into the other timeslot (a
// receiver following the first ones lost all the rest of the call, 2026-11-20): one more then, the pairs in step again
static uint32_t tx_underruns_seen;

static void put_burst(const uint8_t b[DMR_BURST_BYTES])
{
    uint8_t *d = txm.d;
    memset(d, 0, sizeof(txm.d));
    uint32_t u = p25_phy_tx_underrun;
    if ((u - tx_underruns_seen) & 1) {
        p25_phy_tx_put(d, sizeof(txm.d)); // zeros: the other timeslot's block
        dmr_soft_tx_realigned++;
        p25_ev_log(38, (uint16_t)u); // the delay log: the timeslots put back in step (arg underruns so far)
    }
    tx_underruns_seen = u;
    for (int k = 0; k < 132; k++) {
        d[TX_GUARD + k] = (uint8_t)((b[k >> 2] >> (6 - 2 * (k & 3))) & 3);
    }
    while (p25_phy_tx_room() < 2 * sizeof(txm.d)) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    p25_phy_tx_put(d, sizeof(txm.d));
    memset(d, 0, sizeof(txm.d));
    p25_phy_tx_put(d, sizeof(txm.d));
    dmr_soft_tx_stats[1]++;
}

// rmo: repeater mode, the transmitter keyed at once, its timing from reset_at (rmo_plan()); else direct mode
static void soft_transmit(bool rmo, uint32_t reset_at)
{
    static const uint8_t silence[DMR_AMBE_BYTES] = {0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B};
    static const uint8_t lcss[5] = {DMR_LCSS_FIRST, DMR_LCSS_CONT, DMR_LCSS_CONT, DMR_LCSS_LAST, DMR_LCSS_SINGLE};
    static const uint8_t null_frag[4] = {0, 0, 0, 0};
    uint8_t *lc = txm.lc, *full = txm.full, *b = txm.b;
    uint8_t (*frags)[4] = txm.frags;
    uint8_t (*ambe)[DMR_AMBE_BYTES] = txm.ambe;
    uint8_t cc = trxGetDMRColourCode();
    bool pc = (trxTalkGroupOrPcId >> 24) == PC_CALL_FLAG;
    // The engine inserts the ETSI GPS Info LC in DMR voice; do the same here (TA Tx = APRS/Both), so the other
    // radios can show our distance. Without this, TA Tx was ignored on DMO's software transmitter.
    uint8_t gps_lc[9], gps_frags[4][4];
    bool send_gps = (settings.location_lat != SETTINGS_UNITIALISED_LOCATION_LAT) &&
                    ((getCurrentTATxFlag() & TA_TX_APRS) != 0);

    if (send_gps) {
        dmr_gps_info_lc_make(gps_lc, settings.location_lat, settings.location_lon);
        dmr_emb_lc_make(gps_lc, gps_frags);
    }

    dmr_lc_make(lc, pc ? DMR_FLCO_UNIT : DMR_FLCO_GROUP, 0, trxTalkGroupOrPcId & 0xFFFFFF, trxDMRID);
    dmr_emb_lc_make(lc, frags);
    vocoderReset();
    afrx_fifo_prepare(MODEM_MOD_SCALE_ONE); // the modem's 4FSK level as calibrated: DMR's +-1944 Hz
    tx_underruns_seen = 0; // the FIFO's count from 0 (p25_phy_tx_reset())
    dmr_soft_tx_stats[0]++;
    dmr_soft_tx_stats[1] = dmr_soft_tx_stats[2] = 0;
    dmr_full_lc_make(lc, DMR_MASK_VOICE_LC_HEADER, full);
    dmr_burst_data(b, full, cc, DMR_DT_VOICE_LC_HEADER, DMR_SYNC_MS_DATA);
    for (int i = 0; i < TX_HEADERS; i++) {
        put_burst(b);
        p25_ev_log(33, (uint16_t)i); // the delay log (p25_radio.c): a voice LC header into the FIFO
    }
    bool keyed = false;
    if (rmo) { // on the repeater's time line: keyed now (the PA kept off until our first burst), the header first,
               // the voice as it is encoded
        radio_phy_tx_pa_later(dmr_soft_tx_gate && (dmr_soft_rmo_pa_us > 0));
        keyed = afrx_fifo_key_at(dmr_soft_tx_low_power != 0, reset_at);
        radio_phy_tx_pa_later(false);
        p25_ev_log(35, keyed);
        if (keyed) {
            dmr_soft_rmo_stats[3] = p25_phy_tx_reset_us - reset_at;
            if (dmr_soft_tx_gate) {
                radio_phy_tx_gate_at((dmr_soft_rmo_pa_us > 0) ? (p25_phy_tx_reset_us + (uint32_t)dmr_soft_rmo_pa_us) : 0);
            }
        }
    }
    int16_t *pcm = txm.pcm;
    int halves = 0, nf = 0, pos = 0, sf = 0, tx_voice_logged = 0;
    bool ending = false;
    for (;;) {
        if (rmo && !keyed) {
            break;
        }
        if (!afrx_ptt_held()) {
            ending = true;
        }
        if (ending && (nf == 0) && (pos == 0)) {
            break; // the superframe finished
        }
        if (ending) {
            memcpy(ambe[nf], silence, DMR_AMBE_BYTES);
        } else if (sound_mic_take(&pcm[80 * halves])) {
            if (++halves < 2) {
                continue;
            }
            halves = 0;
            uint32_t enc_t0 = systime_get_us();
            vocoderEncodeFrame(pcm, ambe[nf]);
            if (dmr_soft_tx_stats[2] < 4) {
                p25_ev_log(37, (uint16_t)((systime_get_us() - enc_t0) / 100)); // the delay log: a frame encoded (arg
            }                                                                   // its time, 0.1 ms)
            dmr_soft_tx_stats[2]++;
            if (!keyed && (nf + 1 >= TX_KEY_AFTER)) { // on the air now (the mic recording on meanwhile)
                keyed = afrx_fifo_key(dmr_soft_tx_low_power != 0);
                p25_ev_log(35, keyed); // the delay log: keyed
                if (!keyed) {
                    break;
                }
                if (dmr_soft_tx_gate) {
                    radio_phy_tx_gate(true); // the PA on for our timeslot only
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (++nf < 3) {
            continue;
        }
        nf = 0;
        if (pos == 0) {
            dmr_burst_voice_sync(b, (const uint8_t(*)[DMR_AMBE_BYTES])ambe, DMR_SYNC_MS_VOICE);
        } else {
            // Alternate the base LC and the GPS Info LC in the embedded signalling, as the engine alternates its
            // talker alias phases: every other superframe carries the four GPS fragments in bursts B - E.
            const uint8_t (*emb)[4] = (send_gps && ((sf % 2) == 0)) ? gps_frags : frags;

            dmr_burst_voice_emb(b, (const uint8_t(*)[DMR_AMBE_BYTES])ambe, cc, 0, lcss[pos - 1],
                                (pos < 5) ? emb[pos - 1] : null_frag);
        }
        put_burst(b);
        if (tx_voice_logged < 3) {
            p25_ev_log(34, (uint16_t)tx_voice_logged++); // a voice burst into the FIFO
        }
        pos = (pos + 1) % 6;
        if (pos == 0) {
            sf++; // a superframe finished (A + B - F)
        }
    }
    if (keyed) {
        dmr_full_lc_make(lc, DMR_MASK_TERMINATOR_LC, full);
        dmr_burst_data(b, full, cc, DMR_DT_TERMINATOR_LC, DMR_SYNC_MS_DATA);
        put_burst(b);
    }
    uint32_t underruns = afrx_fifo_finish(keyed);
    radio_phy_tx_gate(false);
    if (keyed) {
        dmr_soft_tx_stats[3] = underruns;
    }
}

/* ---- repeater mode ---- */

static bool rmo(void)
{
    return currentRadioDevice->trxDMRModeTx == DMR_MODE_RMO;
}

// the repeater followed: its bursts in a row, the last fresh, our colour code among them, the recording's clock known
static bool downlink_ok(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t us;
    return dl.any && (dl.run >= DL_RUN) && ((now - dl.last) <= pdMS_TO_TICKS(DL_FRESH_MS)) && dl.cc_seen &&
           ((now - dl.cc_ok) <= pdMS_TO_TICKS(DL_CC_MS)) && afrx_sample_us(dl.at, &us);
}

// the physical layer's reset for our next burst: its symbol 0 on the air with the repeater's burst of our timeslot,
// at least RMO_SETUP_US ahead; false: the time line not known
static bool rmo_plan(uint32_t *reset_at)
{
    uint32_t us;
    if (!afrx_sample_us(dl.at, &us)) {
        return false;
    }
    uint32_t now = systime_get_us();
    uint32_t t = us + ((dl.ts != trxGetDMRTimeSlot()) ? 30000u : 0u) - (uint32_t)dmr_soft_rmo_lead_us;
    while ((int32_t)(t - now) < RMO_SETUP_US) {
        t += 60000u;
    }
    extern volatile uint32_t afrx_clock[2];
    dmr_soft_rmo_plan_clock[0] = afrx_clock[0];
    dmr_soft_rmo_plan_clock[1] = afrx_clock[1];
    dmr_soft_rmo_stats[4] = t - now;
    dmr_soft_rmo_stats[5] = now - us;
    *reset_at = t;
    return true;
}

// The wake-up as the engine sends it (ETSI TS 102 361-2, CSBK BS_Dwn_Act: CSBKO 0x38 with its last block bit, FID 0,
// 16 bits reserved, the BS address all ones, ours), one burst, then the transmitter off at once
static void send_wake(void)
{
    uint8_t csbk[10] = {0xB8, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, (uint8_t)(trxDMRID >> 16), (uint8_t)(trxDMRID >> 8),
                        (uint8_t)trxDMRID};
    uint8_t info[12];
    dmr_crc_ccitt_make(csbk, DMR_MASK_CSBK, info);
    dmr_burst_data(txm.b, info, trxGetDMRColourCode(), DMR_DT_CSBK, DMR_SYNC_MS_DATA);
    p25_phy_tx_reset();
    modem_set_mod_scale(MODEM_MOD_SCALE_ONE);
    tx_underruns_seen = 0;
    put_burst(txm.b);
    radio_phy_tx_pa_later(dmr_soft_tx_gate && (dmr_soft_rmo_pa_us > 0)); // no carrier before the burst
    int begun = p25_phy_tx_begin(dmr_soft_tx_low_power != 0);
    radio_phy_tx_pa_later(false);
    if (begun == 0) {
        if (dmr_soft_tx_gate) {
            radio_phy_tx_gate_at((dmr_soft_rmo_pa_us > 0) ? (p25_phy_tx_reset_us + (uint32_t)dmr_soft_rmo_pa_us) : 0);
        }
        // the burst out once the second interrupt came (the empty block after it being read)
        TickType_t t0 = xTaskGetTickCount();
        while (((p25_phy_tx_isr_n == UINT32_MAX) || (p25_phy_tx_isr_n < 1)) &&
               ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(100))) {
            vTaskDelay(1);
        }
        radio_phy_tx_gate(false);
        p25_phy_tx_end();
    }
}

// The PTT held, transmit() now? Repeater mode: not while waiting for the repeater after a wake-up, nor once failed
static bool tx_ready(void)
{
    if (wake_reset) {
        wake_reset = false;
        wake.sent = 0;
    }
    if (!dmr_soft_tx || !rmo() || downlink_ok()) {
        return true;
    }
    if (dmr_soft_wake_failed) {
        return false;
    }
    return (wake.sent == 0) || ((xTaskGetTickCount() - wake.at) >= pdMS_TO_TICKS(WAKE_RETRY_MS));
}

// The PTT held: the recording and the playback are off (afrx.c). In software: direct mode; repeater mode on the
// repeater's time line, or its wake-up (then receiving again, tx_ready()); dmr_soft_tx 0: the engine transmits from
// its receive state from scratch, until the PTT is released and its transmission (a repeater's hang time) is over
// Repeater mode: true, the transmission planned on the repeater's time line (*reset_at); false: a wake-up sent (then
// receiving until tx_ready()), or the wake-ups given up (dmr_soft_wake_failed)
static bool rmo_access(uint32_t *reset_at)
{
    if (downlink_ok() && rmo_plan(reset_at)) {
        dmr_soft_rmo_stats[1]++;
        return true;
    }
    if (wake.sent > codeplugGetRepeaterWakeAttempts()) {
        dmr_soft_wake_failed = true; // the TX screen ends it (dmr_repeater_failed())
        dmr_soft_rmo_stats[2]++;
        return false;
    }
    send_wake();
    wake.sent++;
    wake.at = xTaskGetTickCount();
    dmr_soft_rmo_stats[0]++;
    return false;
}

static void transmit(void)
{
    if (dmr_soft_tx && !rmo()) {
        soft_transmit(false, 0);
        return;
    }
    if (dmr_soft_tx) {
        uint32_t reset_at;
        if (rmo_access(&reset_at)) {
            soft_transmit(true, reset_at);
        }
        return;
    }
    dmr_engine_resume();
    dmr_engine_transmit(true);
    while (afrx_ptt_held()) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    dmr_engine_transmit(false);
    TickType_t t0 = xTaskGetTickCount();
    while (dmr_engine_transmitting() && ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(TX_END_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ---- data (APRS over DMR) ----
   In software as the voice: each burst's 10 octets (a CSBK, a data header) with their CRC-CCITT and the data type's
   mask, or a data block's 12 as they are, through the BPTC (196,96) with the MS data sync, all into the FIFO (12 pairs
   of blocks fit), the transmitter keyed at once in direct mode, on the repeater's time line in repeater mode (woken
   first: data() asked again until it answers). dmr_soft_tx 0: the engine sends them (HRC7000SendDataBursts()), the
   recording off meanwhile (afrx.c), the engine resumed, then back to receiving in software. */
#define DATA_MS 5000 // the bursts sent at most (a repeater woken first)

static const afrx_ops_t ops;

static dmr_data_tx_t data_req;
static volatile bool data_pending, data_failed, data_cancel;

#define DATA_FIRST 12 // bursts into the FIFO before keying (14 pairs of blocks fit): the rest while it sends

// burst i of the request into the FIFO: a CSBK's or a header's 10 octets with their CRC and mask, a block's 12
static void put_data_burst(int i, uint8_t cc)
{
    uint8_t info[12];
    const dmr_data_burst_t *d = &data_req.burst[i];
    if (d->type == DMR_DT_CSBK) {
        dmr_crc_ccitt_make(d->payload, DMR_MASK_CSBK, info);
    } else if (d->type == DMR_DT_DATA_HEADER) {
        dmr_crc_ccitt_make(d->payload, DMR_MASK_DATA_HEADER, info);
    } else {
        memcpy(info, d->payload, 12);
    }
    dmr_burst_data(txm.b, info, cc, d->type, DMR_SYNC_MS_DATA);
    put_burst(txm.b);
}

// the bursts asked, keyed at once (direct mode) or at reset_at (repeater mode); false: not sent
static bool soft_data(bool rmo, uint32_t reset_at)
{
    uint8_t cc = trxGetDMRColourCode();
    p25_phy_tx_reset();
    modem_set_mod_scale(MODEM_MOD_SCALE_ONE);
    tx_underruns_seen = 0;
    int queued = 0;
    for (; (queued < data_req.count) && (queued < DATA_FIRST); queued++) {
        put_data_burst(queued, cc);
    }
    bool later = rmo && dmr_soft_tx_gate && (dmr_soft_rmo_pa_us > 0);
    radio_phy_tx_pa_later(later);
    int begun = rmo ? p25_phy_tx_begin_at(dmr_soft_tx_low_power != 0, reset_at)
                    : p25_phy_tx_begin(dmr_soft_tx_low_power != 0);
    radio_phy_tx_pa_later(false);
    if (begun != 0) {
        modem_set_mod_scale(MODEM_MOD_SCALE_ONE);
        return false;
    }
    if (dmr_soft_tx_gate) {
        radio_phy_tx_gate_at(later ? (p25_phy_tx_reset_us + (uint32_t)dmr_soft_rmo_pa_us) : 0);
    }
    for (; (queued < data_req.count) && !data_cancel; queued++) { // the rest as the FIFO empties (put_burst() waits)
        put_data_burst(queued, cc);
    }
    p25_phy_tx_pad();
    TickType_t t0 = xTaskGetTickCount();
    while ((p25_phy_tx_level() >= P25_PHY_BLOCK_BYTES * 4) && !data_cancel &&
           ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(DATA_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    bool sent = (p25_phy_tx_level() < P25_PHY_BLOCK_BYTES * 4);
    vTaskDelay(pdMS_TO_TICKS(60)); // the last blocks out of the modem
    radio_phy_tx_gate(false);
    p25_phy_tx_end();
    modem_set_mod_scale(MODEM_MOD_SCALE_ONE);
    dmr_soft_tx_stats[0]++;
    return sent;
}

static void data(void)
{
    if (dmr_soft_tx) {
        uint32_t reset_at = 0;
        bool r = rmo();
        if (data_cancel) {
            data_failed = true;
            data_pending = false;
            return;
        }
        if (r && !rmo_access(&reset_at)) {
            if (dmr_soft_wake_failed) { // no repeater
                data_failed = true;
                data_pending = false;
            } else {
                afrx_data(); // again once the repeater answers (tx_ready())
            }
            return;
        }
        data_failed = !soft_data(r, reset_at);
        data_pending = false;
        return;
    }
    dmr_engine_resume();
    uint8_t types[DMR_DATA_BURSTS_MAX], lengths[DMR_DATA_BURSTS_MAX], payloads[DMR_DATA_BURSTS_MAX][12];
    for (int i = 0; i < data_req.count; i++) {
        types[i] = data_req.burst[i].type;
        lengths[i] = data_req.burst[i].length;
        memcpy(payloads[i], data_req.burst[i].payload, 12);
    }
    bool ok = (data_req.count <= DMR_DATA_BURSTS_MAX) &&
              HRC7000SendDataBursts(types, lengths, (const uint8_t(*)[12])payloads, data_req.count);
    TickType_t t0 = xTaskGetTickCount();
    while (ok && !HRC7000DataBurstsDone() && !data_cancel && ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(DATA_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ok && !HRC7000DataBurstsDone()) {
        HRC7000AbortDataBursts(); // cancelled, or too long
        ok = false;
    }
    data_failed = !ok || HRC7000DataBurstsFailed();
    t0 = xTaskGetTickCount();
    while (dmr_engine_transmitting() && ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(TX_END_MS))) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    data_pending = false;
}

bool dmr_soft_send_data(const dmr_data_tx_t *tx)
{
    if (data_pending) {
        return false; // the last still going (the radio task sends one at a time)
    }
    data_failed = false; // also for the engine's own way (dmr.c, not received in software)
    if (!afrx_running(&ops) || (tx->count == 0) || (tx->count > DMR_DATA_TX_MAX)) {
        return false;
    }
    memcpy(&data_req, tx, sizeof(data_req));
    data_failed = false;
    data_cancel = false;
    dmr_soft_wake_failed = false; // repeater mode: woken from scratch
    wake_reset = true;
    data_pending = true;
    afrx_data();
    return true;
}

bool dmr_soft_data_busy(void)
{
    return data_pending;
}

bool dmr_soft_data_failed(void)
{
    return data_failed;
}

void dmr_soft_data_cancel(void)
{
    data_cancel = true;
}

static const afrx_ops_t ops = {
    .bw = AFREC_BW_FLAT, // no band-pass, no de-emphasis (the RRC is ours)
    .overlay = OVERLAY_AMBE,
    .play = &play_cfg,
    .rx_init = rx_init,
    .samples = samples,
    .poll = poll,
    .end_call = end_call,
    .transmit = transmit,
    .tx_ready = tx_ready,
    .precise_clock = true, // repeater mode's timing (and the burst log's times)
    .data = data,
    .stats = {
        .halves = &dmr_soft_stats.halves,
        .missed = &dmr_soft_stats.missed_halves,
        .busy_us_max = &dmr_soft_stats.busy_us_max,
    },
};

bool dmr_soft_wanted(void)
{
    return dmr_soft_enabled && (settingsUsbMode != USB_MODE_HOTSPOT);
}

void dmr_soft_start(void)
{
    afrx_start(&ops);
}

void dmr_soft_stop(void)
{
    if (data_pending) { // asked, not sent: failed
        data_pending = false;
        data_failed = true;
    }
    if (afrx_running(&ops)) {
        afrx_stop();
        dmr_call_over();
    }
}

bool dmr_soft_running(void)
{
    return afrx_running(&ops);
}

void dmr_soft_ptt(bool on)
{
    if (on) {
        dmr_soft_wake_failed = false;
        wake_reset = true;
    }
    afrx_ptt(on);
}

bool dmr_soft_transmitting(void)
{
    return afrx_running(&ops) && afrx_transmitting();
}

bool dmr_soft_has_sync(void)
{
    return afrx_running(&ops) && dm.locked;
}

bool dmr_soft_busy(void)
{
    return afrx_running(&ops) && (slots[0].active || slots[1].active);
}
