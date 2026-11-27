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

#ifndef DM32_TRX_H
#define DM32_TRX_H

#include <stdbool.h>
#include <stdint.h>

// The RF transceiver, whatever its chip (FD6818 on the classic DM-32 RF section, FD6818B on the newer one): the chip's
// driver is picked at power on (trx.c; the drivers: trx_driver.h). Only the radio service uses it.
// Frequencies in 10 Hz units. Tones as the codeplug holds them (services/tones.h).

void trx_init(void);      // pins and the power on settings
void trx_post_init(void); // after the modem init
// The synthesizer locked at power on with the chip's values (0x0C bit 9, 145 MHz receive); false: no transmitting
// at all (the values are wrong for this chip)
bool trx_locked(void);
const char *trx_chip_name(void); // "FD6818", "FD6818B"; NULL: an unknown ID (then the FD6818's values)
void trx_start_ids(uint16_t *id, uint16_t *reg_3d); // 0x00 (the ID) and 0x3D as read at power on
bool trx_rx_inverted(void); // the HR_C7000's RF_MODE receive inversion (bit 15), by the chip
void trx_set_fm(uint8_t rx_volume); // the FM settings; rx_volume: the factory analog receive level (0x48)
void trx_set_dmr(void);             // the DMR settings (bandwidth: DMR whatever "wide" says, until trx_set_fm())
// The analog demodulator (trx_set_fm() sets FM): AM, or SSB (USB and LSB alike, the tuning picks the sideband).
// Receive only: AM and SSB use the 12.5 / 6.25 kHz filters whatever "wide" says.
typedef enum {
    TRX_DEMOD_FM,
    TRX_DEMOD_AM,
    TRX_DEMOD_SSB,
} trx_demod_t;
void trx_set_demod(trx_demod_t demod);


// radioSetFrequency(): AF muted, the band, the frequency, the bandwidth, receive on
void trx_set_frequency(uint32_t frequency, bool wide);
// Receiving, to a frequency a little apart (the same band and bandwidth: the satellites' Doppler correction, the VFO
// tuned): the receiver moved without going idle, the noise and signal readings go on (trx_set_frequency() starts them
// over: the noise at its top for 15 to 20 ms)
void trx_tune(uint32_t frequency);
void trx_receive(bool wide);  // receive on (radioSetRx(), radioSetIF())
void trx_idle(void);          // neither receive nor transmit (the audio muted)
// Transmit (radioSetTx()), the frequency set before: the transmit settings, then (after the baseband) transmit on
#define TRX_MIC_GAIN_MAX 31
// FM: the mic sensitivity, 0 .. 31 (settings.mic_gain_fm, 26 the stock default)
void trx_set_mic_gain(uint8_t gain);
void trx_transmit_prepare(void);
void trx_transmit(bool wide);
void trx_dmr_burst_tx(uint32_t frequency); // DMR, each burst (the stock RF task)
void trx_dmr_burst_rx(uint32_t frequency);
void trx_dmr_af(bool on); // the raw audio out of the digital modes (to the baseband), or muted
// While transmitting: Tone1 (and Tone2, for DTMF) on the air, the mic off; 0 Hz: that one silent
void trx_tx_tone(uint16_t hz1, uint16_t hz2, uint8_t gain);
void trx_tx_tone_end(void);                   // the mic back
void trx_tx_tone_hz(uint16_t hz);             // Tone1 to another frequency (the APRS AFSK), its phase goes on
void trx_tx_tail(bool dcs, bool phase_shift); // the tail: DCS 134.4 Hz; CTCSS shifted 240 degrees, or 55 Hz
void trx_tx_tail_end(void);                   // after the transmission
void trx_tx_fsk(const uint8_t *data, uint32_t length, bool wide); // FFSK 1200 / 1800 Hz (MDC1200), waits till sent
void trx_af_out(bool on);     // the receive audio out (the squelch)

// Signal and noise, raw (RSSI 0x67 / 2, noise 0x65 low byte); from RAM, in a critical section a register
void trx_read_signal(uint8_t *signal, uint8_t *noise);

void trx_set_rx_tone(uint16_t tone); // CODEPLUG_CSS_NONE: off
void trx_set_tx_tone(uint16_t tone);
bool trx_rx_tone_detected(uint16_t tone);

#endif
