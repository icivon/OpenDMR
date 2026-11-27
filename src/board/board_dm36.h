/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * Redistribution and use: see LICENSE (BSD-style, non-commercial without written permission).
 *
 * DM-36 board map. Same screen (240x320 ST7789 / I8080), same HR_C7000 SoC
 * and external flashes as the DM-32UV. RF is FD6818B (BK4829, ID 0x4829);
 * DM-32UV is FD6818 (BK4819, ID 0x4819). No GPS, no separate P1/P2 keys
 * (SK1/SK2 are the side keys in this firmware).
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
#include "dm36/bluetooth.h"
#include "keys.h"
#include "trx_driver.h"

/* ---------------- keypad wiring (measured on real hardware, 2026-11-25) ----------------
 * The DM-36 matrix is wired differently: the physical keys sit at other row/column
 * positions than on the DM-32. Translate each measured position to the logical key
 * bit (keys.h / ui.h). Positions not listed keep the DM-32 mapping.
 *
 *   matrix          physical key     logical key
 *     0  (r0c0)     *                KEYS_STAR       (15)
 *     4  (r1c0)     0                KEYS_0          (14)
 *     8  (r2c0)     #                KEYS_HASH       (13)  (was RIGHT; DM36 has no right key)
 *    12  (r3c0)     Menu             KEYS_GREEN      (0)   menu / OK
 *    13  (r3c1)     Up               KEYS_FRONT_UP   (16)
 *    14  (r3c2)     Down             KEYS_FRONT_DOWN (17)
 *    15  (r3c3)     Back             KEYS_RED        (12)
 *
 * 2026-11-26: the physical # key and the number keys are all reported at matrix
 * position 8 on this board (the key-settings screen shows RIGHT for them).
 * Position 8 is mapped to HASH here so the # key works; the number keys still
 * need their own positions measured before they can be mapped correctly.
 */
#undef  BOARD_KEY_INDEX
#define BOARD_KEY_INDEX(index) dm36_key_index(index)
static inline int dm36_key_index(int index)
{
    switch (index) {
    case 0:  return KEYS_STAR;
    case 4:  return KEYS_0;
    case 8:  return KEYS_HASH;
    case 12: return KEYS_GREEN;
    case 13: return KEYS_FRONT_UP;
    case 14: return KEYS_FRONT_DOWN;
    case 15: return KEYS_RED;
    default: return index;
    }
}

/* DM-36 has no LEFT / RIGHT keys. On list-like screens * and # add the DM-32
 * LEFT / RIGHT "change the selected value" roles. Other screens keep their own
 * * / # functions; shared files only apply this hook where DM-32 used LEFT /
 * RIGHT to change a value. */
#define BOARD_IS_EXTRA_VALUE_CHANGE_KEY(key) \
    (((key) == KEYS_STAR) || ((key) == KEYS_HASH))
#define BOARD_EXTRA_VALUE_CHANGE_STEP(key) (((key) == KEYS_HASH) ? 1 : -1)

/* Stock 0x3011cc8/0x301243c: rows 71-75 are driven as GPIO by IO_DIPLEX2
 * bit[15:9] (0xFE00) and columns are GPIOB_EXT bit[20:17] (pins 49-52). */
#undef  BOARD_PIN_KEYPAD_ROW0
#define BOARD_PIN_KEYPAD_ROW0 71
#undef  BOARD_PIN_KEYPAD_ROW4
#define BOARD_PIN_KEYPAD_ROW4 75
#undef  BOARD_KEY_ROWS
#define BOARD_KEY_ROWS        5
#undef  BOARD_KEY_COLUMNS
#define BOARD_KEY_COLUMNS     4
#undef  BOARD_ROWS_AS_GPIO
#define BOARD_ROWS_AS_GPIO    0xFE00UL

/* Side buttons, measured on real hardware 2026-11-26 with the raw GPIO_EXT scan
 * (tools/debug/dm36_keyscan.py, one key at a time):
 *   SK1 = pin 37 (0x25), SK2 = pin 41 (0x29).
 * They are NOT keypad columns here (the columns are pins 49-52), so read them
 * directly instead of through the DM-32 column offset. PTT stays at pin 43:
 * the stock firmware analysis initializes 43, see docs/dm36-stock-firmware-analysis.md.
 */
#undef  BOARD_PIN_SK1
#define BOARD_PIN_SK1         37
#undef  BOARD_PIN_SK2
#define BOARD_PIN_SK2         41

/* ---------------- model identity ---------------- */
#undef  BOARD_NAME
#define BOARD_NAME            "DM-36"
// TODO(DM36): the CPS radio type reported by the stock firmware; 11 is DM-32.
//#undef  BOARD_CPS_RADIO_TYPE
//#define BOARD_CPS_RADIO_TYPE  11

/* ---------------- features ---------------- */
#undef  BOARD_HAS_GPS
#define BOARD_HAS_GPS         0

/* ---------------- the transceiver chip ----------------
 * The DM-36's RF is an FD6818B (BK4829) with its own register values (the DM-36 stock firmware's, see
 * docs/dm36-stock-fd6818-writes.md). Its driver is board/dm36/trx_dm36.c, on the upstream transceiver driver
 * layer (drivers/trx_driver.h): upstream's driver list (drivers/trx.c) is replaced by it, so the DM-36 always
 * uses its own values, whatever the chip's ID reads (the stock firmware never reads the ID either). */
extern trx_driver_t trx_dm36;
#define BOARD_TRX_DRIVER (&trx_dm36)

/* DM-36 audio quirks: the FD6818B AF output shares the speaker node with the
 * baseband beep path; mute it while a prompt plays. Its amplifier also starts
 * slower than the DM-32's.
 *
 * In the digital modes the AF output carries the discriminator to the baseband
 * (0x47 = 0x6943): it must not be written there, a beep would mute the
 * digital receive (the audio out of the FM value) until the next burst. */
#define BOARD_BEEP_PATH_AF_OUT(on, carrier) \
    do { if (is_analog()) { trx_af_out(!(on) && (carrier)); } } while (0)
#define BOARD_AMP_START_MS      120

/* CPS replies are mirrored to the LytLink UART2 link once it is up. */
#undef  BOARD_CPS_REPLY_OUT
#define BOARD_CPS_REPLY_OUT(data, length) \
    do { if (bluetooth_connected()) bluetooth_write_data((data), (length)); } while (0)

/*
 * TODO(DM36) - measured values go here. Each group was identified by static
 * analysis as DM-32-specific; any of them can differ on the DM-36.
 *
 *   Keypad    BOARD_PIN_KEY_1/3/4, BOARD_PIN_PTT
 *             -> rows/columns/mask were recovered from the stock scanner above;
 *                the side-button GPIO assignment is still handled separately.
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
