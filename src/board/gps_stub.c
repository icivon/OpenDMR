/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * DM36 adaptation by BG2LNY
 * Redistribution and use: see LICENSE (BSD-style, non-commercial without written permission).
 *
 * No-GPS board stub (DM-36). The Makefile compiles this instead of
 * services/gps.c when BOARD_HAS_GPS is 0. Keeping it here leaves the GPS
 * service, its screens and all its callers untouched, so the DM-32 build and
 * upstream merges are unaffected: the GPS menu simply reports "none/off" and
 * no UART1 or GPS power pin is ever touched.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "gps.h"

void gps_start(void)
{
}

void gps_set_enabled(bool on)
{
    (void)on;
}

void gps_read(gps_data_t *data)
{
    memset(data, 0, sizeof(*data));
}

bool gps_has_fix(void)
{
    return false;
}

bool gps_position(int32_t *latitude, int32_t *longitude)
{
    if (latitude != NULL) {
        *latitude = 0;
    }
    if (longitude != NULL) {
        *longitude = 0;
    }
    return false;
}
