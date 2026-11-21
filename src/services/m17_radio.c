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
 * OpenDMR, service: the M17 receiver (m17_radio.h, m17/README.md).
 *
 * As the NXDN receiver (nxdn_radio.c): the modem records its AF input (the FD6818's discriminator, 8 kHz, here without
 * the de-emphasis) in halves of 256 samples; the software receivers' task (afrx.c) gives each half as the modem
 * finishes it to m17_demod.c (4FSK in software: the syncs found by correlation, the timing, gain and DC from each),
 * whose dibits go into m17_core.c
 * (frames, FEC, LSF / LICH). Each stream frame's two Codec 2 3200 frames are decoded (c2dec.c) into the playback's
 * queue of 20 ms frames (voice_play.c: into the prompt FIFO, the beeps' path, which plays while the modem records).
 *
 * Transmit (transmit(), the PTT): the recording stopped, the mic's 20 ms frames through our Codec 2 3200 encoder
 * (c2enc.c), two a stream frame (m17_core.c: the preamble, the LSF from the radio's callsign to ALL, stream frames, the
 * last with its end bit, the EoT), the dibits through the P25 physical layer's transmit FIFO (4800 symbols/s, as M17),
 * the modem's 4FSK level scaled to M17's deviation (m17_tx_dev).
 *
 * The demodulator's and the receiver's state, the decoder and the encoder live in the M17 overlay RAM (ckcpu.ld),
 * claimed while the receiver runs (afrx.c).
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
#include "c2dec.h"
#include "c2enc.h"
#include "codeplug.h"
#include "dmr_compat.h"
#include "m17_core.h"
#include "m17_demod.h"
#include "m17_radio.h"
#include "ui.h"
#include "voice_play.h"
#include "afrx.h"

#define PCM_FRAMES    16   // 320 ms of decoded voice
#define PLAY_START    4    // frames queued before the playback starts (they come two at a time every 40 ms)
#define CALL_TIMEOUT  400  // ms without a stream frame: the call ended
#ifdef M17_HARD_VITERBI    // the hard decision build (comparisons): as before
#define VOICE_METRIC  24
#else
#define VOICE_METRIC  64   // a stream frame with more coded bits wrong: its voice not played (silence). With the soft
                           // decisions frames of 25 - 48 wrong bits came out right 71 - 75 % (-115 dBm, 2026-11-20); a
                           // noise frame has ~130: only those muted (a voice frame has no CRC: a wrong one can't be told)
#endif
// the test sweep (m17_tx_source 1): 300 Hz to 3 kHz in 2 s, again and again, a quarter of full scale
#define SWEEP_LO      161061273U  // 300 Hz: 2^32 300 / 8000 a sample
#define SWEEP_HI      1610612736U // 3 kHz
#define SWEEP_STEP    90597U      // (HI - LO) / 16000 a sample

#define OVERLAY_BSS __attribute__((section(".m17_bss"))) // in the M17 overlay, zeroed by its load

volatile m17_radio_stats_t m17_radio_stats;
volatile uint32_t m17_tx_dev = 1365;  // the 4FSK level (MODEM_MOD_SCALE_ONE: P25's +-1.8 kHz) for M17's +-2.4 kHz
volatile uint32_t m17_tx_low_power;   // not 0: 50 mW (tests with the radios side by side)
volatile uint32_t m17_tx_source;      // 0 the mic; 1 a test sweep (tests: m17/py/m17_tx_check.py)
volatile uint32_t m17_tx_stats[4];    // calls, stream frames, the longest Codec 2 encoding (us), underruns

/* in the overlay */
static m17_demod dm OVERLAY_BSS;
static m17_rx rx OVERLAY_BSS;
static union {
    struct {
        c2dec_t dec;
        int16_t pcm[PCM_FRAMES][C2DEC_SAMPLES];
    } rx;
    c2enc_t enc; // transmitting: the decoder and the playback queue unused (both set up again after)
} mem OVERLAY_BSS;
static uint8_t tx_dibits[M17_FRAME_DIBITS] OVERLAY_BSS;
// for tools over the cable (m17/py/m17_monitor.py): the dibits given to m17_core.c (5: the demodulator lost the
// signal), the last ones; the recording, the last M17_RAW_TAP samples, held RAW_HOLD samples after a call began (the
// call's start in it)
#define M17_TAP     4096
#define M17_RAW_TAP 12000
#define RAW_HOLD    4000
uint8_t m17_tap[M17_TAP] OVERLAY_BSS;
// the last stream frames decoded (their 16 bytes, then the hard and the soft metric), for checks against known content
// over the cable (m17/py/m17_rx_check.py)
#define M17_STREAM_LOG 64
uint8_t m17_stream_log[M17_STREAM_LOG][20] OVERLAY_BSS;
uint32_t m17_stream_log_n OVERLAY_BSS;
uint32_t m17_tap_n OVERLAY_BSS;
int16_t m17_raw_tap[M17_RAW_TAP] OVERLAY_BSS;
uint32_t m17_raw_tap_n OVERLAY_BSS;
uint32_t m17_raw_hold OVERLAY_BSS; // samples still to take (0: taking all the time; set once a call began)
volatile uint32_t m17_raw_rearm; // over the cable: not 0, the recording tap taking again
volatile uint32_t m17_raw_source; // over the cable: 1, the recording tap takes the first samples played in a call
volatile uint32_t m17_raw_mic_ring; // over the cable: not 0, the recording tap keeps the last samples (the mic's
                                    // transmitting; with m17_raw_source 1 the voice played receiving)


/* transmit: the PTT (m17_radio_transmit()) */
static volatile uint8_t tx_can;
static uint8_t tx_lsf[M17_LSF_BYTES];

static m17_call_t call;
static TickType_t last_frame;
static volatile TickType_t heard_at;
static volatile bool heard;


/* ---- the call ---- */

static void call_publish(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

static void call_begin(void)
{
    if (!call.active) {
        memset(&call, 0, sizeof(call));
        call.active = true;
        c2dec_init(&mem.rx.dec);
        m17_radio_stats.calls++;
        if (m17_raw_source == 1) { // the played audio from the call's start
            m17_raw_hold = 1;
            m17_raw_tap_n = 0;
        } else if (!m17_raw_hold) {
            m17_raw_hold = RAW_HOLD;
        }
        call_publish();
    }
}

static void call_end(void)
{
    call.active = false;
    voice_play_end(); // what is queued played out, then the speaker off
    call_publish();
}

// the LSF (from the frame or the LICH): the caller, and whether it is voice we can play
static void lsf(void)
{
    char src[10], dst[10];
    m17_address(rx.lsf + 6, src);
    m17_address(rx.lsf, dst);
    rx.lsf_valid = false;
    if (!call.active) {
        return;
    }
    bool muted = !m17_lsf_voice(rx.lsf);
    if (strcmp(src, call.src) || strcmp(dst, call.dst) || (muted != call.muted)) {
        memcpy(call.src, src, sizeof(src));
        memcpy(call.dst, dst, sizeof(dst));
        call.muted = muted;
        call_publish();
    }
}

/* ---- the voice ---- */

static void voice(const uint8_t stream[M17_STREAM_BYTES], bool good)
{
    uint32_t t0 = systime_get_us();
    for (int h = 0; h < 2; h++) {
        int16_t *out = voice_play_frame();
        if (!out) { // the playback fell behind: the newest frame dropped
            m17_radio_stats.pcm_drops++;
            continue;
        }
        if (good && !call.muted) {
            c2dec_decode(&mem.rx.dec, stream + C2DEC_BYTES * h, out);
        } else {
            memset(out, 0, sizeof(mem.rx.pcm[0]));
        }
        voice_play_put();
        m17_radio_stats.c2_frames++;
    }
    uint32_t us = systime_get_us() - t0;
    if (us > m17_radio_stats.decode_us_max) {
        m17_radio_stats.decode_us_max = us;
    }
}

static void log_stream(void)
{
    uint8_t *log = m17_stream_log[m17_stream_log_n % M17_STREAM_LOG];
    memcpy(log, rx.stream, M17_STREAM_BYTES);
    log[16] = (uint8_t)rx.metric;
    log[17] = (uint8_t)(rx.metric >> 8);
    log[18] = (uint8_t)rx.soft_metric;
    log[19] = (uint8_t)(rx.soft_metric >> 8);
    m17_stream_log_n++;
}

static void on_dibit(void *ctx, int d)
{
    (void)ctx;
    // the tap keeps dibits (5: lost), as the tools read it
    m17_tap[m17_tap_n % M17_TAP] = (uint8_t)((d == M17_DEMOD_LOST) ? 5
                                             : (d >= 2 * M17_SOFT_ONE) ? 1 : (d >= 0) ? 0 : (d >= -2 * M17_SOFT_ONE) ? 2 : 3);
    m17_tap_n++;
    if (d == M17_DEMOD_LOST) {
        m17_rx_reset(&rx);
        return;
    }
    switch (m17_rx_symbol(&rx, (int8_t)d)) {
    case M17_RX_LSF:
        m17_radio_stats.lsfs++;
        call_begin();
        last_frame = xTaskGetTickCount();
        lsf();
        break;
    case M17_RX_STREAM:
        log_stream();
        m17_radio_stats.streams++;
        m17_radio_stats.gain_sum += (uint32_t)dm.g;
        if (rx.metric > VOICE_METRIC) {
            m17_radio_stats.bad_frames++;
            if (!call.active) {
                break; // noise taken for a frame
            }
        }
        last_frame = xTaskGetTickCount();
        heard_at = last_frame;
        heard = true;
        call_begin();
        if (rx.lsf_valid) {
            lsf();
        }
        voice(rx.stream, rx.metric <= VOICE_METRIC);
        if (rx.eos) {
            call_end();
        }
        break;
    case M17_RX_EOT:
        m17_radio_stats.eots++;
        if (call.active) {
            call_end();
        }
        break;
    default:
        break;
    }
}

/* ---- the playback (voice_play.c) ---- */

// the voice played (the recording tap: m17_raw_source 1, the call's first samples)
static void play_tap(const int16_t *x, int n)
{
    for (int i = 0; (i < n) && (m17_raw_source == 1) && ((m17_raw_tap_n < M17_RAW_TAP) || m17_raw_mic_ring); i++) {
        m17_raw_tap[m17_raw_tap_n % M17_RAW_TAP] = x[i]; // m17_raw_mic_ring: the call's last samples kept
        m17_raw_tap_n++;
    }
}

static const voice_play_cfg_t play_cfg = {
    .frames = mem.rx.pcm,
    .count = PCM_FRAMES,
    .start = PLAY_START,
    .underruns = &m17_radio_stats.underruns,
    .tap = play_tap,
};

/* ---- transmit ---- */

// the receiver, the demodulator and the decoder from the start (the recording on; afrx.c starts the playback)
static void receive_init(void)
{
    m17_demod_init(&dm, on_dibit, NULL);
    m17_rx_init(&rx);
    c2dec_init(&mem.rx.dec);
    memset(&call, 0, sizeof(call));
}

/* The mic: its ADC has a large DC (~2400, after ~12000 at the start), which Codec 2 (made for AC coupled input) would
   code as a low tone in the pauses: a DC block (y = x - x1 + (1 - 1/64) y1, ~20 Hz) first. The raw samples also go
   into the recording tap (m17/py/m17_monitor.py --raw: the first 1.5 s of the transmission's mic). */
typedef struct {
    bool started; // x1 the first sample: no step from 0 to the DC (a click as loud as speech, 2026-11-20)
    int32_t x1;
    int32_t y1; // Q8
} mic_hp_t;

static void mic_filter(int16_t *x, mic_hp_t *hp)
{
    if (!hp->started) {
        hp->started = true;
        hp->x1 = x[0];
    }
    for (int i = 0; i < C2DEC_N; i++) {
        if ((m17_raw_tap_n < M17_RAW_TAP) || m17_raw_mic_ring) {
            m17_raw_tap[m17_raw_tap_n % M17_RAW_TAP] = x[i]; // the mic as it comes (before the DC block)
            m17_raw_tap_n++;
        }
        int32_t y = ((x[i] - hp->x1) << 8) + hp->y1 - (hp->y1 >> 6);
        hp->x1 = x[i];
        hp->y1 = y;
        y >>= 8;
        x[i] = (int16_t)((y > 32767) ? 32767 : (y < -32768) ? -32768 : y);
    }
}

/* A transmission while the PTT stays held (afrx.c: the playback and the recording off): the preamble and the LSF into
   the FIFO and the transmitter keyed (80 ms: the first stream frame comes 40 ms of mic and two encodings later); then a
   stream frame for every two mic frames (40 ms). The PTT released: the frame finished with silence and sent as the
   last, the EoT; once sent, back to receive. */
static void transmit(void)
{
    // the playback is off (afrx.c): the encoder takes its queue's RAM (mem)
    afrx_fifo_prepare(m17_tx_dev);
    c2enc_init(&mem.enc);
    m17_tx_stats[0]++;
    m17_tx_stats[1] = 0;

    char src[16];
    codeplug_get_radio_name(src);
    src[9] = 0;
    m17_lsf_make(tx_lsf, NULL, src, tx_can);
    m17_tx_preamble(tx_dibits);
    p25_phy_tx_put(tx_dibits, M17_FRAME_DIBITS);
    m17_tx_lsf(tx_lsf, tx_dibits);
    p25_phy_tx_put(tx_dibits, M17_FRAME_DIBITS);
    bool started = afrx_fifo_key(m17_tx_low_power != 0);
    int16_t pcm_tx[C2DEC_SAMPLES];
    uint8_t data[M17_STREAM_BYTES];
    uint32_t tone = 0, step = SWEEP_LO; // the test sweep's phase and its step a sample (2^32: 2 pi)
    mic_hp_t hp = {0};
    m17_raw_hold = 1; // the recording tap: the mic's first samples (mic_filter()), kept after
    m17_raw_tap_n = 0;
    int halves = 0, nc = 0;
    uint16_t fn = 0;
    bool ending = false;
    while (started) {
        if (!afrx_ptt_held()) {
            ending = true;
        }
        if (ending) { // silence to the end of the stream frame
            memset(&pcm_tx[C2DEC_N * halves], 0, sizeof(int16_t) * C2DEC_N * (unsigned)(2 - halves));
            halves = 0;
        } else if (sound_mic_take(&pcm_tx[C2DEC_N * halves])) {
            int16_t *x = &pcm_tx[C2DEC_N * halves];
            if (m17_tx_source == 1) { // the mic's timing, the sweep's samples
                for (int i = 0; i < C2DEC_N; i++) {
                    x[i] = (int16_t)(c2_sintab[tone >> 22] >> 2);
                    tone += step;
                    step = (step + SWEEP_STEP > SWEEP_HI) ? SWEEP_LO : step + SWEEP_STEP;
                }
            } else {
                mic_filter(x, &hp);
            }
            if (++halves < 2) {
                continue;
            }
            halves = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        uint32_t t0 = systime_get_us();
        c2enc_encode(&mem.enc, pcm_tx, &data[C2DEC_BYTES * nc]);
        uint32_t us = systime_get_us() - t0;
        if (us > m17_tx_stats[2]) {
            m17_tx_stats[2] = us;
        }
        if (++nc < 2) {
            continue;
        }
        nc = 0;
        m17_tx_stream(tx_lsf, fn, ending, data, tx_dibits);
        p25_phy_tx_put(tx_dibits, M17_FRAME_DIBITS);
        fn = (uint16_t)((fn + 1) & 0x7FFF);
        m17_tx_stats[1]++;
        if (ending) {
            break;
        }
    }
    if (started) {
        m17_tx_eot(tx_dibits);
        p25_phy_tx_put(tx_dibits, M17_FRAME_DIBITS);
    }
    uint32_t underruns = afrx_fifo_finish(started);
    if (started) {
        m17_tx_stats[3] = underruns;
    }
    m17_raw_hold = 1; // the mic's samples kept for the cable until m17_raw_rearm
}

void m17_radio_set_can(uint8_t can)
{
    tx_can = can & 0x0F;
}

void m17_radio_transmit(bool on)
{
    afrx_ptt(on);
}

bool m17_radio_transmitting(void)
{
    return afrx_transmitting();
}

/* ---- receiving (afrx.c runs it) ---- */

static void samples(int16_t *x, int n)
{
    if (m17_raw_rearm) {
        m17_raw_rearm = 0;
        m17_raw_hold = 0;
    }
    for (int k = 0; (k < n) && (m17_raw_hold != 1); k++) {
        m17_raw_tap[m17_raw_tap_n % M17_RAW_TAP] = x[k];
        m17_raw_tap_n++;
        if (m17_raw_hold > 1) {
            m17_raw_hold--;
        }
    }
    m17_demod_samples(&dm, x, n);
    m17_radio_stats.syncs = dm.syncs;
    m17_radio_stats.lost = dm.lost;
}

static void poll(void)
{
    if (call.active && ((xTaskGetTickCount() - last_frame) >= pdMS_TO_TICKS(CALL_TIMEOUT))) {
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
    .bw = AFREC_BW_FLAT, // no band-pass, no de-emphasis
    .overlay = OVERLAY_M17,
    .play = &play_cfg,
    .rx_init = receive_init,
    .samples = samples,
    .poll = poll,
    .end_call = end_call,
    .transmit = transmit,
    .stats = {
        .halves = &m17_radio_stats.halves,
        .busy_us_max = &m17_radio_stats.busy_us_max,
    },
};

void m17_radio_start(void)
{
    if (afrx_start(&ops)) {
        heard = false;
    }
}

void m17_radio_stop(void)
{
    if (!afrx_running(&ops)) {
        return;
    }
    afrx_stop();
    call.active = false;
    call_publish();
}

bool m17_radio_call(m17_call_t *out)
{
    *out = call;
    return call.active;
}

bool m17_radio_heard_within(uint32_t ms)
{
    return heard && ((xTaskGetTickCount() - heard_at) < pdMS_TO_TICKS(ms));
}
