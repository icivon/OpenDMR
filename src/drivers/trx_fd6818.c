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
 * OpenDMR, driver: the FD6818 transceiver chip (ID 0x4819), the classic DM-32 RF section (trx_driver.h,
 * trx_fd68xx.h). The tables as OpenDM32 (hardware/AT1846S.c), the DMR bursts as the stock firmware's RF task
 * (DM32.01.L01.048).
 */

#include "trx_driver.h"
#include "trx_fd68xx.h"

#define REG_37_TX     0x1D0F // transmit, and each DMR burst sent
#define REG_37_DMR_RX 0x1F1F // each DMR burst received
#define AF_DMR        0x0140 // REG_AF in DMR receive: the raw audio out to the baseband
#define DMR_50_RX     0xBB20 // the raw baseband to the HR_C7000 (dmr_settings[])

// Power on (AT1846InitSettings)
static const fd68xx_setting_t init_settings[] = {
    {0x00, 0x8000}, {0x00, 0x0000}, {0x37, 0x1D0F}, {0x13, 0x03BE}, {0x12, 0x037B}, {0x11, 0x027B}, {0x10, 0x007A},
    {0x14, 0x0019}, {0x49, 0x2A38}, {0x7B, 0x8420}, {0x48, 0x33EF}, {0x1F, 0x5454}, {0x25, 0xC1BA}, {0x77, 0x88EF},
    {0x3E, 0xA037}, {0x13, 0x037E}, {0x7D, 0xE35E}, {0xB3, 0xFF00}, {0x33, 0xFE00}, {0xB4, 0x0000}, {0x34, 0x0009},
    {0xB3, 0xFE00}, {0x33, 0xFC00}, {0xB4, 0x0009}, {0x34, 0x0019}, {0x10, 0x007A}, {0x11, 0x027B}, {0x12, 0x037B},
    {0x13, 0x03BE}, {0x14, 0x0019}, {0x1F, 0x5454}, {0x30, 0xBFF1}, {0x37, 0x1D1F}, {0x3E, 0xA037}, {0x47, 0x0100},
    {0x48, 0xBBBF}, {0x49, 0x2A38}, {0x4E, 0x4708}, {0x4F, 0x302F}, {0x52, 0x028F}, {0x7B, 0x8420}, {0x09, 0x006F},
    {0x09, 0x106B}, {0x09, 0x2067}, {0x09, 0x3062}, {0x09, 0x4050}, {0x09, 0x5047}, {0x09, 0x603A}, {0x09, 0x702C},
    {0x09, 0x8041}, {0x09, 0x9037}, {0x09, 0xA025}, {0x09, 0xB017}, {0x09, 0xC0E4}, {0x09, 0xD0CB}, {0x09, 0xE0B5},
    {0x09, 0xF09F},
};

// After the modem init (AT1846PostinitSettings)
static const fd68xx_setting_t post_init_settings[] = {
    {0x47, 0x0000}, {0x13, 0x037E}, {0x47, 0x6042}, {0x7E, 0x302E}, {0x2B, 0xC000}, {0x43, 0x3028}, {0x78, 0x6968},
    {0x30, 0x0000}, {0x38, 0xBA00}, {0x39, 0x00DB}, {0x32, 0x0244}, {0x07, 0x0470}, {0x07, 0x21CE}, {0x51, 0x0000},
    {0x3F, 0x0000}, {0x37, 0x1F0F}, {0x30, 0x0000}, {0x30, 0xBFF1}, {0x47, 0x6042}, {0x48, 0xBBBF},
};

// FM, as the OpenDM32 releases (0x0304E8F0 in 20260711); 0x50 = 0x3B20 (the DMR value 0xBB20 gives a low deviation)
static const fd68xx_setting_t fm_settings[] = {
    {0x31, 0x0000}, {0x42, 0x6F5A}, {0x2A, 0x7400}, {0x30, 0x0000}, {0x13, 0x037E}, {0x47, 0x6042},
    {0x7E, 0x302E}, {0x2B, 0xC000}, {0x43, 0x3028}, {0x07, 0x0470}, {0x07, 0x21CE}, {0x51, 0x0000},
    {0x3F, 0x0000}, {0x37, 0x1F0F}, {0x50, 0x3B20}, {0x30, 0xBFF1}, {0x47, 0x6042}, {0x48, 0xBBBF},
};

// DMR, as the OpenDM32 releases (0x0304E8B1 in 20260711): 0x50 = 0xBB20, the FD6818 gives the HR_C7000 its raw
// baseband (without bit 15 it keeps sending demodulated audio and no DMR sync is ever found). 0x7E: the AGC automatic
// (bit 15 clear; OpenDM32 0xB03F fixes it at the highest gain): with the gain fixed the RSSI tops out at about
// -57 dBm (S8) however close the station, automatic it reads S9+40 like FM; the bad frames measured the same
// (32 % fixed, 31 % automatic, the same weak signal, 2026-11-10)
static const fd68xx_setting_t dmr_settings[] = {
    {0x31, 0x0000}, {0x42, 0x6F5A}, {0x2A, 0x7400}, {0x47, 0x0000}, {0x30, 0x0000}, {0x13, 0x037E}, {0x47, 0x0140},
    {0x48, 0xBBBF}, {0x7E, 0x303F}, {0x2B, 0x0700}, {0x43, 0x4048}, {0x78, 0x6968}, {0x30, 0x0000}, {0x37, 0x1F1F},
    {0x50, 0xBB20}, {0x30, 0x0000}, {0x30, 0xBFF1}, {0x47, 0x0140}, {0x3F, 0x0000}, {0xC7, 0x0140}, {0x47, 0x0140},
};

// The band registers 0x1A / 0x40 by frequency (FD6818_SelBand_Reg1A()), from the top
static const struct {
    uint32_t from;
    uint16_t reg_1a;
    uint16_t reg_40;
} bands[] = {
    {73999999, 0x1F80, 0x3AF0}, {36999999, 0x2F50, 0x39F0}, {26000000, 0x9F30, 0x385C}, {18499999, 0x3F48, 0x38F0},
    {12399999, 0xAF28, 0x375C}, {9299999, 0x4F44, 0x37F0},  {6199999, 0xBF24, 0x365C},  {4699999, 0x5F42, 0x36F0},
    {3099999, 0xCF22, 0x355C},  {2399999, 0x6F41, 0x35F0},  {1599999, 0xDF21, 0x345C},
};

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
    FD68XX_WRITE_TABLE(dmr_settings);
}

// AM as the UV-K5 firmwares (BK4819_InitAGC(true)), 0x42 and 0x2A with the "AM fix"
static void set_demod(trx_demod_t demod)
{
    bool am = (demod == TRX_DEMOD_AM);

    fd68xx_set_demod(demod);
    trx_spi_write(0x42, am ? 0x6F5C : 0x6F5A);
    trx_spi_write(0x2A, am ? 0x7434 : 0x7400);
    trx_spi_write(REG_AGC_TOP, am ? 0x0000 : 0x0019);
    trx_spi_write(REG_AGC_THR, am ? ((50 << 7) | 32) : 0x2A38);
}

static void set_band(uint32_t frequency)
{
    for (unsigned i = 0; i < (sizeof(bands) / sizeof(bands[0])); i++) {
        if (frequency >= bands[i].from) {
            trx_spi_write(0x1A, bands[i].reg_1a);
            trx_spi_write(0x40, bands[i].reg_40);
            return;
        }
    }
}

static void rx_on(void)
{
    trx_spi_write(REG_CONTROL, CONTROL_RX);
}

static void transmit_prepare(void)
{
    trx_spi_write(0x3F, 0x0000);
    trx_spi_write(0x40, 0x34EA);
    trx_spi_write(0x37, REG_37_TX);
}

static void tx_on(void)
{
    trx_spi_write(REG_CONTROL, CONTROL_TX);
}

// 0x2b674: the frequency, idle, the audio out off, transmit, the baseband path for transmitting
static void dmr_burst_tx(uint32_t frequency)
{
    fd68xx_set_frequency(frequency);
    trx_spi_write(REG_CONTROL, CONTROL_IDLE);
    trx_spi_write(REG_AF, trx_spi_read(REG_AF) & (uint16_t)~AF_OUT_MASK);
    trx_spi_write(0x37, REG_37_TX);
    trx_spi_write(0x52, 0x028F);
    trx_spi_write(REG_CONTROL, CONTROL_IDLE);
    trx_spi_write(REG_CONTROL, CONTROL_TX);
    trx_spi_write(0x50, DMR_50_TX);
}

// 0x2b6a0: idle, the frequency, receive, the audio out; 0x50 back to the raw baseband (stock elsewhere)
static void dmr_burst_rx(uint32_t frequency)
{
    trx_spi_write(REG_CONTROL, CONTROL_IDLE);
    fd68xx_set_frequency(frequency);
    trx_spi_write(0x32, 0x0244);
    trx_spi_write(0x37, REG_37_DMR_RX);
    trx_spi_write(REG_CONTROL, CONTROL_IDLE);
    trx_spi_write(REG_CONTROL, CONTROL_RX);
    trx_spi_write(REG_AF, AF_DMR);
    trx_spi_write(0x3F, 0x0000);
    trx_spi_write(0x50, DMR_50_RX);
}

static void dmr_af(bool on)
{
    trx_spi_write(REG_AF, on ? AF_DMR : AF_MUTED);
}

trx_driver_t trx_fd6818 = {
    .name = "FD6818",
    .id = 0x4819,
    .hrc7000_rx_inverted = true,

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
    .tune = fd68xx_tune,
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
    .set_rx_tone = fd68xx_set_rx_tone,
    .set_tx_tone = fd68xx_set_tx_tone,
    .rx_tone_detected = fd68xx_rx_tone_detected,
};
