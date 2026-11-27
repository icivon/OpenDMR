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
 * OpenDMR, driver: the steps a transceiver chip driver implements (trx_driver_t). Only trx.c and the chip drivers
 * (trx_<chip>.c) include this.
 *
 * trx.c holds the flows (power on, the lock check, FM / DMR, the frequency, receive, transmit) and calls these steps
 * in order; a driver writes its chip's registers for each step. A flow one chip does differently than the others
 * (the DMR bursts) is a single step. Another chip: a trx_<chip>.c with its trx_driver_t, declared here and listed in
 * drivers[] (trx.c).
 *
 * A driver lives in RAM (not const): trx_af_out(), trx_read_signal() and trx_rx_tone_detected() run from RAM, with
 * the flash maybe busy.
 */

#ifndef DM32_TRX_DRIVER_H
#define DM32_TRX_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "trx.h"

typedef enum {
    TRX_BANDWIDTH_25,   // FM wide
    TRX_BANDWIDTH_12_5, // FM narrow, AM
    TRX_BANDWIDTH_6_25, // SSB
    TRX_BANDWIDTH_DMR,  // the digital modes
} trx_bandwidth_t;

typedef struct {
    const char *name;         // as printed on the chip
    uint16_t id;              // what read_id() gives for it
    bool hrc7000_rx_inverted; // the HR_C7000's RF_MODE receive inversion (bit 15; trx_rx_inverted())

    /* Power on */
    uint16_t (*read_id)(void); // the chip's ID (the first call resets the chip)
    uint16_t (*read_if)(void); // a diagnostic register for trx_start_ids() (the FD68xx: 0x3D)
    void (*init)(void);        // the power on settings
    void (*post_init)(void);   // after the modem init
    bool (*synthesizer_locked)(void);

    /* Modes */
    void (*fm_settings)(void);  // the analog modes
    void (*dmr_settings)(void); // the digital modes
    void (*set_volume)(uint8_t rx_volume);
    void (*set_demod)(trx_demod_t demod);
    void (*set_bandwidth)(trx_bandwidth_t bandwidth);

    /* Frequency, receive, transmit */
    void (*idle)(void); // neither receive nor transmit, the audio muted
    void (*set_band)(uint32_t frequency);
    void (*set_frequency)(uint32_t frequency);
    void (*rx_on)(void); // from idle
    // Receiving, to a frequency a little apart (the same band, trx_tune()): without going idle, the noise and signal
    // readings not started over
    void (*tune)(uint32_t frequency);
    void (*transmit_prepare)(void);
    void (*tx_on)(void); // FM, from idle
    void (*set_mic_gain)(uint8_t gain);

    /* DMR */
    void (*dmr_burst_tx)(uint32_t frequency); // each burst, from the DMR engine's interrupt
    void (*dmr_burst_rx)(uint32_t frequency);
    void (*dmr_af)(bool on);

    /* Tones and data while transmitting (trx.h) */
    void (*tx_tone)(uint16_t hz1, uint16_t hz2, uint8_t gain);
    void (*tx_tone_end)(void);
    void (*tx_tone_hz)(uint16_t hz);
    void (*tx_tail)(bool dcs, bool phase_shift);
    void (*tx_tail_end)(void);
    void (*tx_fsk)(const uint8_t *data, uint32_t length, bool wide);

    /* Receive */
    void (*af_out)(bool on);
    void (*read_signal)(uint8_t *signal, uint8_t *noise);
    void (*set_rx_tone)(uint16_t tone);
    void (*set_tx_tone)(uint16_t tone);
    bool (*rx_tone_detected)(uint16_t tone);
} trx_driver_t;

extern trx_driver_t trx_fd6818;  // trx_fd6818.c
extern trx_driver_t trx_fd6818b; // trx_fd6818b.c

#endif
