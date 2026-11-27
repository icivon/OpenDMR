/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * Redistribution and use: see LICENSE (BSD-style, non-commercial without written permission).
 *
 * Board selection. BUILD=dm36 selects the DM-36 map; the default is DM-32UV.
 * All board-specific values live in this directory; no other source file needs
 * a per-board #if. Including hw.h pulls this in automatically.
 */

#ifndef BOARD_H
#define BOARD_H

#if defined(BOARD_DM36)
#include "board_dm36.h"
#else
#include "board_dm32.h"
#endif

/* Board hooks with DM-32 defaults. DM-36 overrides them in board_dm36.h, so
 * the shared .c files need only a one-line call and upstream merges stay small. */
#ifndef BOARD_BEEP_PATH_AF_OUT
#define BOARD_BEEP_PATH_AF_OUT(on, carrier) ((void)0)
#endif
#ifndef BOARD_AMP_START_MS
#define BOARD_AMP_START_MS 75
#endif
#ifndef BOARD_CPS_REPLY_OUT
#define BOARD_CPS_REPLY_OUT(data, length) ((void)0)
#endif

/* Boards without LEFT / RIGHT may add their own keys for changing a value on
 * list-like screens. The default keeps the DM-32 code path byte-for-byte. */
#ifndef BOARD_IS_EXTRA_VALUE_CHANGE_KEY
#define BOARD_IS_EXTRA_VALUE_CHANGE_KEY(key) (0)
#define BOARD_EXTRA_VALUE_CHANGE_STEP(key)   (0)
#endif

#endif
