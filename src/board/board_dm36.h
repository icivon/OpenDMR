/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * Redistribution and use: see LICENSE (BSD-style, non-commercial without written permission).
 *
 * DM-36 board map. Same screen (240x320 ST7789 / I8080), same HR_C7000 SoC,
 * same FD6818B RF chip, same external flashes as the DM-32UV. No GPS, no
 * separate P1/P2 keys (SK1/SK2 are the side keys in this firmware).
 *
 * Start from the DM-32 map and override only what is known to differ. Entries
 * marked TODO(DM36) still carry the DM-32 value: they are the first things to
 * fill in once the radio is on the bench. Keeping them here (instead of in the
 * shared sources) means the DM-32 build is untouched and upstream merges stay
 * clean.
 *
 * How to measure, see docs/dm36-porting.md P3.
 */

#ifndef BOARD_DM36_H
#define BOARD_DM36_H

#include "board_dm32.h"

/* ---------------- model identity ---------------- */
#undef  BOARD_NAME
#define BOARD_NAME            "DM-36"
// TODO(DM36): the CPS radio type reported by the stock firmware; 11 is DM-32.
//#undef  BOARD_CPS_RADIO_TYPE
//#define BOARD_CPS_RADIO_TYPE  11

/* ---------------- features ---------------- */
#undef  BOARD_HAS_GPS
#define BOARD_HAS_GPS         0

/*
 * TODO(DM36) - measured values go here. Each group was identified by static
 * analysis as DM-32-specific; any of them can differ on the DM-36.
 *
 *   Keypad    BOARD_PIN_KEY_1/3/4, BOARD_PIN_PTT, BOARD_KEY_ROWS/COLUMNS,
 *             BOARD_ROWS_AS_GPIO, BOARD_PIN_KEYPAD_ROW0/4
 *             -> read GPIO_EXT while holding PTT / SK1 / SK2 (dm32dbg.py mr).
 *
 *   Mux       BOARD_IO_DIPLEX0/1/2, BOARD_GPIO?_DDR/DR, BOARD_IOMGR_REN?
 *             -> copied verbatim from the DM-32 stock GPIO_Init(); recheck if
 *                the keypad rows are no longer LCD_DB0-DB4.
 *
 *   RF        BOARD_PIN_FD6818_SCL/SDA/SCN, BOARD_PIN_PA_* / BOARD_PIN_LNA_*
 *             -> no FD6818 register response means the soft-SPI pins moved.
 *
 *   Cal       src/services/factory_cal.c SEARCH_* / field offsets (not a pin)
 *             -> see docs/dm36-porting.md P1, from the DM-36 flash dump.
 */

#endif
