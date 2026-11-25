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

#endif
