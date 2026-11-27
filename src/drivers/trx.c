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
 * OpenDMR, driver: the transceiver (trx.h), whatever its chip. The flows are here, each a list of the chip driver's
 * steps (trx_driver.h); the registers are the drivers' business.
 */

#include <stddef.h>
#include "hal.h"
#include "hw.h"
#include "trx_spi.h"
#include "trx_driver.h"

#define LOCK_CHECK_FREQUENCY 14500000 // 145 MHz (10 Hz units): in band on every DM-32
#define LOCK_CHECK_MS        80

// The chip drivers, in the order they are tried; the first is also the one for an unknown chip
static trx_driver_t *const drivers[] = {&trx_fd6818, &trx_fd6818b};

static trx_driver_t *drv; // in RAM (trx_driver.h)
static bool drv_known;    // the chip's ID was one of drivers[]
static bool locked_at_start;
static uint16_t start_id, start_if;
static bool dmr = false;  // the DMR settings are in (the bandwidth)
static trx_demod_t demod = TRX_DEMOD_FM;

static trx_bandwidth_t bandwidth(bool wide)
{
    if (dmr) {
        return TRX_BANDWIDTH_DMR;
    }
    if (demod == TRX_DEMOD_SSB) {
        return TRX_BANDWIDTH_6_25;
    }
    return (wide && (demod == TRX_DEMOD_FM)) ? TRX_BANDWIDTH_25 : TRX_BANDWIDTH_12_5;
}

/* Power on */

// Locked three reads in a row, 1 ms apart (it can read locked once while the synthesizer settles), within ms
static bool wait_locked(uint32_t ms)
{
    int run = 0;

    for (uint32_t waited = 0;; waited++) {
        if (drv->synthesizer_locked()) {
            if (++run >= 3) {
                return true;
            }
        } else {
            run = 0;
            if (waited >= ms) {
                return false;
            }
        }
        delay_ms(1);
    }
}

// The driver's values, FM receive on 145 MHz: whether the synthesizer locks
static bool lock_check(void)
{
    drv->post_init();
    drv->fm_settings();
    trx_set_frequency(LOCK_CHECK_FREQUENCY, false);
    return wait_locked(LOCK_CHECK_MS);
}

// The driver by the chip's ID; its power on settings, the lock check (no lock: no transmitting at all), and its
// power on settings again
void trx_init(void)
{
    trx_spi_init();
    drv = drivers[0];
    drv_known = false;
    for (size_t i = 0; i < (sizeof(drivers) / sizeof(drivers[0])); i++) {
        start_id = drivers[i]->read_id();
        if (start_id == drivers[i]->id) {
            drv = drivers[i];
            drv_known = true;
            break;
        }
    }
    drv->init();
    start_if = drv->read_if();
    locked_at_start = lock_check();
    drv->init();
}

bool trx_locked(void)
{
    return locked_at_start;
}

const char *trx_chip_name(void)
{
    return drv_known ? drv->name : NULL;
}

void trx_start_ids(uint16_t *id, uint16_t *reg_3d)
{
    *id = start_id;
    *reg_3d = start_if;
}

bool trx_rx_inverted(void)
{
    return drv->hrc7000_rx_inverted;
}

void trx_post_init(void)
{
    drv->post_init();
}

/* Modes */

void trx_set_fm(uint8_t rx_volume)
{
    dmr = false;
    drv->fm_settings();
    drv->set_volume(rx_volume);
    trx_set_demod(TRX_DEMOD_FM);
}

void trx_set_demod(trx_demod_t new_demod)
{
    demod = new_demod;
    drv->set_demod(new_demod);
}

// The demodulator as FM first (as after the power on): SSB left the IF (0x3D) at 0, which the DMR settings don't
// write; so (with the AFC off, as it was in SSB then), a weak (-116 dBm) transmission's first DMR burst (a direct
// call's only voice LC header) was never received, after any visit to USB / LSB until a restart (2026-11-21)
void trx_set_dmr(void)
{
    trx_set_demod(TRX_DEMOD_FM);
    dmr = true;
    drv->dmr_settings();
}

/* Frequency, receive, transmit */

void trx_set_frequency(uint32_t frequency, bool wide)
{
    drv->idle();
    drv->set_band(frequency);
    drv->set_frequency(frequency);
    drv->set_bandwidth(bandwidth(wide));
    drv->rx_on();
}

void trx_tune(uint32_t frequency)
{
    drv->tune(frequency);
}

void trx_receive(bool wide)
{
    drv->idle();
    drv->set_bandwidth(bandwidth(wide));
    drv->rx_on();
}

void trx_idle(void)
{
    drv->idle();
}

void trx_set_mic_gain(uint8_t gain)
{
    drv->set_mic_gain(gain);
}

void trx_transmit_prepare(void)
{
    drv->transmit_prepare();
}

void trx_transmit(bool wide)
{
    drv->idle();
    drv->set_bandwidth(bandwidth(wide));
    drv->tx_on();
}

/* DMR */

void trx_dmr_burst_tx(uint32_t frequency)
{
    drv->dmr_burst_tx(frequency);
}

void trx_dmr_burst_rx(uint32_t frequency)
{
    drv->dmr_burst_rx(frequency);
}

void trx_dmr_af(bool on)
{
    drv->dmr_af(on);
}

/* Tones and data while transmitting */

void trx_tx_tone(uint16_t hz1, uint16_t hz2, uint8_t gain)
{
    drv->tx_tone(hz1, hz2, gain);
}

void trx_tx_tone_end(void)
{
    drv->tx_tone_end();
}

void trx_tx_tone_hz(uint16_t hz)
{
    drv->tx_tone_hz(hz);
}

void trx_tx_tail(bool dcs, bool phase_shift)
{
    drv->tx_tail(dcs, phase_shift);
}

void trx_tx_tail_end(void)
{
    drv->tx_tail_end();
}

void trx_tx_fsk(const uint8_t *data, uint32_t length, bool wide)
{
    drv->tx_fsk(data, length, wide);
}

/* Receive */

void RAM_CODE trx_af_out(bool on)
{
    drv->af_out(on);
}

void RAM_CODE trx_read_signal(uint8_t *signal, uint8_t *noise)
{
    drv->read_signal(signal, noise);
}

void trx_set_rx_tone(uint16_t tone)
{
    drv->set_rx_tone(tone);
}

void trx_set_tx_tone(uint16_t tone)
{
    drv->set_tx_tone(tone);
}

bool RAM_CODE trx_rx_tone_detected(uint16_t tone)
{
    return drv->rx_tone_detected(tone);
}
