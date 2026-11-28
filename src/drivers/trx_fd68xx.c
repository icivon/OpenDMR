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
 * OpenDMR, driver: what the FD68xx transceiver chips have in common (trx_fd68xx.h). After OpenDM32
 * hardware/AT1846S.c (DCS, CTCSS) and hardware/radioHardwareInterface.c, DM32 branches.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "codeplug.h"
#include "tones.h"
#include "trx_fd68xx.h"

#define REG_VOLUME    0x48
#define REG_BANDWIDTH 0x43
#define REG_TONE      0x07
#define REG_DCS       0x08
#define REG_CSS_MODE  0x51
#define REG_SIGNAL    0x67
#define REG_NOISE     0x65

#define CONTROL_TONE 0xC3FA // transmitting a tone: the mic ADC off (bit 2), the AF DAC on (bit 9), as the stock firmware
#define REG_TONES    0x70   // bit 15 Tone1 on, 14:8 its gain; bit 7 Tone2 on, 6:0 its gain
#define REG_TONE1_HZ 0x71   // Hz x 10.32444
#define REG_TONE2_HZ 0x72
#define TONE1_ON     0x8000
#define TONE2_ON     0x0080
#define TONE_SCALE   103244 // x 10000
#define BANDWIDTH_25 0x3028
#define BANDWIDTH_12 0x4008
#define BANDWIDTH_DMR 0x4048 // DMR (always 12.5 kHz): with the FM value the repeater downlink wasn't decoded
#define BANDWIDTH_SSB 0x3658 // 6.25 kHz mode, RF filter 3 kHz (BK4819_FILTER_BW_NARROWER of the UV-K5 firmware)

// The demodulator (BK4819: 0x47 bits 11:8 the AF output), as RADIO_SetModulation() of the UV-K5 firmware (egzumer):
// FM 1, AM 7, USB 5 ("baseband 2") with the IF at 0, the chip then a product detector. LSB is the same setting: with
// the IF at 0 both sidebands come through, the tuning picks one (fagci's firmware does the same). The AFC off in every
// mode (fd68xx_set_demod()).
#define AF_FM     BOARD_AF_FM
#define AF_AM     0x7
#define AF_SSB    0x5
#define REG_IF    0x3D
#define REG_AFC   0x73
#define AFC_OFF   0x0010

#define CTCSS_SCALE 20649 // register = Hz * 20.64888 (the DCS bit rate 134.4 Hz is 0x0AD7)
#define DCS_TONE    0x0AD7
#define DCS_TX_GAIN 0x3D
#define DCS_CODES   83

#define ID_RESET      0x8000 // REG_ID: the reset, then 0
#define FLAGS_LOCKED  0x0200 // REG_FLAGS bit 9: the synthesizer locked

static bool id_read;           // fd68xx_read_id() reset the chip and read its ID
static uint16_t chip_id;
#define AF_ENABLE 0x0002 // REG_AF bit 1: the audio output on (the mute value has it clear on the FD6818B)
static uint16_t af_on = BOARD_AF_FM_WORD; // DM-32: 0x6142, DM-36: 0x6943 (BOARD_AF_FM_WORD)
static uint16_t if_default;    // 0x3D and 0x2F as the chip starts (fd68xx_note_defaults()), for FM
static uint16_t reg_2f_default;

// 83 DCS codes (9 bits) with their Golay parity (11 bits), 20 bits packed in 3 bytes, by code
static const uint8_t dcs_packed[DCS_CODES * 3] = {
    0x63, 0x87, 0x09, 0xB7, 0x86, 0x0A, 0x5D, 0x06, 0x0B, 0x1F, 0x85, 0x0C, 0xF5, 0x05, 0x0D, 0xB6, 0x85, 0x11,
    0xFD, 0x80, 0x13, 0xCA, 0x87, 0x14, 0xF4, 0x06, 0x16, 0xD1, 0x85, 0x1A, 0x79, 0x86, 0x1C, 0x93, 0x06, 0x1D,
    0xE6, 0x82, 0x1D, 0x47, 0x07, 0x1E, 0x5E, 0x03, 0x26, 0x2B, 0x87, 0x26, 0xC1, 0x07, 0x27, 0x7B, 0x80, 0x2A,
    0xD3, 0x83, 0x2C, 0x39, 0x03, 0x2D, 0xED, 0x02, 0x2E, 0x7A, 0x83, 0x31, 0xEC, 0x01, 0x35, 0x4D, 0x84, 0x36,
    0xA7, 0x04, 0x37, 0xBC, 0x06, 0x39, 0x1D, 0x83, 0x3A, 0x5F, 0x00, 0x3D, 0x8B, 0x01, 0x3E, 0xE9, 0x86, 0x42,
    0x8E, 0x86, 0x49, 0xB0, 0x07, 0x4B, 0x5B, 0x84, 0x51, 0xFA, 0x01, 0x52, 0x8F, 0x85, 0x52, 0x27, 0x86, 0x54,
    0x77, 0x81, 0x58, 0xE8, 0x85, 0x59, 0x3C, 0x84, 0x5A, 0x94, 0x87, 0x5C, 0xCF, 0x00, 0x63, 0x8D, 0x83, 0x64,
    0xC6, 0x86, 0x66, 0x3E, 0x82, 0x6C, 0x97, 0x82, 0x71, 0xA9, 0x03, 0x73, 0xEB, 0x80, 0x74, 0x85, 0x06, 0x7A,
    0xF0, 0x82, 0x7A, 0x58, 0x81, 0x7C, 0x76, 0x87, 0x84, 0x9C, 0x07, 0x85, 0xE9, 0x83, 0x85, 0xB9, 0x84, 0x89,
    0xC5, 0x86, 0x8C, 0x2F, 0x06, 0x8D, 0xB8, 0x87, 0x92, 0x7E, 0x02, 0x9A, 0x0B, 0x86, 0x9A, 0xE1, 0x06, 0x9B,
    0xC6, 0x83, 0xA1, 0xF8, 0x02, 0xA3, 0x1B, 0x04, 0xA7, 0xE3, 0x00, 0xAD, 0x9E, 0x01, 0xB3, 0xC7, 0x80, 0xBA,
    0xD9, 0x05, 0xC3, 0x71, 0x06, 0xC5, 0xF5, 0x00, 0xCA, 0x1F, 0x80, 0xCB, 0x28, 0x87, 0xCC, 0xC2, 0x07, 0xCD,
    0xC3, 0x04, 0xD6, 0x47, 0x02, 0xD9, 0x93, 0x03, 0xDA, 0x2B, 0x82, 0xE1, 0xBD, 0x00, 0xE5, 0x98, 0x83, 0xE9,
    0xE4, 0x81, 0xEC, 0x0E, 0x01, 0xED, 0xDA, 0x00, 0xEE, 0x4D, 0x81, 0xF1, 0x0F, 0x02, 0xF6,
};

/* The steps (trx_driver.h) the FD68xx chips share */

void fd68xx_write_table(const fd68xx_setting_t *settings, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        trx_spi_write(settings[i].reg, settings[i].value);
    }
}

void fd68xx_set_frequency(uint32_t frequency)
{
    trx_spi_write(0x38, (uint16_t)frequency);
    trx_spi_write(0x39, (uint16_t)(frequency >> 16));
}

// The frequency, then receive written again without and with the VCO calibration bit (REG_CONTROL bit 15), not idle
// between: the synthesizer moves and locks in about 1.3 ms, the noise and signal readings go on (idle then receive
// started them over: the noise at its top, 120, back down after 15 to 20 ms; the FD6818, measured 2026-11-26). The
// frequency alone isn't taken. As BK4819_TuneTo() (not precise) of fagci's UV-K5 firmwares
#define CONTROL_VCO_CALIBRATION 0x8000

void fd68xx_tune(uint32_t frequency)
{
    fd68xx_set_frequency(frequency);
    trx_spi_write(REG_CONTROL, CONTROL_RX & (uint16_t)~CONTROL_VCO_CALIBRATION);
    trx_spi_write(REG_CONTROL, CONTROL_RX);
}

uint16_t fd68xx_read_id(void)
{
    if (!id_read) {
        trx_spi_write(REG_ID, ID_RESET);
        trx_spi_write(REG_ID, 0x0000);
        chip_id = trx_spi_read(REG_ID);
        id_read = true;
    }
    return chip_id;
}

uint16_t fd68xx_read_if(void)
{
    return trx_spi_read(REG_IF);
}

void fd68xx_note_defaults(void)
{
    if_default = trx_spi_read(REG_IF);
    reg_2f_default = trx_spi_read(0x2F);
}

bool fd68xx_synthesizer_locked(void)
{
    return (trx_spi_read(REG_FLAGS) & FLAGS_LOCKED) != 0;
}

void fd68xx_set_volume(uint8_t rx_volume)
{
    trx_spi_write(REG_VOLUME, 0x31E0 | (rx_volume & 0x0F));
}

// AM as the UV-K5 firmwares with the "AM fix" registers (armel/uv-k1-k5v3-firmware-custom PR 15, fagci's hawk5): the
// AM AGC, 0x31 bit 0, the noise gate times, the 300 Hz high pass off; FM back to the FM table's values. The chip's own
// registers (the AGC, 0x2A, 0x42) in its driver
void fd68xx_set_demod(trx_demod_t new_demod)
{
    bool am = (new_demod == TRX_DEMOD_AM);

    // A board that names its own word (BOARD_AF_FM_WORD) keeps it in every analog mode: the DM-36 takes the flat
    // discriminator through the modem, where the source field of the FD6818's own audio out means nothing.
    af_on = BOARD_AF_FM_WORD;
    (void)am;
    trx_spi_write(REG_AF, AF_MUTED);
    trx_spi_write(REG_IF, (new_demod == TRX_DEMOD_SSB) ? 0 : if_default);
    // The AFC off in every mode, as the FD6818 manual says with a TCXO ("AFC 设置"; the radio's is calibrated). On, with
    // the transmitter a little off frequency, its loop hunted at 15 - 30 Hz (0x0C bit 8 afc_rail stayed 0), heard in FM
    // as a low "du du du" (the discriminator: that tone and its harmonics 20 - 34 dB over the noise; none with it off).
    // Its speed can't be set, only its range (bits 13:11, 2.2 kHz here); a weak M17 signal 800 Hz off lost whole
    // transmissions with it on. Off, DMR, P25, NXDN and M17 at -116 dBm and 0 / +-300 Hz received as well or better
    // (2026-11-21).
    trx_spi_write(REG_AFC, (uint16_t)(trx_spi_read(REG_AFC) | AFC_OFF));
    trx_spi_write(0x31, (uint16_t)((trx_spi_read(0x31) & ~1u) | (am ? 1u : 0u)));
    trx_spi_write(0x2B, am ? 0x0600 : 0xC000);
    trx_spi_write(0x2F, am ? 0x9990 : reg_2f_default);
}

void fd68xx_set_bandwidth(trx_bandwidth_t bandwidth)
{
    static const uint16_t values[] = {
        [TRX_BANDWIDTH_25] = BANDWIDTH_25,
        [TRX_BANDWIDTH_12_5] = BANDWIDTH_12,
        [TRX_BANDWIDTH_6_25] = BANDWIDTH_SSB,
        [TRX_BANDWIDTH_DMR] = BANDWIDTH_DMR,
    };

    trx_spi_write(REG_BANDWIDTH, values[bandwidth]);
}

void fd68xx_idle(void)
{
    trx_spi_write(REG_CONTROL, CONTROL_IDLE);
}

// The FM mic sensitivity (0x7D bits 4:0, BK4819 REG_7D), as the stock firmware (0x300e618: 0xE340 | 22 .. 30, 26 by
// default; the power on table had 30)
void fd68xx_set_mic_gain(uint8_t gain)
{
    trx_spi_write(0x7D, (uint16_t)(0xE340 | ((gain > TRX_MIC_GAIN_MAX) ? TRX_MIC_GAIN_MAX : gain)));
}

void RAM_CODE fd68xx_read_signal(uint8_t *signal, uint8_t *noise)
{
    *signal = (uint8_t)((trx_spi_read(REG_SIGNAL) & 0x1FF) >> 1);
    *noise = (uint8_t)trx_spi_read(REG_NOISE);
}

// docs/fd6818-registers.md; BK4819_TransmitTone() of the UV-K5 firmware does the same
static uint16_t tone_word(uint16_t hz)
{
    return (uint16_t)((((uint32_t)hz * TONE_SCALE) + 5000) / 10000);
}

void fd68xx_tx_tone(uint16_t hz1, uint16_t hz2, uint8_t gain)
{
    uint16_t tones = 0;

    if (hz1 != 0) {
        trx_spi_write(REG_TONE1_HZ, tone_word(hz1));
        tones |= (uint16_t)(TONE1_ON | ((gain & 0x7F) << 8));
    }
    if (hz2 != 0) {
        trx_spi_write(REG_TONE2_HZ, tone_word(hz2));
        tones |= (uint16_t)(TONE2_ON | (gain & 0x7F));
    }
    trx_spi_write(REG_TONES, tones);
    trx_spi_write(REG_CONTROL, CONTROL_TONE);
}

void fd68xx_tx_tone_hz(uint16_t hz)
{
    trx_spi_write(REG_TONE1_HZ, tone_word(hz));
}

void fd68xx_tx_tone_end(void)
{
    trx_spi_write(REG_TONES, 0x0000);
    trx_spi_write(REG_CONTROL, CONTROL_TX);
}

// The stock firmware's tail (0x300E84C, as BK4819_GenTail() of the UV-K5 firmware), then 300 ms before the carrier
// goes: 0x52 bit 15 the shift or the 134.4 Hz of DCS, 14:13 its kind; or the CTCSS generator to 55 Hz
#define REG_TAIL        0x52
#define TAIL_NONE       0x028F // the init value
#define TAIL_SHIFT_240  0xE28F
#define TAIL_DCS_134    0x828F
#define TAIL_55HZ_TONE  0x046F

void fd68xx_tx_tail(bool dcs, bool phase_shift)
{
    if (dcs) {
        trx_spi_write(REG_TAIL, TAIL_DCS_134);
    } else if (phase_shift) {
        trx_spi_write(REG_TAIL, TAIL_SHIFT_240);
    } else {
        trx_spi_write(REG_TONE, TAIL_55HZ_TONE);
    }
}

void fd68xx_tx_tail_end(void)
{
    trx_spi_write(REG_TAIL, TAIL_NONE);
}

/* FFSK (MDC1200): the FSK modem as BK4819_send_MDC1200() of the UV-K5 firmware (OneOfEleven) */

#define REG_DEVIATION  0x40 // 11:0 the FM deviation
#define REG_FILTERS    0x2B
#define REG_FSK_MODE   0x58
#define REG_FSK        0x59
#define REG_FSK_SYNC01 0x5A
#define REG_FSK_SYNC23 0x5B
#define REG_FSK_CRC    0x5C
#define REG_FSK_LENGTH 0x5D
#define REG_FSK_FIFO   0x5F
#define REG_IRQ_ENABLE 0x3F
#define REG_IRQ        0x02
#define FSK_MODE_FFSK_1200_1800 0x3C03 // Tx FFSK 1200/1800, Rx FFSK 1200/1800, its Rx bandwidth, on
#define FSK_TX_FILTERS  0x0005 // the Tx 300 Hz high pass and the pre-emphasis off
#define FSK_CLEAR_FIFOS 0xC000
#define FSK_TX_ON       0x0800
#define FSK_PREAMBLE_4  (3 << 4) // 4 bytes of the chip's own preamble
#define FSK_SYNC_4      (1 << 3) // 4 sync bytes (0 here: MDC1200's sync is in the data)
#define FSK_CRC_OFF     0x5625
#define FSK_TONE2_GAIN  96
#define IRQ_FSK_TX_DONE 0x8000
#define IRQ_PENDING     0x0001 // REG_FLAGS bit 0
#define FSK_DEVIATION_NARROW 850
#define FSK_DEVIATION_WIDE   1050
#define FSK_BAUD        1200
#define FSK_OVERHEAD    8 // bytes the chip sends before the data: its preamble and sync

void fd68xx_tx_fsk(const uint8_t *data, uint32_t length, bool wide)
{
    uint16_t css = trx_spi_read(REG_CSS_MODE);
    uint16_t deviation = trx_spi_read(REG_DEVIATION);
    uint16_t filters = trx_spi_read(REG_FILTERS);
    uint16_t fsk = FSK_PREAMBLE_4 | FSK_SYNC_4;
    // The packet's time on the air; the chip says when it's done, this is twice that
    TickType_t limit = xTaskGetTickCount() +
                       pdMS_TO_TICKS(((2 * 8 * 1000) * (length + FSK_OVERHEAD)) / FSK_BAUD);

    trx_spi_write(REG_CONTROL, CONTROL_TONE); // the mic off
    trx_spi_write(REG_CSS_MODE, 0x0000);      // no CTCSS / DCS under it
    trx_spi_write(REG_DEVIATION, (uint16_t)((deviation & 0xF000) | (wide ? FSK_DEVIATION_WIDE : FSK_DEVIATION_NARROW)));
    trx_spi_write(REG_FILTERS, FSK_TX_FILTERS);
    trx_spi_write(REG_FSK_MODE, FSK_MODE_FFSK_1200_1800);
    trx_spi_write(REG_TONE2_HZ, tone_word(1200));
    trx_spi_write(REG_TONES, TONE2_ON | FSK_TONE2_GAIN);
    trx_spi_write(REG_FSK_LENGTH, (uint16_t)((length - 1) << 8));
    trx_spi_write(REG_FSK_SYNC01, 0x0000);
    trx_spi_write(REG_FSK_SYNC23, 0x0000);
    trx_spi_write(REG_FSK_CRC, FSK_CRC_OFF);
    trx_spi_write(REG_FSK, FSK_CLEAR_FIFOS | fsk);
    trx_spi_write(REG_FSK, fsk);
    for (uint32_t i = 0; (i + 1) < length; i += 2) { // 16 bits at a time, the first byte low
        trx_spi_write(REG_FSK_FIFO, (uint16_t)(data[i] | (data[i + 1] << 8)));
    }
    trx_spi_write(REG_IRQ_ENABLE, IRQ_FSK_TX_DONE);
    trx_spi_write(REG_FSK, FSK_TX_ON | fsk);

    while ((int32_t)(xTaskGetTickCount() - limit) < 0) {
        vTaskDelay(pdMS_TO_TICKS(4));
        if (trx_spi_read(REG_FLAGS) & IRQ_PENDING) {
            trx_spi_write(REG_IRQ, 0);
            if (trx_spi_read(REG_IRQ) & IRQ_FSK_TX_DONE) {
                break;
            }
        }
    }

    trx_spi_write(REG_FSK, fsk);
    trx_spi_write(REG_IRQ_ENABLE, 0x0000);
    trx_spi_write(REG_TONES, 0x0000);
    trx_spi_write(REG_FSK_MODE, 0x0000);
    trx_spi_write(REG_DEVIATION, deviation);
    trx_spi_write(REG_FILTERS, filters);
    trx_spi_write(REG_CSS_MODE, css);
    trx_spi_write(REG_CONTROL, CONTROL_TX);
}

void RAM_CODE fd68xx_af_out(bool on)
{
    trx_spi_write(REG_AF, on ? af_on : AF_MUTED);
}

/* CTCSS and DCS */

static uint16_t ctcss_register(uint16_t tone)
{
    return (uint16_t)((((uint32_t)tone * CTCSS_SCALE) + 5000) / 10000); // tone in 0.1 Hz: Hz x 20.649
}

// The octal digits held as hex (0x023) to the code (023 octal)
static uint16_t dcs_code(uint16_t tone)
{
    uint16_t digits = tone & TONES_DCS_CODE;

    return (uint16_t)((((digits >> 8) & 0x7) << 6) | (((digits >> 4) & 0x7) << 3) | (digits & 0x7));
}

// The 23 bit word to send: the Golay parity, the 0x800 marker and the code (dcsGetBitPatternFromCode())
static uint32_t dcs_word(uint16_t code)
{
    int low = 0;
    int high = DCS_CODES - 1;

    while (low <= high) {
        int middle = (low + high) / 2;
        const uint8_t *e = &dcs_packed[middle * 3];
        uint32_t entry = e[0] | (e[1] << 8) | ((uint32_t)e[2] << 16);
        uint16_t found = (entry >> 15) & 0x1FF;

        if (found < code) {
            low = middle + 1;
        } else if (found > code) {
            high = middle - 1;
        } else {
            return ((entry & 0x7FF) << 12) | 0x800 | code;
        }
    }
    return 0;
}

// The CTCSS generator at the DCS bit rate, the word in 0x08, then the mode (fd6818SetDCS())
static void set_dcs(uint16_t tone, bool transmit)
{
    uint32_t word = dcs_word(dcs_code(tone));
    bool inverted = (tone & TONES_DCS_INVERTED) != 0;

    trx_spi_write(REG_TONE, DCS_TONE | (transmit ? 0 : 0x4000));
    trx_spi_write(REG_DCS, (uint16_t)(word & 0x0FFF));
    trx_spi_write(REG_DCS, (uint16_t)(0x8000 | (((word >> 20) & 0x07) << 8) | ((word >> 12) & 0xFF)));
    if (transmit) {
        trx_spi_write(REG_CSS_MODE, (uint16_t)((inverted ? 0xA000 : 0x8000) | DCS_TX_GAIN));
    } else {
        trx_spi_write(REG_CSS_MODE, inverted ? 0x3000 : 0x1000);
    }
}

void fd68xx_set_rx_tone(uint16_t tone)
{
    if (tone == CODEPLUG_CSS_NONE) {
        trx_spi_write(REG_CSS_MODE, 0x0000);
    } else if (tone & TONES_DCS) {
        set_dcs(tone, false);
    } else {
        trx_spi_write(REG_CSS_MODE, 0x1000); // CTCSS, gain 0
        trx_spi_write(REG_TONE, 0x0000);
        trx_spi_write(REG_TONE, ctcss_register(tone));
        trx_spi_write(0x52, 0x028F); // the detection thresholds
    }
}

void fd68xx_set_tx_tone(uint16_t tone)
{
    if (tone == CODEPLUG_CSS_NONE) { // as the stock firmware (0x300E0D6)
        trx_spi_write(REG_CSS_MODE, 0x0000);
        trx_spi_write(REG_TONE, 0x0470);
        trx_spi_write(REG_TONE, 0x21CE);
    } else if (tone & TONES_DCS) {
        set_dcs(tone, true);
    } else {
        trx_spi_write(REG_CSS_MODE, 0x9045); // CTCSS, its gain
        trx_spi_write(REG_TONE, ctcss_register(tone));
    }
}

// 0x0C: bit 10 CTCSS, bit 14 DCS, bit 15 inverted DCS (the stock firmware, 0x300EE06 / 0x300EE10)
bool RAM_CODE fd68xx_rx_tone_detected(uint16_t tone)
{
    uint16_t flags;

    if (tone == CODEPLUG_CSS_NONE) {
        return false;
    }
    flags = trx_spi_read(REG_FLAGS);
    if (!(tone & TONES_DCS)) {
        return (flags & 0x0400) != 0;
    }
    return (flags & ((tone & TONES_DCS_INVERTED) ? 0x8000 : 0x4000)) != 0;
}
