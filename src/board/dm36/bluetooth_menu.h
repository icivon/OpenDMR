/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * DM36 adaptation by BG2LNY
 *
 * DM-36 only: Bluetooth menu strings and screens.
 *
 * These text ids are deliberately outside the generated text_en[] table: DM-32 must not gain a single byte
 * from the Bluetooth menu. text() routes ids >= TEXT_COUNT to bluetooth_menu_text() only on DM-36.
 */

#ifndef DM36_BLUETOOTH_MENU_H
#define DM36_BLUETOOTH_MENU_H

#include "text.h"
#include "ui.h"

#define BT_TEXT_MENU      ((text_id_t)(TEXT_COUNT + 1))
#define BT_TEXT_SWITCH    ((text_id_t)(TEXT_COUNT + 2))
#define BT_TEXT_MODE      ((text_id_t)(TEXT_COUNT + 3))
#define BT_TEXT_NAME      ((text_id_t)(TEXT_COUNT + 4))
#define BT_TEXT_ADDRESS   ((text_id_t)(TEXT_COUNT + 5))
#define BT_TEXT_DEVICES   ((text_id_t)(TEXT_COUNT + 6))
#define BT_TEXT_INFO      ((text_id_t)(TEXT_COUNT + 7))
#define BT_TEXT_ACTIVE    ((text_id_t)(TEXT_COUNT + 8))
#define BT_TEXT_PASSIVE   ((text_id_t)(TEXT_COUNT + 9))
#define BT_TEXT_VERSION   ((text_id_t)(TEXT_COUNT + 10))
#define BT_TEXT_LINK      ((text_id_t)(TEXT_COUNT + 11))
#define BT_TEXT_LOCAL_NAME ((text_id_t)(TEXT_COUNT + 12))
#define BT_TEXT_LOCAL_ADDR ((text_id_t)(TEXT_COUNT + 13))
#define BT_TEXT_LAST_EVENT ((text_id_t)(TEXT_COUNT + 14))
#define BT_TEXT_CONNECTED ((text_id_t)(TEXT_COUNT + 15))
#define BT_TEXT_IDLE      ((text_id_t)(TEXT_COUNT + 16))
#define BT_TEXT_NO_DEVICES ((text_id_t)(TEXT_COUNT + 17))
#define BT_TEXT_SAVING    ((text_id_t)(TEXT_COUNT + 18))

const char *bluetooth_menu_text(text_id_t id);
extern const ui_screen_t bluetooth_menu_screen;

#endif
