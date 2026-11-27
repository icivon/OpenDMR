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
 * OpenDMR, driver: the FD6818B transceiver chip (ID 0x4829, 0x3D reads 0), the newer DM-32 RF section (stock
 * firmware DM32.NRFB...; #162, #174; trx_driver.h, trx_fd68xx.h). Its synthesizer never locks on the FD6818's
 * values (0x0C bit 9 stays clear: no receive, transmit far off frequency).
 *
 * The values after RadioFoundry (MIT; its docs/spec/hardware-dm32.md 8.10, drivers/rf/fd6818/fd6818_setup_b.c), who
 * checked them on two such radios (FM and DMR both ways). FM both ways checked on dev2vv's radio (#174). AM: not
 * worked out, the FM values.
 */

#include "codeplug.h"
#include "tones.h"
#include "trx_driver.h"
#include "trx_fd68xx.h"

#define REG_37        0x9D1F // before each switch to receive or transmit
#define REG_40        0x34F0 // 0x3480 + the power on calibration byte; the chip sets 0x1A itself
#define CONTROL_STEP  0x0200 // REG_CONTROL between idle and receive / transmit
#define REG_50_TX_ON  0xBB20 // 0x50 around the switch to FM transmit
#define REG_50_TX     0x3B20
#define AF_DMR        0x6943 // REG_AF in DMR receive: the raw audio out to the baseband

static const fd68xx_setting_t init_settings[] = {
    {0x00, 0x8000}, {0x00, 0x0000}, {0x37, 0x9D1F}, {0x13, 0x03DF}, {0x12, 0x03DB}, {0x11, 0x033A}, {0x10, 0x0318},
    {0x14, 0x0210}, {0x19, 0x1041}, {0x1C, 0x0422}, {0x1D, 0x2AAB}, {0x1E, 0x4C58}, {0x1F, 0x165A}, {0x25, 0x6DBA},
    {0x28, 0x0A40}, {0x29, 0xAA00}, {0x2A, 0x6600}, {0x2C, 0x0022}, {0x2F, 0x9890}, {0x3A, 0x9A7C}, {0x3E, 0x94C6},
    {0x3F, 0x07FE}, {0x40, 0x34F0}, {0x46, 0x6050}, {0x48, 0xB386}, {0x49, 0x2A32}, {0x4A, 0x5430}, {0x4D, 0xA015},
    {0x4F, 0x2A28}, {0x53, 0x2028}, {0x73, 0x6681}, {0x77, 0x88EF}, {0x7B, 0x73DC}, {0x7E, 0x303E}, {0x4C, 0xE520},
    {0x4E, 0x1F10}, {0x36, 0x003F}, {0x09, 0x006F}, {0x09, 0x106B}, {0x09, 0x2067}, {0x09, 0x3062}, {0x09, 0x4050},
    {0x09, 0x5047}, {0x09, 0x603A}, {0x09, 0x702C}, {0x09, 0x8041}, {0x09, 0x9037}, {0x09, 0xA025}, {0x09, 0xB017},
    {0x09, 0xC0E4}, {0x09, 0xD0CB}, {0x09, 0xE0B5}, {0x09, 0xF09F}, {0x21, 0x06D8}, {0x24, 0x8C7E},
};

static const fd68xx_setting_t post_init_settings[] = {
    {0x30, 0x0000},
};

// The 0x2B mode word, 0x48 its power on value again (DMR changes it); 0x50 as FM transmit leaves it (DMR: 0x3B18)
static const fd68xx_setting_t fm_settings[] = {
    {0x30, 0x0000}, {0x32, 0x0244}, {0x47, 0x6042}, {0x48, 0xB386}, {0x7E, 0x302E}, {0x2B, 0xC000}, {0x50, 0x3B20},
};

// These, then 0x3F = 0 (the power on 0x07FE never in DMR)
static const fd68xx_setting_t dmr_settings[] = {
    {0x47, 0x6943}, {0x48, 0xB2FD}, {0x7E, 0xB03F}, {0x2B, 0x0700}, {0x28, 0x0A40}, {0x3F, 0x0000},
};

static uint32_t burst_frequency; // the last DMR burst's frequency words (0: none since set_dmr())

/* The steps of its own (trx_driver.h) */

static void init(void)
{
    FD68XX_WRITE_TABLE(init_settings);
}

static void post_init(void)
{
    FD68XX_WRITE_TABLE(post_init_settings);
    fd68xx_note_defaults();
}

static void fm_settings_write(void)
{
    FD68XX_WRITE_TABLE(fm_settings);
}

static void dmr_settings_write(void)
{
    burst_frequency = 0;
    FD68XX_WRITE_TABLE(dmr_settings);
}

// FM and AM alike (AM not worked out on this chip)
static void set_demod(trx_demod_t demod)
{
    fd68xx_set_demod(demod);
    trx_spi_write(0x2A, 0x6600);
    trx_spi_write(REG_AGC_TOP, 0x0210);
    trx_spi_write(REG_AGC_THR, 0x2A32);
}

static void set_band(uint32_t frequency)
{
    (void)frequency;
    trx_spi_write(0x40, REG_40);
}

// From idle: 0x37, (transmit) 0x52 and the 0x0200 step first
static void control_on(uint16_t control)
{
    trx_spi_write(0x37, REG_37);
    if (control != CONTROL_RX) {
        trx_spi_write(0x52, 0x028F);
    }
    trx_spi_write(REG_CONTROL, CONTROL_STEP);
    trx_spi_write(REG_CONTROL, control);
}

static void rx_on(void)
{
    control_on(CONTROL_RX);
}

static void transmit_prepare(void)
{
    trx_spi_write(0x3F, 0x0000);
    trx_spi_write(0x40, 0x34EA);
    trx_spi_write(0x37, REG_37);
}

static void tx_on(void)
{
    trx_spi_write(0x50, REG_50_TX_ON);
    control_on(CONTROL_TX);
    trx_spi_write(0x50, REG_50_TX);
}

// Straight between receive and transmit, through idle only for another frequency (through idle each time, the first
// burst received after each one sent comes back about 400 Hz off)
static void burst_frequency_set(uint32_t frequency)
{
    if (frequency != burst_frequency) {
        trx_spi_write(REG_CONTROL, CONTROL_IDLE);
        fd68xx_set_frequency(frequency);
        burst_frequency = frequency;
    }
}

static void dmr_burst_tx(uint32_t frequency)
{
    burst_frequency_set(frequency);
    trx_spi_write(REG_AF, trx_spi_read(REG_AF) & (uint16_t)~AF_OUT_MASK);
    trx_spi_write(0x52, 0x028F);
    trx_spi_write(REG_CONTROL, CONTROL_TX);
    trx_spi_write(0x50, DMR_50_TX);
}

// 0x50 stays as the burst sent left it (0x3B18)
static void dmr_burst_rx(uint32_t frequency)
{
    burst_frequency_set(frequency);
    trx_spi_write(REG_CONTROL, CONTROL_RX);
    trx_spi_write(REG_AF, AF_DMR);
    trx_spi_write(0x3F, 0x0000);
}

static void dmr_af(bool on)
{
    trx_spi_write(REG_AF, on ? AF_DMR : AF_MUTED);
}

// Two words by the receive tone: CTCSS, else (DCS or none) the power on 0x2F
static void set_rx_tone(uint16_t tone)
{
    bool ctcss = (tone != CODEPLUG_CSS_NONE) && !(tone & TONES_DCS);

    trx_spi_write(0x2E, ctcss ? 0x8608 : 0x9608);
    trx_spi_write(0x2F, ctcss ? 0x1890 : 0x9890);
    fd68xx_set_rx_tone(tone);
}

trx_driver_t trx_fd6818b = {
    .name = "FD6818B",
    .id = 0x4829,
    .hrc7000_rx_inverted = false,

    .read_id = fd68xx_read_id,
    .read_if = fd68xx_read_if,
    .init = init,
    .post_init = post_init,
    .synthesizer_locked = fd68xx_synthesizer_locked,

    .fm_settings = fm_settings_write,
    .dmr_settings = dmr_settings_write,
    .set_volume = fd68xx_set_volume,
    .set_demod = set_demod,
    .set_bandwidth = fd68xx_set_bandwidth,

    .idle = fd68xx_idle,
    .set_band = set_band,
    .set_frequency = fd68xx_set_frequency,
    .rx_on = rx_on,
    .tune = fd68xx_tune, // CONTROL_STEP only from idle (control_on()); tune doesn't go through idle
    .transmit_prepare = transmit_prepare,
    .tx_on = tx_on,
    .set_mic_gain = fd68xx_set_mic_gain,

    .dmr_burst_tx = dmr_burst_tx,
    .dmr_burst_rx = dmr_burst_rx,
    .dmr_af = dmr_af,

    .tx_tone = fd68xx_tx_tone,
    .tx_tone_end = fd68xx_tx_tone_end,
    .tx_tone_hz = fd68xx_tx_tone_hz,
    .tx_tail = fd68xx_tx_tail,
    .tx_tail_end = fd68xx_tx_tail_end,
    .tx_fsk = fd68xx_tx_fsk,

    .af_out = fd68xx_af_out,
    .read_signal = fd68xx_read_signal,
    .set_rx_tone = set_rx_tone,
    .set_tx_tone = fd68xx_set_tx_tone,
    .rx_tone_detected = fd68xx_rx_tone_detected,
};
