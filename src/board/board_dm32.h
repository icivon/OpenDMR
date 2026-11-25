/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * Redistribution and use: see LICENSE (BSD-style, non-commercial without written permission).
 *
 * DM-32UV board map. These are the values that were previously hard-coded in
 * hw.h, gpio.c, keys.c, rotary.c, leds.c, fd6818.c, radio_internal.h, gps.c,
 * rda5807.c, battery.c, firmware_info.c and cable_link.c.
 *
 * Keep this file a straight transcription of the original literals: the DM-32
 * build must be behaviourally identical to the build before the board layer
 * existed. New boards should include this file and override only what differs.
 */

#ifndef BOARD_DM32_H
#define BOARD_DM32_H

/* ---------------- model identity ---------------- */
#define BOARD_NAME            "DM-32UV"
#define BOARD_CPS_RADIO_TYPE  11  // what the OpenDM32 releases report (cable_link.c)
#define BOARD_HAS_GPS         1

/* ---------------- GPIO pin mux / directions (hal/gpio.c pins_init) ---------------- */
#define BOARD_IO_DIPLEX0      0xFFFDFD80UL
#define BOARD_IO_DIPLEX1      0xFFFFFE07UL
#define BOARD_IO_DIPLEX2      0x0FC80100UL
#define BOARD_GPIOA_DDR       0x070604E0UL
#define BOARD_GPIOB_DDR       0x00A0D01FUL
#define BOARD_GPIOC_DDR       0x00007FC4UL
#define BOARD_GPIOA_DR_SET    0x0060UL
#define BOARD_GPIOB_DR_SET    0x0000C005UL
#define BOARD_GPIOC_DR_SET    0x7044UL
#define BOARD_IOMGR_REN0      0xFFFBFFEFUL
#define BOARD_IOMGR_REN1      0x00FFFFFFUL
#define BOARD_IOMGR_REN2      0x0FFFFF70UL
#define BOARD_IO_DIPLEX1_EXTRA 0x38000000UL // pads 29/30/31 pulls, after the stock GPIO_Init()
#define BOARD_REN0_CLEAR_BIT  24
#define BOARD_REN2_EXTRA      0x0C000000UL

/* ---------------- pins used by hw.h ---------------- */
#define BOARD_PIN_LCD_BKLIGHT 0x0F
#define BOARD_PIN_PWR_SWCHECK 0x10
#define BOARD_PIN_PWR_SW      0x11 // held high: the radio stays on
#define BOARD_PIN_SPI0_CS     0x12
#define BOARD_PIN_KEY_BKLIGHT 0x28
#define BOARD_PIN_MIC_MUT     0x2E
#define BOARD_PIN_LCD_NRESET  0x42
#define BOARD_PIN_LCD_RD      0x46
#define BOARD_PIN_KEYPAD_ROW0 71
#define BOARD_PIN_KEYPAD_ROW4 75

/* ---------------- keypad / side buttons (drivers/keys.c) ---------------- */
#define BOARD_PIN_KEY_1       49  // column 0
#define BOARD_PIN_KEY_3       51  // column 2, also SK1
#define BOARD_PIN_KEY_4       52  // column 3, also SK2
#define BOARD_PIN_PTT         43
#define BOARD_KEY_ROWS        5
#define BOARD_KEY_COLUMNS     4
#define BOARD_ROWS_AS_GPIO    0xFE00UL // IO_DIPLEX2: LCD_DB0 - DB4 as GPIOs
// Matrix position -> logical key bit (keys.h). The DM-32 wiring is the identity mapping.
#define BOARD_KEY_INDEX(index) (index)

/* ---------------- rotary / LEDs (drivers/rotary.c, leds.c) ---------------- */
#define BOARD_PIN_ROTARY_A    0x27
#define BOARD_PIN_ROTARY_B    0x26
#define BOARD_PIN_LED_GREEN   32
#define BOARD_PIN_LED_RED     33

/* ---------------- FD6818 soft SPI (drivers/fd6818.c) ---------------- */
#define BOARD_PIN_FD6818_SCL  0x07
#define BOARD_PIN_FD6818_SDA  0x08
#define BOARD_PIN_FD6818_SCN  0x18
#define BOARD_PIN_FD6818_GP0  0x3B
#define BOARD_PIN_FD6818_GP1  0x3A

/* ---------------- RF switches (services/radio_internal.h) ---------------- */
#define BOARD_PIN_LNA_VHF     0x22 // R5_V_SW
#define BOARD_PIN_LNA_UHF     0x24 // R5_U_SW
#define BOARD_PIN_PA_VHF      0x37
#define BOARD_PIN_PA_UHF      0x23
#define BOARD_PIN_AUDIO_SW    0x2F // "PWM/FD6818"
#define BOARD_PIN_AUDIO_AMP   0x35
#define BOARD_UHF_FROM        28000000UL // 10 Hz
#define BOARD_B220_FROM       20000000UL

/* ---------------- GPS (services/gps.c) ---------------- */
#define BOARD_PIN_GPS_POWER   0x2C // low = on

/* ---------------- FM broadcast receiver (drivers/rda5807.c) ---------------- */
#define BOARD_PIN_FM_SDIO     0x19
#define BOARD_PIN_FM_SCLK     0x1A

/* ---------------- battery / ADC (services/battery.c) ---------------- */
#define BOARD_ADC_PER_VOLT_X100 82 // ADC counts per 0.1 V, x 10
#define BOARD_BATTERY_FULL_X10  82 // 8.2 V
#define BOARD_BATTERY_EMPTY_X10 64 // 6.4 V
#define BOARD_BATTERY_LOW_X10   62

#endif
