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
 * OpenDMR, driver: what the FD68xx transceiver chips (FD6818, FD6818B: the BK4819 family) have in common, for their
 * drivers (trx_fd6818.c, trx_fd6818b.c): the shared registers, and the functions both use as they are or build on.
 */

#ifndef DM32_TRX_FD68XX_H
#define DM32_TRX_FD68XX_H

#include <stdbool.h>
#include <stdint.h>
#include "trx_driver.h"
#include "trx_spi.h"

#define REG_ID        0x00
#define REG_FLAGS     0x0C
#define REG_CONTROL   0x30
#define REG_AF        0x47 // 0x6142: receive audio out, 0x6042: muted
#define REG_AGC_TOP   0x14 // the lowest gain of the AGC table
#define REG_AGC_THR   0x49 // the AGC thresholds

#define CONTROL_IDLE  0x0000
#define CONTROL_RX    0xBFF1
#define CONTROL_TX    0xC1FE

// REG_AF: 0x6042 is the FD6818's mute and 0x6142 its FM audio out (source 1). The FD6818B (DM-36) holds the FM audio
// out in 0x6042 (its audio source 0, the stock firmware's FM value at 0x0300E952) and mutes with 0x6040 (bit 1 clear,
// the stock's 0x0306A272): written 0x6142 it played a loud noise instead of the voice, and 0x6042 never muted.
#ifndef BOARD_AF_MUTED
#define BOARD_AF_MUTED 0x6042
#endif
#ifndef BOARD_AF_FM
#define BOARD_AF_FM    0x1 // the audio source the FM demodulator takes (bits 11:8)
#endif
#define AF_MUTED      BOARD_AF_MUTED
#define AF_OUT_MASK   0x0F00 // REG_AF bits 11:8: the audio out
// The whole REG_AF word a board wants for the received audio, where AF_MUTED | AF_ENABLE | source does not describe it.
// The DM-36 needs the FD6818's flat discriminator on the modem's AF input (0x6943, its digital receive value) and the
// audio played from the modem's own DAC (AUDIO_CONTROL bit 6): its AF output pin carries no demodulated audio at all.
#ifndef BOARD_AF_FM_WORD
#define BOARD_AF_FM_WORD (AF_MUTED | 0x0002 | (BOARD_AF_FM << 8))
#endif
#define DMR_50_TX     0x3B18 // 0x50 while a DMR burst is sent (stock 0x2b5a8)

typedef struct {
    uint8_t reg;
    uint16_t value;
} fd68xx_setting_t;

#define FD68XX_WRITE_TABLE(settings) fd68xx_write_table((settings), sizeof(settings) / sizeof((settings)[0]))

/* For the chip drivers */

void fd68xx_write_table(const fd68xx_setting_t *settings, uint32_t count);
void fd68xx_note_defaults(void); // after the post init table: 0x3D and 0x2F as the chip has them, for FM
void fd68xx_set_demod(trx_demod_t demod); // the demodulator registers all FD68xx chips share (then the chip's own)
void fd68xx_set_rx_tone(uint16_t tone);   // the receive tone (after the chip's own registers for it)

/* Steps (trx_driver.h) as they are */

uint16_t fd68xx_read_id(void); // REG_ID after a reset (the first call resets the chip, later ones give that ID)
uint16_t fd68xx_read_if(void); // 0x3D
bool fd68xx_synthesizer_locked(void);
void fd68xx_set_volume(uint8_t rx_volume); // the receive level as the stock firmware (0x300f148: 0x31E0 | level)
void fd68xx_set_bandwidth(trx_bandwidth_t bandwidth);
void fd68xx_idle(void);
void fd68xx_set_frequency(uint32_t frequency); // the frequency words 0x38 / 0x39 (10 Hz units)
void fd68xx_tune(uint32_t frequency);          // receiving, a little apart: not through idle (trx_driver.h)
void fd68xx_set_mic_gain(uint8_t gain);
void fd68xx_tx_tone(uint16_t hz1, uint16_t hz2, uint8_t gain);
void fd68xx_tx_tone_end(void);
void fd68xx_tx_tone_hz(uint16_t hz);
void fd68xx_tx_tail(bool dcs, bool phase_shift);
void fd68xx_tx_tail_end(void);
void fd68xx_tx_fsk(const uint8_t *data, uint32_t length, bool wide);
void fd68xx_af_out(bool on);
void fd68xx_read_signal(uint8_t *signal, uint8_t *noise);
void fd68xx_set_tx_tone(uint16_t tone);
bool fd68xx_rx_tone_detected(uint16_t tone);

#endif
