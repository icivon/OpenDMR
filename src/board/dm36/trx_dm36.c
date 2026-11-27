/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * DM36 adaptation by BG2LNY
 * Redistribution and use: see LICENSE (BSD-style, non-commercial without written permission).
 *
 * OpenDMR, driver: the DM-36's FD6818B transceiver (BK4829, ID 0x4829) on the upstream transceiver driver
 * layer (drivers/trx_driver.h, drivers/trx_fd68xx.h): the register values from the DM-36 stock firmware
 * (DM36.NRFB.01.026, the writes listed in docs/dm36-stock-fd6818-writes.md), the flows as the FD6818B's
 * (drivers/trx_fd6818b.c).
 *
 * The chip is an FD6818B (BK4829), not the FD6818 (BK4819) of the DM-32: the FD6818's band registers (0x1A)
 * and its control flow don't apply to it. The chip picks its band itself (0x40 = 0x34F0, the power on
 * calibration byte) and the switch between receive and transmit goes through the 0x0200 step.
 *
 * The DM-36 build compiles this file alone (src/Makefile): the DM-32 keeps the shared drivers.
 */

#include "trx_driver.h"
#include "trx_fd68xx.h"
#include "trx_spi.h"
#include "codeplug.h"
#include "tones.h"
#include "bluetooth.h"

#define REG_37        0x9D1F // before each switch to receive or transmit
#define REG_40        0x34F0 // 0x3480 + the power on calibration byte; the chip sets 0x1A itself
#define CONTROL_STEP  0x0200 // REG_CONTROL between idle and receive / transmit
#define REG_50_TX_ON  0xBB20 // 0x50 around the switch to transmit
#define REG_50_TX     0x3B20
#define AF_DMR        0x6943 // REG_AF in DMR receive: the raw audio out to the baseband
#define BANDWIDTH_DMR 0x0048 // the DM-36 stock firmware (0x0300f26e), not the DM-32's 0x4048

// The FD6818B's own words, where the shared ones (trx_fd68xx.c) are the FD6818's:
// the bandwidth (0x43): 25 kHz FM 0x3028, 12.5 kHz FM 0x3048, 6.25 kHz (SSB) 0x205C. The shared
// fd68xx_set_bandwidth() writes the FD6818's 0x4008 for 12.5 kHz and 0x3658 for SSB: a different chip's words.
// The DM-36 stock firmware's four words are 0x3048, 0x3028, 0x205C and 0x2028 (the FM 12.5 kHz, the FM 25 kHz and
// two the stock never selects), the DM-1801 Pro's the same (hardware-dm1801pro.md 5.2).
#define BANDWIDTH_25   0x3028
#define BANDWIDTH_12_5 0x3048
#define BANDWIDTH_6_25 0x205C

// The mic gain (0x7D): the FD6818B's encoding, not the FD6818's. The shared fd68xx_set_mic_gain() writes
// 0xE340 | gain: the FD6818's receive digital gain 3 (bits 12:8) and its mic step table 22/24/26/28/30. The FD6818B:
// the receive digital gain 9 and the mic step table 31/34/38/41/44 (RadioFoundry's set-up B, the DM-32's newer RF
// section: docs/spec/hardware-dm32.md 8.10; their boards/dm32/radio_dm32.c writes 0xE92C on it).
#define MIC_GAIN_BASE    0xE900 // the receive digital gain 9
#define MIC_GAIN_DEFAULT 0xE90D // the stock DM-36's table entry 2 (see set_mic_gain())

// Power on: the DM-36 stock firmware's FD6818B init (0x0300ed96), value by value
static const fd68xx_setting_t init_settings[] = {
    {0x00, 0x8000}, {0x00, 0x0000}, {0x37, 0x9D1F},
    {0x13, 0x03DF}, {0x12, 0x03DB}, {0x11, 0x033A}, {0x10, 0x0318}, {0x14, 0x0210},
    {0x19, 0x1041}, {0x1C, 0x0422}, {0x1D, 0x2AAB}, {0x1E, 0x4C58}, {0x1F, 0x165A},
    {0x25, 0x6DBA}, {0x28, 0x0A40}, {0x29, 0xAA00}, {0x2A, 0x6600}, {0x2C, 0x0022},
    {0x2F, 0x9890}, {0x3A, 0x9A7C}, {0x3E, 0x94C6}, {0x3F, 0x07FE}, {0x40, 0x34F0},
    {0x46, 0x6050}, {0x48, 0xB386}, {0x49, 0x2A32}, {0x4A, 0x5430}, {0x4C, 0xE520},
    {0x4D, 0xA015}, {0x4E, 0x1F10}, {0x4F, 0x2A28}, {0x53, 0x2028}, {0x73, 0x6681},
    {0x77, 0x88EF}, {0x7B, 0x73DC}, {0x7E, 0x303E}, {0x36, 0x003F},
    {0x09, 0x006F}, {0x09, 0x106B}, {0x09, 0x2067}, {0x09, 0x3062}, {0x09, 0x4050}, {0x09, 0x5047},
    {0x09, 0x603A}, {0x09, 0x702C}, {0x09, 0x8041}, {0x09, 0x9037}, {0x09, 0xA025}, {0x09, 0xB017},
    {0x09, 0xC0E4}, {0x09, 0xD0CB}, {0x09, 0xE0B5}, {0x09, 0xF09F},
    // The mic gain: the FD6818B's receive digital gain 9 (the FD6818's is 3). The stock firmware doesn't write 0x7D
    // at all (it writes it with another function); the value is the one the FD6818B's set-up B uses (RadioFoundry).
    {0x7D, MIC_GAIN_DEFAULT},
};

// After the modem init: the FM setup of the stock firmware (0x37 = 0x9D1F, 0x48 = 0xB386)
static const fd68xx_setting_t post_init_settings[] = {
    {0x47, 0x0000}, {0x13, 0x03DF}, {0x47, 0x6042}, {0x7E, 0x302E}, {0x2B, 0xC000}, {0x43, 0x3028}, {0x78, 0x6968},
    {0x30, 0x0000}, {0x38, 0xBA00}, {0x39, 0x00DB}, {0x32, 0x0244}, {0x07, 0x0470}, {0x07, 0x21CE}, {0x51, 0x0000},
    {0x3F, 0x0000}, {0x37, 0x9D1F}, {0x30, 0x0000}, {0x30, 0xBFF1}, {0x47, 0x6042}, {0x48, 0xB386},
};

// FM: the stock firmware's analog settings (0x0300e93a...)
static const fd68xx_setting_t fm_settings[] = {
    {0x31, 0x0000}, {0x42, 0x6F5A}, {0x2A, 0x6600}, {0x30, 0x0000}, {0x13, 0x03DF}, {0x47, 0x6042}, {0x7E, 0x302E},
    {0x2B, 0xC000}, {0x43, 0x3028}, {0x07, 0x0470}, {0x07, 0x21CE}, {0x51, 0x0000}, {0x3F, 0x0000}, {0x37, 0x9D1F},
    {0x50, 0x3B20}, {0x30, 0xBFF1}, {0x47, 0x6042}, {0x48, 0xB386},
};

// DMR: the stock firmware's DMR table (0x0300f26e): 0x43 = 0x0048 (the bandwidth), 0x47 = 0x6740 / 0x6943,
// 0x7E = 0xB03F. The final values of 0x13 / 0x4A / 0x28 are the stock writes at 0x0300f28a..0x0300f2de:
// the earlier 0x13 = 0x03FF, 0x4A = 0x5450, 0x47 = 0x6740 are the first pass and are superseded.
// 0x50 is not written here (the stock's DMR table doesn't either): on the FD6818B its bit 15 is the analog
// transmit (the stock writes 0xBB20 around the transmit and 0x3B20 elsewhere, as the FD6818B's tx_on() does),
// not the FD6818's raw baseband. The raw audio of the digital modes comes with 0x47 = 0x6943.
// 0x48 is the FD6818B's set-up B word (0xB2FD), not the stock's 0xB2F5: the difference is the DAC gain field
// (5 against 13, about 2 dB a step). The digital modes are demodulated in software here (the discriminator from the
// chip's audio output: dmr_soft.c, nxdn_radio.c, m17_radio.c, p25_phy.c), so the level into the baseband matters:
// the DM-32's DMR word (0xBBBF) has the DAC gain 15, the stock's 5 is about 20 dB under it. The FD6818B's set-up
// B was checked on radios both ways (RadioFoundry: 0x6943 / 0xB2FD).
static const fd68xx_setting_t dmr_settings[] = {
    {0x31, 0x0000}, {0x42, 0x6F5A}, {0x2A, 0x6600}, {0x47, 0x0000}, {0x30, 0x0000}, {0x13, 0x03FF}, {0x47, 0x6740},
    {0x48, 0xB2FD}, {0x7E, 0xB03F}, {0x2B, 0x00E0}, {0x43, 0x0048}, {0x78, 0x6968}, {0x30, 0x0000}, {0x37, 0x9D1F},
    {0x30, 0x0000}, {0x30, 0xBFF1}, {0x13, 0x03DF}, {0x4A, 0x5430}, {0x28, 0x0A40}, {0x47, 0x6943},
    {0x3F, 0x0000}, {0xC7, 0x6943}, {0x47, 0x6943},
};

static uint32_t burst_frequency; // the last DMR burst's frequency words (0: none since set_dmr())

/* The steps of its own (trx_driver.h) */

static void init(void)
{
    FD68XX_WRITE_TABLE(init_settings);

    // The LytLink Bluetooth module (DM-36 only): its UART2 and its task. radio_init() runs before the
    // scheduler, so the task starts with it. The power on settings are written twice (upstream's trx_init()
    // does that for the lock check), so this is called twice as well: bluetooth_start() is idempotent.
    bluetooth_start();
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

// The AGC (0x14 = 0x0210, 0x49 = 0x2A32) and 0x2A (0x6600) as the DM-36 stock firmware, the "AM fix" register
// 0x42 as the UV-K5 firmwares
static void set_demod(trx_demod_t demod)
{
    bool am = (demod == TRX_DEMOD_AM);

    fd68xx_set_demod(demod);
    trx_spi_write(0x42, am ? 0x6F5C : 0x6F5A);
    trx_spi_write(0x2A, 0x6600);
    trx_spi_write(REG_AGC_TOP, 0x0210);
    trx_spi_write(REG_AGC_THR, 0x2A32);
}

// The bandwidth words of the FD6818B (0x43): the DM-36 stock firmware's and the FD6818B's. The shared
// fd68xx_set_bandwidth() writes the FD6818's words (0x4008 for 12.5 kHz, 0x3658 for SSB, 0x4048 for DMR): a
// different chip's. The DMR word is the stock's 0x0048 (the same filter byte as the set-up B's 0x3048).
static void set_bandwidth(trx_bandwidth_t bandwidth)
{
    static const uint16_t values[] = {
        [TRX_BANDWIDTH_25] = BANDWIDTH_25,
        [TRX_BANDWIDTH_12_5] = BANDWIDTH_12_5,
        [TRX_BANDWIDTH_6_25] = BANDWIDTH_6_25,
        [TRX_BANDWIDTH_DMR] = BANDWIDTH_DMR,
    };

    trx_spi_write(0x43, values[bandwidth]);
}

// The mic gain (0x7D): the FD6818B's encoding. The steps are the stock DM-36's own table, read from its firmware
// (0x03079468: 0x0B, 0x0C, 0x0D, 0x0F, 0x11 - 11/12/13/15/17 above the 0xE900 base), NOT the FD6818's
// 22/24/26/28/30 nor the set-up B values 31/34/38/41/44 that were used here: those are some 4 dB hotter, which is
// why FM transmit came out far too loud. The setting (0..31) picks one of the five, as on the stock (26 -> the middle).
static void set_mic_gain(uint8_t gain)
{
    static const uint16_t steps[] = {11, 12, 13, 15, 17};
    uint8_t index = (gain >= 30) ? 4 : (gain >= 28) ? 3 : (gain >= 26) ? 2 : (gain >= 24) ? 1 : 0;

    trx_spi_write(0x7D, (uint16_t)(MIC_GAIN_BASE | steps[index]));
}

// The chip picks its band itself (as the FD6818B): the calibration byte in 0x40, no 0x1A
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

// Straight between receive and transmit, through idle only for another frequency (through idle each time, the
// first burst received after each one sent comes back about 400 Hz off)
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

// Two words by the receive tone: CTCSS, else (DCS or none) the power on 0x2F (the DM-36 stock firmware writes
// them in pairs when it switches the audio mode)
static void set_rx_tone(uint16_t tone)
{
    bool ctcss = (tone != CODEPLUG_CSS_NONE) && !(tone & TONES_DCS);

    trx_spi_write(0x2E, ctcss ? 0x8608 : 0x9608);
    trx_spi_write(0x2F, ctcss ? 0x1890 : 0x9890);
    fd68xx_set_rx_tone(tone);
}

trx_driver_t trx_dm36 = {
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
    .set_bandwidth = set_bandwidth,

    .idle = fd68xx_idle,
    .set_band = set_band,
    .set_frequency = fd68xx_set_frequency,
    .rx_on = rx_on,
    .tune = fd68xx_tune, // CONTROL_STEP only from idle (control_on()); tune doesn't go through idle
    .transmit_prepare = transmit_prepare,
    .tx_on = tx_on,
    .set_mic_gain = set_mic_gain,

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
